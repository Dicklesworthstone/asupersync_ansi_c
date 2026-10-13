#!/usr/bin/env bash
# test_fuzz_parser_mutants.sh — a planted bug per parser fuzz harness
# (bd-9kll.16.7). Run by `make test-fuzz-parser-mutants` in the fuzz-parity CI
# job; `make test-gates` skips it (12 sanitizer library builds are too slow
# for every check).
#
# NO CLAIM: a green replay control proves wiring, not fuzz discovery. It shows
# that the harness calls that parser on that input under the sanitizers. It
# does not show that fuzzing would find a bug of that kind, and the discovery
# control below observes that, one planted bug at a time, without gating.
#
# fuzz-parsers-smoke is green when no input crashes a parser. It is also green
# when a harness never reaches its parser or the sanitizers are not looking, so
# green alone proves nothing about a harness. Each of the harnesses in
# tests/fuzz/fuzz_parsers.c therefore has one realistic off-by-one planted in
# its parser behind ASX_MUTANT_<TARGET> (grep the macro in src/). This script
# builds each harness against its own mutant library, with the same recipe
# `make fuzz-parsers` uses, and runs two controls.
#
# 1. Replay control (GATES: any failure exits nonzero). The harness runs from
#    the committed seeds for a bounded time. It must crash: a nonzero exit, a
#    libFuzzer crash artifact, and a report that names the plant (a sanitizer
#    error whose stack is in the planted source file, or the harness's own
#    property message). The unmutated harnesses, built and run the same way,
#    must stay clean. Four plants sit on a boundary that only the seed added
#    for it (tests/fuzz/parsers_seeds, see WITHHELD below) reaches directly, so
#    for those four a pass shows the corpus replay, not a discovery.
#
# 2. Discovery control (OBSERVES: never changes the exit status). For each of
#    those four harnesses the boundary seed is withheld (a corpus copy without
#    it) and the mutant harness runs with a fixed libFuzzer -seed and an
#    execution and time budget. The table at the end says `found after N
#    execs` or `NOT found within N execs`. A NOT-found row is a finding about
#    that harness's dictionary, mutators or corpus; it goes in the bead, it is
#    not hidden, and the plant is not loosened to make it findable.
#
# The replay row of every harness also records whether its crash came from the
# corpus (before libFuzzer's INITED line) or from mutation.
#
# Needs clang with the libFuzzer runtime (libclang-rt-dev). Without it the
# script SKIPs, and fails under STRICT_GATES=1 / CI like every other tool gate.
#
# Environment:
#   ASX_FUZZ_MUTANT_TARGETS      space-separated subset (default: all twelve)
#   ASX_FUZZ_MUTANT_SECONDS      replay control bound per mutant (default 30; ends at the crash)
#   ASX_FUZZ_CLEAN_SECONDS       bound per unmutated harness (default 5)
#   ASX_FUZZ_DISCOVERY           0 skips the discovery control (default 1)
#   ASX_FUZZ_DISCOVERY_RUNS      discovery execution budget (default 1000000)
#   ASX_FUZZ_DISCOVERY_SECONDS   discovery time budget, a backstop (default 60)
#   ASX_MUTANT_BUILD_DIR         build root (default build/mutants)
#   FUZZ_PARSERS_CC              compiler (default clang)
#   JOBS                         make parallelism (default 4)
#
# Builds under $ASX_MUTANT_BUILD_DIR/fuzz-parsers/, writes the table to
# $ASX_MUTANT_BUILD_DIR/fuzz-parsers/discovery.md and logs JSONL to
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
DISCOVERY="${ASX_FUZZ_DISCOVERY:-1}"
DISCOVERY_RUNS="${ASX_FUZZ_DISCOVERY_RUNS:-1000000}"
DISCOVERY_SECONDS="${ASX_FUZZ_DISCOVERY_SECONDS:-60}"
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

# The boundary seeds added for a plant (9f298a3): TARGET=seed file name. The
# discovery control withholds each from a corpus copy.
WITHHELD="WEBSOCKET=seed03 CODEC_BIN=seed03 FRAME_CODECS=seed04 RESOLVE_HOST=seed04"

failures=0
TABLE_ROWS=""

# Row of the results table: harness | replay | discovery. TABLE_ROWS is
# printed, and written to discovery.md, at the end.
table_row() {
    TABLE_ROWS="${TABLE_ROWS}| $1 | $2 | $3 |
"
}

# withheld_for TARGET: the boundary seed's file name, or empty.
withheld_for() {
    local pair
    for pair in $WITHHELD; do
        if [ "${pair%%=*}" = "$1" ]; then
            printf '%s' "${pair#*=}"
            return 0
        fi
    done
    return 0
}

