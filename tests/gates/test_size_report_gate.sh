#!/usr/bin/env bash
# test_size_report_gate.sh — negative controls for the size report's build
# gate (tools/ci/size_report.py --require-builds; the CI lane size-report;
# bd-9kll.13.3).
#
# The lane exists because no other CI job builds BUILD_TYPE=release: the
# release library did not compile for hours and nothing noticed. If the gate
# stopped failing on a failed build, the lane would report an ERROR row and
# pass. These controls run the real script with a `make` that fails, put
# first on PATH, so nothing is built:
#   - a library build that fails, --require-builds   -> FAILS, naming the row
#   - the same run without --require-builds          -> exits 0 (observe-only)
#   - the same run with --strict                     -> FAILS (the stricter gate)
# and the report's own code, which those cases never reach (a failing make
# produces an ERROR row, no sizes, so text_report's size paths did not run;
# on the first real run it crashed with `'int' object is not subscriptable`,
# a per-object list that the archive totals had overwritten with a count):
#   - --selftest on the script                       -> PASSES
#   - --selftest on a copy with that bug put back    -> FAILS (the negative
#     control: the selftest would have caught it)
#   - --render of a recorded report.json             -> prints the report
#
# Scratch files live under build/test-gates/<run_id>/; tracked files are never
# touched. Logs JSONL to build/test-logs/gates.jsonl. Nothing is deleted.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
RUN_ID="size-report-gate-$(date -u +%Y%m%dT%H%M%SZ)-$$"
WORK="$REPO_ROOT/build/test-gates/$RUN_ID"
LOG_DIR="$REPO_ROOT/build/test-logs"
LOG="$LOG_DIR/gates.jsonl"
mkdir -p "$WORK/bin" "$LOG_DIR"

failures=0

# A make that fails the way a broken release build does.
cat >"$WORK/bin/make" <<'EOF'
#!/usr/bin/env bash
echo "src/net/net.c:186:9: error: writing 4 bytes into a region of size 0 [-Werror=stringop-overflow=]" >&2
echo "make: *** [obj/net/net.o] Error 1" >&2
exit 2
EOF
chmod +x "$WORK/bin/make"

log_case() {
    # case expected observed status detail
    printf '{"ts":"%s","run_id":"%s","layer":"gates","suite":"size_report_gate","test":"%s","expected":"%s","observed":"%s","status":"%s","detail":"%s"}\n' \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$RUN_ID" "$1" "$2" "$3" "$4" "$5" >>"$LOG"
}

# run_case <name> <expected exit: 0|nonzero> <expected output regex> <script args...>
run_case() {
    local name="$1" expect="$2" pattern="$3"
    shift 3
    local out rc observed status
    out="$(PATH="$WORK/bin:$PATH" python3 "${SIZE_REPORT_PY:-$REPO_ROOT/tools/ci/size_report.py}" \
        --out "$WORK/$name" --profiles CORE --classes default --types release \
        --jobs 1 "$@" 2>&1)"
    rc=$?
    printf '%s\n' "$out" >"$WORK/$name.log"

    if [ "$rc" -eq 0 ]; then observed="exit0"; else observed="exit$rc"; fi
    status="pass"
    if [ "$expect" = "0" ] && [ "$rc" -ne 0 ]; then status="fail"; fi
    if [ "$expect" = "nonzero" ] && [ "$rc" -eq 0 ]; then status="fail"; fi
    if ! grep -Eq -- "$pattern" <<<"$out"; then status="fail"; fi

    if [ "$status" = "pass" ]; then
        echo "  PASS: $name ($observed)"
    else
        echo "  FAIL: $name ($observed; expected $expect matching /$pattern/; log $WORK/$name.log)"
        failures=$((failures + 1))
    fi
    log_case "$name" "$expect:$pattern" "$observed" "$status" "$WORK/$name.log"
}

echo "=== test_size_report_gate ($RUN_ID) ==="

run_case failed_build_fails_the_build_gate nonzero 'FAIL \(builds\): 1 defect' --require-builds
run_case failed_build_names_its_row nonzero '  CORE-default-release: full library build failed' \
    --require-builds
run_case failed_build_is_observed_without_the_gate 0 \
    'CORE-default-release +ERROR full library build failed'
run_case failed_build_fails_strict nonzero 'FAIL \(strict\): 1 build error' --strict

# The report's own code, without a build.
run_case selftest_passes 0 'size_report selftest: PASS' --selftest

# The original bug put back: the archive totals' object count under the key
# that holds the per-object list. Written to scratch; the tracked script is
# only read. The copy's ROOT is wrong and the selftest does not use it.
sed 's/"bss": sum(r\["bss"\] for r in rows), "object_count": len(rows)}/"bss": sum(r["bss"] for r in rows), "objects": len(rows)}/' \
    "$REPO_ROOT/tools/ci/size_report.py" >"$WORK/size_report_mutant.py"
if cmp -s "$REPO_ROOT/tools/ci/size_report.py" "$WORK/size_report_mutant.py"; then
    echo "  FAIL: selftest_mutant_is_a_mutant (the sed did not change the script)"
    failures=$((failures + 1))
else
    SIZE_REPORT_PY="$WORK/size_report_mutant.py" \
        run_case selftest_fails_when_the_list_is_overwritten nonzero \
        'size_report selftest: FAIL' --selftest
fi

# --render of a recorded report: the selftest's synthetic report, written
# out and read back by the script itself.
python3 -B - "$REPO_ROOT/tools/ci" "$WORK/recorded.json" <<'EOF'
import json, sys
sys.path.insert(0, sys.argv[1])
import size_report
with open(sys.argv[2], "w", encoding="utf-8") as fh:
    json.dump(size_report.synthetic_report(), fh)
EOF
run_case render_prints_a_recorded_report 0 \
    'largest kernel objects by bss, KiB \(top 20 of every row' --render "$WORK/recorded.json"

echo ""
if [ "$failures" -eq 0 ]; then
    echo "test_size_report_gate: PASS"
    exit 0
fi
echo "test_size_report_gate: FAIL ($failures case(s))"
exit 1
