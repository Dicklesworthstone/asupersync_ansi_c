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
    out="$(PATH="$WORK/bin:$PATH" python3 "$REPO_ROOT/tools/ci/size_report.py" \
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

echo ""
if [ "$failures" -eq 0 ]; then
    echo "test_size_report_gate: PASS"
    exit 0
fi
echo "test_size_report_gate: FAIL ($failures case(s))"
exit 1