log_case() {
    # case expected observed status detail
    printf '{"ts":"%s","run_id":"%s","layer":"gates","suite":"fuzz_parser_mutants","test":"%s","expected":"%s","observed":"%s","status":"%s","detail":"%s"}\n' \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$RUN_ID" "$1" "$2" "$3" "$4" "$5" >>"$LOG"
}

# expect_for TARGET: what the report of its planted bug must say. E_KIND is
# "sanitizer" (a sanitizer error whose stack names E_FILE), "property" (the
# harness's own fuzz_fail message, E_TEXT) or "either". The two HTTP plants are
# one off-by-one (the start-line guard `pos >= len` became `pos > len`) with
# two faces: when a chunk boundary falls inside the line and more input
# follows, the parser takes a byte of the next chunk and the harness reports
# "consumed more than it was given"; when the chunk is the last one, the same
# read is one byte past the buffer and AddressSanitizer reports it in
# src/net/http.c. Which a seed gives depends on its chunk mode and on the
# request method its mode byte selects: HTTP_RESPONSE seed00 (mode 0x80)
# answers a HEAD, so its "hello" is read as the start of a second message
# that ends the buffer: the sanitizer face, found from the corpus (batch #10,
# 69e3d15). Both faces are the plant; a crash in another file is not.
expect_for() {
    E_KIND=sanitizer
    E_TEXT=""
    case "$1" in
    HTTP_REQUEST)
        E_KIND=either
        E_FILE=src/net/http.c
        E_TEXT="http parser consumed more than it was given"
        ;;
    HTTP_RESPONSE)
        E_KIND=either
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

# run_harness DIR TARGET NAME WITHHELD ARG...: libFuzzer from a copy of the
# committed seeds (less the seed file named WITHHELD, if any), with -seed=1 and
# ARG... as the bound, in a fresh run directory (so a stale crash file cannot
# be mistaken for a find). Sets H_RC, H_LOG, H_RUN, H_ARTIFACTS, H_EXECS (the
# executions libFuzzer reports, crash included) and H_PHASE (corpus when the
# crash came before libFuzzer's INITED line, else mutation; none without one).
run_harness() {
    local dir="$1" t="$2" name="$3" withheld="$4" f
    shift 4
    H_RUN="$dir/run-$RUN_ID-$name"
    H_LOG="$H_RUN/log.txt"
    mkdir -p "$H_RUN/corpus"
    for f in "$SEEDS/$t"/*; do
        if [ "$(basename "$f")" != "$withheld" ]; then cp "$f" "$H_RUN/corpus/"; fi
    done
    (cd "$H_RUN" && "$dir/bin/$t" "$@" -timeout=10 -rss_limit_mb=2048 -seed=1 \
        -print_final_stats=1 corpus >"$H_LOG" 2>&1)
    H_RC=$?
    H_ARTIFACTS="$(find "$H_RUN" -maxdepth 1 \( -name 'crash-*' -o -name 'timeout-*' -o -name 'oom-*' \) | wc -l | tr -d ' ')"
    H_EXECS="$(sed -n 's/^stat::number_of_executed_units: *//p' "$H_LOG" | tail -1)"
    if [ -z "$H_EXECS" ]; then H_EXECS="?"; fi
    if grep -q 'INITED' "$H_LOG"; then
        H_PHASE=mutation
    elif [ "$H_ARTIFACTS" -gt 0 ]; then
        H_PHASE=corpus
    else
        H_PHASE=none
    fi
}

