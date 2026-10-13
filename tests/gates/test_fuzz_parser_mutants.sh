#!/usr/bin/env bash
# test_fuzz_parser_mutants.sh — every parser fuzz harness must find a planted
# bug (bd-9kll.16.7).
#
# fuzz-parsers-smoke is green when no input crashes a parser. It is also green
# when a harness never reaches its parser or the sanitizers are not looking, so
# green alone proves nothing about a harness. Each of the harnesses in
# tests/fuzz/fuzz_parsers.c therefore has one realistic off-by-one planted in
# its parser behind ASX_MUTANT_<TARGET> (grep the macro in src/). This gate
# builds each harness against its own mutant library, with the same recipe
# `make fuzz-parsers` uses, and runs it from the committed seeds for a bounded
# time. It requires:
#   - the mutant harness to crash: a nonzero exit, a libFuzzer crash artifact,
#     and the report to name the plant (a sanitizer error whose stack is in the
#     planted source file, or the harness's own property message);
#   - the unmutated harnesses, built and run the same way for the same time,
#     to stay clean.
# A harness that does not find its plant in the bounded run is the finding.
# The plant is not loosened to make it findable; the seeds or the harness are
# fixed. The log records, per target, whether the crash came from the corpus
# (before libFuzzer's INITED line) or from mutation.
#
# Needs clang with the libFuzzer runtime (libclang-rt-dev). Without it the
# gate SKIPs, and fails under STRICT_GATES=1 / CI like every other tool gate.
#
# Environment:
#   ASX_FUZZ_MUTANT_TARGETS    space-separated subset (default: all twelve)
#   ASX_FUZZ_MUTANT_SECONDS    bounded run per mutant (default 30; ends at the crash)
#   ASX_FUZZ_CLEAN_SECONDS     bounded run per clean harness (default 5)
#   ASX_MUTANT_BUILD_DIR       build root (default build/mutants)
#   FUZZ_PARSERS_CC            compiler (default clang)
#   JOBS                       make parallelism (default 4)
#
# Builds under $ASX_MUTANT_BUILD_DIR/fuzz-parsers/ and logs JSONL to
# build/test-logs/gates.jsonl. Nothing is deleted.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
RUN_ID="fuzz-mutants-$(date -u +%Y%m%dT%H%M%SZ)-$$"
LOG_DIR="$REPO_ROOT/build/test-logs"
LOG="$LOG_DIR/gates.jsonl"
JOBS="${JOBS:-4}"
CC_FUZZ="${FUZZ_PARSERS_CC:-clang}"
MUTANT_SECONDS="${ASX_FUZZ_MUTANT_SECONDS:-30}"
CLEAN_SECONDS="${ASX_FUZZ_CLEAN_SECONDS:-5}"
STRICT="${STRICT_GATES:-${CI:-0}}"
BUILD_ROOT="${ASX_MUTANT_BUILD_DIR:-build/mutants}"
case "$BUILD_ROOT" in
/*) ABS_ROOT="$BUILD_ROOT/fuzz-parsers" ;;
*) ABS_ROOT="$REPO_ROOT/$BUILD_ROOT/fuzz-parsers" ;;
esac
SEEDS="$REPO_ROOT/tests/fuzz/parsers_seeds"
mkdir -p "$LOG_DIR" "$ABS_ROOT"

ALL_TARGETS="HTTP_REQUEST HTTP_RESPONSE WEBSOCKET BASE64 CODEC_JSON CODEC_BIN WEB_QUERY RAPTORQ_OTI RAPTORQ_DECODE FRAME_CODECS RESOLVE_HOST JSON_READER"
TARGETS="${ASX_FUZZ_MUTANT_TARGETS:-$ALL_TARGETS}"

failures=0

log_case() {
    # case expected observed status detail
    printf '{"ts":"%s","run_id":"%s","layer":"gates","suite":"fuzz_parser_mutants","test":"%s","expected":"%s","observed":"%s","status":"%s","detail":"%s"}\n' \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$RUN_ID" "$1" "$2" "$3" "$4" "$5" >>"$LOG"
}

# expect_for TARGET: what the report of its planted bug must say. E_KIND is
# "sanitizer" (a sanitizer error whose stack names E_FILE) or "property" (the
# harness's own fuzz_fail message, E_TEXT).
expect_for() {
    E_KIND=sanitizer
    E_TEXT=""
    case "$1" in
    HTTP_REQUEST)
        E_KIND=property
        E_FILE=src/net/http.c
        E_TEXT="http parser consumed more than it was given"
        ;;
    HTTP_RESPONSE)
        E_KIND=property
        E_FILE=src/net/http.c
        E_TEXT="http parser consumed more than it was given"
        ;;
    WEBSOCKET) E_FILE=src/net/websocket.c ;;
    BASE64) E_FILE=src/security/crypto.c ;;
    CODEC_JSON) E_FILE=src/runtime/hooks.c ;;
    CODEC_BIN) E_FILE=src/runtime/hooks.c ;;
    WEB_QUERY) E_FILE=src/net/web.c ;;
    RAPTORQ_OTI) E_FILE=src/raptorq/raptorq.c ;;
    RAPTORQ_DECODE)
        E_KIND=property
        E_FILE=src/raptorq/raptorq.c
        E_TEXT="raptorq decoded block is not the source"
        ;;
    FRAME_CODECS) E_FILE=src/bytes/codec.c ;;
    RESOLVE_HOST) E_FILE=src/net/net.c ;;
    JSON_READER) E_FILE=src/conformance/json.c ;;
    *) return 1 ;;
    esac
    return 0
}

# build_harnesses DIR EXTRA_CFLAGS TARGET...: `make fuzz-parsers` into DIR with
# only TARGET... as harnesses and EXTRA_CFLAGS on the instrumented library.
build_harnesses() {
    local dir="$1" extra="$2"
    shift 2
    mkdir -p "$ABS_ROOT"
    make -C "$REPO_ROOT" -j"$JOBS" --no-print-directory fuzz-parsers \
        FUZZ_PARSERS_DIR="$dir" FUZZ_PARSERS_CC="$CC_FUZZ" \
        FUZZ_PARSER_TARGETS="$*" FUZZ_PARSERS_EXTRA_CFLAGS="$extra" \
        >"$ABS_ROOT/build-$(basename "$dir").log" 2>&1
}

# run_harness DIR TARGET SECONDS: libFuzzer from the committed seeds, in a
# fresh run directory (so a stale crash file cannot be mistaken for a find).
# Sets H_RC, H_LOG, H_RUN, H_PHASE (corpus|mutation|none) and H_ARTIFACTS.
run_harness() {
    local dir="$1" t="$2" secs="$3"
    H_RUN="$dir/run-$RUN_ID-$t"
    H_LOG="$H_RUN/log.txt"
    mkdir -p "$H_RUN/corpus"
    cp "$SEEDS/$t"/* "$H_RUN/corpus/"
    (cd "$H_RUN" && "$dir/bin/$t" -max_total_time="$secs" -timeout=10 -rss_limit_mb=2048 \
        -seed=1 corpus >"$H_LOG" 2>&1)
    H_RC=$?
    H_ARTIFACTS="$(find "$H_RUN" -maxdepth 1 \( -name 'crash-*' -o -name 'timeout-*' -o -name 'oom-*' \) | wc -l | tr -d ' ')"
    if grep -q 'INITED' "$H_LOG"; then
        H_PHASE=mutation
    elif [ "$H_ARTIFACTS" -gt 0 ]; then
        H_PHASE=corpus
    else
        H_PHASE=none
    fi
}

echo "=== test_fuzz_parser_mutants ($RUN_ID) ==="

# --- Tooling ------------------------------------------------------------------
cat >"$ABS_ROOT/probe.c" <<'EOF'
int LLVMFuzzerTestOneInput(const unsigned char *d, unsigned long n) {
    (void)d;
    (void)n;
    return 0;
}
EOF
if ! command -v "$CC_FUZZ" >/dev/null 2>&1 ||
    ! "$CC_FUZZ" -fsanitize=fuzzer,address,undefined -o "$ABS_ROOT/probe" "$ABS_ROOT/probe.c" \
        >"$ABS_ROOT/probe.log" 2>&1; then
    if [ "$STRICT" = "1" ] || [ "$STRICT" = "true" ]; then
        echo "  FAIL: $CC_FUZZ with the libFuzzer runtime is unavailable (STRICT_GATES/CI); log $ABS_ROOT/probe.log"
        log_case "tooling" "libfuzzer-available" "missing" "fail" "$ABS_ROOT/probe.log"
        exit 1
    fi
    echo "  SKIP: $CC_FUZZ with the libFuzzer runtime is unavailable (STRICT_GATES=1 makes this fail)"
    log_case "tooling" "libfuzzer-available" "missing" "skip" "$ABS_ROOT/probe.log"
    exit 0
fi

# --- The plants cover exactly the harnesses make builds -----------------------
# shellcheck disable=SC2016  # $(...) is make's, not the shell's
MAKE_TARGETS="$(make -s -C "$REPO_ROOT" --eval='.PHONY: print-fuzz-parser-targets
print-fuzz-parser-targets: ; @echo $(FUZZ_PARSER_TARGETS)' print-fuzz-parser-targets 2>/dev/null)"
sorted_make="$(printf '%s\n' "$MAKE_TARGETS" | tr ' ' '\n' | sort | tr '\n' ' ')"
sorted_all="$(printf '%s\n' "$ALL_TARGETS" | tr ' ' '\n' | sort | tr '\n' ' ')"
if [ "$sorted_make" != "$sorted_all" ]; then
    echo "  FAIL: FUZZ_PARSER_TARGETS ($sorted_make) differs from the gate's list ($sorted_all): a harness without a planted bug, or a plant without a harness"
    log_case "targets" "$sorted_all" "$sorted_make" "fail" "Makefile FUZZ_PARSER_TARGETS"
    failures=$((failures + 1))
fi
for t in $ALL_TARGETS; do
    if ! grep -rq "ASX_MUTANT_$t" "$REPO_ROOT/src"; then
        echo "  FAIL: no ASX_MUTANT_$t plant in src/"
        log_case "$t" "plant-present" "missing" "fail" "src/"
        failures=$((failures + 1))
    fi
done

# --- Unmutated harnesses stay clean -------------------------------------------
CLEAN_ABS="$ABS_ROOT/clean"
echo "  building the unmutated harnesses ($CLEAN_ABS)"
# shellcheck disable=SC2086
if ! build_harnesses "$CLEAN_ABS" "" $ALL_TARGETS; then
    echo "  FAIL: the unmutated harnesses do not build (log $ABS_ROOT/build-clean.log)"
    log_case "clean" "build" "build-failed" "fail" "$ABS_ROOT/build-clean.log"
    failures=$((failures + 1))
else
    for t in $TARGETS; do
        expect_for "$t" || continue
        run_harness "$CLEAN_ABS" "$t" "$CLEAN_SECONDS"
        if [ "$H_RC" -eq 0 ] && [ "$H_ARTIFACTS" -eq 0 ] &&
            ! grep -Eq 'ERROR: AddressSanitizer|runtime error:|property violated' "$H_LOG"; then
            echo "  PASS: $t clean build ran $CLEAN_SECONDS s from its seeds without a finding"
            log_case "$t" "clean-stays-clean" "exit$H_RC" "pass" "$H_LOG"
        else
            echo "  FAIL: $t clean build reported a finding (exit$H_RC; log $H_LOG)"
            log_case "$t" "clean-stays-clean" "exit$H_RC artifacts=$H_ARTIFACTS" "fail" "$H_LOG"
            failures=$((failures + 1))
        fi
    done
fi

# --- Each harness finds its plant ---------------------------------------------
for t in $TARGETS; do
    if ! expect_for "$t"; then
        echo "  FAIL: $t is not a known parser fuzz harness"
        log_case "$t" "known-target" "unknown" "fail" "$0"
        failures=$((failures + 1))
        continue
    fi
    m_abs="$ABS_ROOT/$t"
    if ! build_harnesses "$m_abs" "-DASX_MUTANT_$t" "$t"; then
        echo "  FAIL: $t mutant harness does not build (log $ABS_ROOT/build-$t.log)"
        log_case "$t" "mutant-build" "build-failed" "fail" "$ABS_ROOT/build-$t.log"
        failures=$((failures + 1))
        continue
    fi
    start="$(date +%s)"
    run_harness "$m_abs" "$t" "$MUTANT_SECONDS"
    elapsed=$(($(date +%s) - start))
    found="no"
    if [ "$H_RC" -ne 0 ] && [ "$H_ARTIFACTS" -gt 0 ]; then
        if [ "$E_KIND" = "property" ]; then
            if grep -q "property violated: $E_TEXT" "$H_LOG"; then found="yes"; fi
        else
            if grep -Eq 'ERROR: AddressSanitizer|runtime error:' "$H_LOG" &&
                grep -q "$E_FILE" "$H_LOG"; then
                found="yes"
            fi
        fi
    fi
    if [ "$found" = "yes" ]; then
        echo "  PASS: $t found its planted bug in ${elapsed}s (from the $H_PHASE; $E_KIND in $E_FILE)"
        log_case "$t" "mutant-found" "exit$H_RC phase=$H_PHASE ${elapsed}s" "pass" "$H_LOG"
    else
        echo "  FAIL: $t did NOT find its planted bug in ${MUTANT_SECONDS}s (exit$H_RC; artifacts=$H_ARTIFACTS; expected $E_KIND in $E_FILE; log $H_LOG)"
        log_case "$t" "mutant-found" "exit$H_RC artifacts=$H_ARTIFACTS phase=$H_PHASE" "fail" "$H_LOG"
        failures=$((failures + 1))
    fi
done

if [ "$failures" -eq 0 ]; then
    echo "test_fuzz_parser_mutants: every parser harness found its planted bug, and the unmutated harnesses stayed clean"
    exit 0
fi
echo "test_fuzz_parser_mutants: $failures check(s) failed"
exit 1