# names_plant: the last run crashed, and its report names the plant of the
# target expect_for set up: a sanitizer error whose stack is in E_FILE, or
# the harness's own property message E_TEXT (E_KIND says which, or either).
names_plant() {
    if [ "$H_RC" -eq 0 ] || [ "$H_ARTIFACTS" -eq 0 ]; then return 1; fi
    case "$E_KIND" in
    property) grep -q "property violated: $E_TEXT" "$H_LOG" ;;
    either)
        grep -q "property violated: $E_TEXT" "$H_LOG" ||
            { grep -Eq 'ERROR: AddressSanitizer|runtime error:' "$H_LOG" &&
                grep -q "$E_FILE" "$H_LOG"; }
        ;;
    *)
        grep -Eq 'ERROR: AddressSanitizer|runtime error:' "$H_LOG" && grep -q "$E_FILE" "$H_LOG"
        ;;
    esac
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
        run_harness "$CLEAN_ABS" "$t" "clean-$t" "" -max_total_time="$CLEAN_SECONDS"
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
        table_row "$t" "**FAIL**: the mutant harness does not build" "not run"
        continue
    fi
    # Replay control: the committed seeds, whole. Gates.
    start="$(date +%s)"
    run_harness "$m_abs" "$t" "replay-$t" "" -max_total_time="$MUTANT_SECONDS"
    elapsed=$(($(date +%s) - start))
    if names_plant; then
        echo "  PASS: $t replay control found its planted bug after $H_EXECS execs, ${elapsed}s (from the $H_PHASE; $E_KIND in $E_FILE)"
        log_case "$t" "replay-control" "exit$H_RC phase=$H_PHASE execs=$H_EXECS ${elapsed}s" "pass" "$H_LOG"
        replay_cell="found after $H_EXECS execs, ${elapsed}s (from the $H_PHASE)"
    else
        echo "  FAIL: $t replay control did NOT find its planted bug in ${MUTANT_SECONDS}s (exit$H_RC; artifacts=$H_ARTIFACTS; expected $E_KIND in $E_FILE; log $H_LOG)"
        log_case "$t" "replay-control" "exit$H_RC artifacts=$H_ARTIFACTS phase=$H_PHASE" "fail" "$H_LOG"
        failures=$((failures + 1))
        replay_cell="**FAIL**: not found in ${MUTANT_SECONDS}s"
    fi

    # Discovery control: the boundary seed withheld, a fixed -seed, an
    # execution budget. Observes; nothing here changes the exit status.
    w="$(withheld_for "$t")"
    if [ -z "$w" ]; then
        table_row "$t" "$replay_cell" "no boundary seed was added; the replay control reaches it from the original seeds"
    elif [ "$DISCOVERY" != "1" ]; then
        table_row "$t" "$replay_cell" "skipped (ASX_FUZZ_DISCOVERY=$DISCOVERY)"
    else
        start="$(date +%s)"
        run_harness "$m_abs" "$t" "discovery-$t" "$w" \
            -runs="$DISCOVERY_RUNS" -max_total_time="$DISCOVERY_SECONDS"
        elapsed=$(($(date +%s) - start))
        if names_plant; then
            d_cell="$w withheld: found after $H_EXECS execs, ${elapsed}s"
            echo "  OBSERVED: $t discovery control ($w withheld): found after $H_EXECS execs, ${elapsed}s"
            log_case "$t" "discovery-control" "found execs=$H_EXECS ${elapsed}s" "observed" "$H_LOG"
        elif [ "$H_RC" -ne 0 ] && [ "$H_ARTIFACTS" -gt 0 ]; then
            d_cell="$w withheld: a crash that does not name the plant after $H_EXECS execs, ${elapsed}s (see $H_LOG)"
            echo "  OBSERVED: $t discovery control ($w withheld): crash NOT naming the plant after $H_EXECS execs (log $H_LOG)"
            log_case "$t" "discovery-control" "other-crash execs=$H_EXECS ${elapsed}s" "observed" "$H_LOG"
        else
            d_cell="$w withheld: NOT found within $H_EXECS execs, ${elapsed}s (budget $DISCOVERY_RUNS execs / ${DISCOVERY_SECONDS}s)"
            echo "  OBSERVED: $t discovery control ($w withheld): NOT found within $H_EXECS execs, ${elapsed}s"
            log_case "$t" "discovery-control" "not-found execs=$H_EXECS ${elapsed}s" "observed" "$H_LOG"
        fi
        table_row "$t" "$replay_cell" "$d_cell"
    fi
done

# --- Results ------------------------------------------------------------------
TABLE_FILE="$ABS_ROOT/discovery.md"
{
    printf 'Replay control (gates) and discovery control (observes), run %s\n\n' "$RUN_ID"
    printf 'Fixed libFuzzer -seed=1. Replay: the committed seeds, up to %s s. Discovery: the boundary seed withheld, up to %s execs / %s s.\n' \
        "$MUTANT_SECONDS" "$DISCOVERY_RUNS" "$DISCOVERY_SECONDS"
    printf 'A green replay proves wiring, not fuzz discovery.\n\n'
    printf '| harness | replay control (gates) | discovery control (observes) |\n'
    printf '|---|---|---|\n'
    printf '%s' "$TABLE_ROWS"
} >"$TABLE_FILE"
echo
cat "$TABLE_FILE"
echo

if [ "$failures" -eq 0 ]; then
    echo "test_fuzz_parser_mutants: the replay control passed for every harness, and the unmutated harnesses stayed clean (discovery table: $TABLE_FILE)"
    exit 0
fi
echo "test_fuzz_parser_mutants: $failures check(s) failed"
exit 1
