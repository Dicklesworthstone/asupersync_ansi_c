#!/usr/bin/env bash
# =============================================================================
# run_gcc_analyzer.sh — GCC -fanalyzer lane (bd-9kll.16.4, plan section 10.7.2)
#
# Compiles every translation unit of src/ with the build's own flags plus
# -fanalyzer, in the two configurations that compile different code:
#
#   CORE        PROFILE=CORE  DETERMINISTIC=1   the kernel (the default build)
#   POSIX_LIVE  PROFILE=POSIX DETERMINISTIC=0   the OS adapters, real clock
#
# and triages each -Wanalyzer-* diagnostic by the section 10.7.2 rule:
#
#   - a diagnostic with an ASX_ANALYZER_WAIVER comment within 3 lines above or
#     1 line below its line is a documented false positive; each is listed
#     with its rationale in docs/STATIC_ANALYSIS_TRIAGE.md;
#   - any other diagnostic is must-fix and fails the lane. Backlog does not
#     exist in this lane (section 10.7.2: not allowed in kernel paths).
#
# The analyzer's findings change between GCC releases, so the lane is pinned:
# CI runs it in the gcc:15.2.0 container (ci.yml, job gcc-analyzer), and the
# waivers were triaged with GCC 15.2. A different compiler is a warning
# locally and a failure under STRICT_GATES=1 / CI.
#
# The flags come from `make -n build`, so a profile or define added to the
# Makefile reaches this lane without an edit here; -Werror is dropped so every
# diagnostic of a unit is collected, and objects go to /dev/null.
#
# Usage: tools/ci/run_gcc_analyzer.sh [--verbose] [CONFIG...]
#   CONFIG   CORE and/or POSIX_LIVE (default: both)
# Environment:
#   GCC_ANALYZER_CC       compiler (default gcc)
#   GCC_ANALYZER_EXPECT   expected GCC major.minor (default 15.2)
#   JOBS                  parallel compiles (default: online CPUs)
#   ASX_ANALYZER_OUT      report directory (default build/gcc-analyzer)
#
# Exit: 0 clean, 1 must-fix findings, 2 tool error.
#
# SPDX-License-Identifier: MIT
# =============================================================================

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
CC_ANALYZER="${GCC_ANALYZER_CC:-gcc}"
EXPECT="${GCC_ANALYZER_EXPECT:-15.2}"
STRICT="${STRICT_GATES:-${CI:-0}}"
OUT_DIR="${ASX_ANALYZER_OUT:-$REPO_ROOT/build/gcc-analyzer}"
WAIVER_PATTERN='ASX_ANALYZER_WAIVER'
VERBOSE=0
CONFIGS=()

if command -v nproc >/dev/null 2>&1; then
    JOBS="${JOBS:-$(nproc)}"
else
    JOBS="${JOBS:-4}"
fi

# Worker mode: run one compile command, its diagnostics into the given file.
if [ "${1:-}" = "--worker" ]; then
    cmd="$2"
    out="$3"
    (cd "$REPO_ROOT" && sh -c "$cmd") >"$out" 2>&1
    exit 0
fi

while [ $# -gt 0 ]; do
    case "$1" in
    --verbose) VERBOSE=1 ;;
    CORE | POSIX_LIVE) CONFIGS+=("$1") ;;
    -h | --help)
        sed -n '2,40p' "${BASH_SOURCE[0]}"
        exit 0
        ;;
    *)
        echo "run_gcc_analyzer: unknown argument: $1" >&2
        exit 2
        ;;
    esac
    shift
done
if [ ${#CONFIGS[@]} -eq 0 ]; then CONFIGS=(CORE POSIX_LIVE); fi

strict() { [ "$STRICT" = "1" ] || [ "$STRICT" = "true" ]; }

# config_make_args NAME: the make variables of a configuration.
config_make_args() {
    case "$1" in
    CORE) echo "PROFILE=CORE DETERMINISTIC=1" ;;
    POSIX_LIVE) echo "PROFILE=POSIX DETERMINISTIC=0" ;;
    esac
}

# --- The compiler must be GCC with the analyzer -------------------------------
if ! command -v "$CC_ANALYZER" >/dev/null 2>&1; then
    echo "[asx] gcc-analyzer: $CC_ANALYZER not found" >&2
    if strict; then exit 2; fi
    echo "[asx] gcc-analyzer: SKIP (STRICT_GATES=1 makes this a failure)"
    exit 0
fi
if ! echo 'int asx_probe;' | "$CC_ANALYZER" -fanalyzer -x c -c -o /dev/null - >/dev/null 2>&1; then
    echo "[asx] gcc-analyzer: $CC_ANALYZER does not support -fanalyzer (not GCC 10+?)" >&2
    if strict; then exit 2; fi
    echo "[asx] gcc-analyzer: SKIP (STRICT_GATES=1 makes this a failure)"
    exit 0
fi
ACTUAL="$("$CC_ANALYZER" -dumpfullversion 2>/dev/null || "$CC_ANALYZER" -dumpversion)"
case "$ACTUAL" in
"$EXPECT" | "$EXPECT".*) ;;
*)
    echo "[asx] gcc-analyzer: $CC_ANALYZER is GCC $ACTUAL; the waivers were triaged with GCC $EXPECT" >&2
    if strict; then
        echo "[asx] gcc-analyzer: FAIL (pinned compiler mismatch under STRICT_GATES/CI)" >&2
        exit 2
    fi
    echo "[asx] gcc-analyzer: continuing; findings may differ from the pinned compiler's" >&2
    ;;
esac

mkdir -p "$OUT_DIR"
REPORT="$OUT_DIR/report.txt"
: >"$REPORT"

# has_waiver FILE LINE: an ASX_ANALYZER_WAIVER within 3 lines above or 1 below
# (the window run_static_analysis.sh uses for cppcheck and clang-tidy).
has_waiver() {
    local file="$1" line="$2" start end
    start=$((line > 3 ? line - 3 : 1))
    end=$((line + 1))
    [ -f "$REPO_ROOT/$file" ] || return 1
    sed -n "${start},${end}p" "$REPO_ROOT/$file" 2>/dev/null | grep -q "$WAIVER_PATTERN"
}

total_findings=0
total_waived=0
tool_errors=0

for cfg in "${CONFIGS[@]}"; do
    work="$OUT_DIR/$cfg"
    mkdir -p "$work/diag"
    cmds="$work/compile.cmds"
    # shellcheck disable=SC2046
    make -C "$REPO_ROOT" -n build CC="$CC_ANALYZER" BUILD_DIR="$work/obj" \
        $(config_make_args "$cfg") 2>/dev/null |
        grep -E ' -c -o [^ ]+ [^ ]+\.c$' >"$cmds" || true
    count="$(wc -l <"$cmds" | tr -d ' ')"
    if [ "$count" -eq 0 ]; then
        echo "[asx] gcc-analyzer ($cfg): FAIL (make -n build printed no compile commands)" >&2
        tool_errors=$((tool_errors + 1))
        continue
    fi

    # The build's command, minus dependency files and -Werror, plus the
    # analyzer; its object goes nowhere.
    i=0
    jobs_file="$work/jobs.list"
    : >"$jobs_file"
    while IFS= read -r line; do
        i=$((i + 1))
        cmd="$(printf '%s' "$line" |
            sed -e 's/ -MMD -MP//' -e 's/ -Werror / /' \
                -e 's/ -c -o [^ ]* / -fanalyzer -Wno-error -fdiagnostics-path-format=separate-events -fno-diagnostics-show-caret -c -o \/dev\/null /')"
        printf '%s\0%s\0' "$cmd" "$work/diag/$i.txt" >>"$jobs_file"
    done <"$cmds"

    echo "[asx] gcc-analyzer ($cfg): $count units, GCC $ACTUAL, -j$JOBS"
    xargs -0 -n 2 -P "$JOBS" "$0" --worker <"$jobs_file"

    : >"$work/findings.txt"
    cat "$work"/diag/*.txt 2>/dev/null |
        grep -E '^[^ ]+:[0-9]+:[0-9]+: (warning|error): ' |
        grep -E '\[-Wanalyzer-[a-z0-9-]+\]' |
        sed -E 's/^([^ ]+):([0-9]+):([0-9]+): (warning|error): (.*)$/\1:\2:\3:\5/' |
        sort | uniq -c >"$work/findings.txt"

    # A compile error that is not an analyzer diagnostic means the lane did
    # not analyze that unit: a tool error, never a silent pass.
    if cat "$work"/diag/*.txt 2>/dev/null | grep -E '^[^ ]+:[0-9]+:[0-9]+: (fatal )?error: ' |
        grep -vq 'Wanalyzer'; then
        echo "[asx] gcc-analyzer ($cfg): FAIL (a unit did not compile; see $work/diag)" >&2
        cat "$work"/diag/*.txt | grep -E '^[^ ]+:[0-9]+:[0-9]+: (fatal )?error: ' | head -5 >&2
        tool_errors=$((tool_errors + 1))
        continue
    fi

    cfg_findings=0
    cfg_waived=0
    while IFS= read -r row; do
        [ -n "$row" ] || continue
        n="$(printf '%s' "$row" | awk '{print $1}')"
        rest="${row#*[0-9] }"
        file="$(printf '%s' "$rest" | cut -d: -f1)"
        lineno="$(printf '%s' "$rest" | cut -d: -f2)"
        msg="$(printf '%s' "$rest" | cut -d: -f4-)"
        if has_waiver "$file" "$lineno"; then
            cfg_waived=$((cfg_waived + 1))
            printf 'WAIVED  [%s] %s:%s x%s %s\n' "$cfg" "$file" "$lineno" "$n" "$msg" >>"$REPORT"
            if [ "$VERBOSE" -eq 1 ]; then
                echo "  WAIVED  $file:$lineno x$n $msg"
            fi
        else
            cfg_findings=$((cfg_findings + 1))
            printf 'FINDING [%s] %s:%s x%s %s\n' "$cfg" "$file" "$lineno" "$n" "$msg" >>"$REPORT"
            echo "  FINDING [$cfg] $file:$lineno $msg"
        fi
    done <"$work/findings.txt"

    total_findings=$((total_findings + cfg_findings))
    total_waived=$((total_waived + cfg_waived))
    echo "[asx] gcc-analyzer ($cfg): $cfg_findings finding(s), $cfg_waived waived location(s)"
done

echo "[asx] gcc-analyzer: $total_findings finding(s), $total_waived waived location(s) (report: $REPORT)"
if [ "$tool_errors" -gt 0 ]; then
    echo "[asx] gcc-analyzer: FAIL ($tool_errors tool error(s))" >&2
    exit 2
fi
if [ "$total_findings" -gt 0 ]; then
    echo "[asx] gcc-analyzer: FAIL (must-fix findings; triage each in docs/STATIC_ANALYSIS_TRIAGE.md)" >&2
    echo '  Waive a documented false positive with a comment within 3 lines above it:' >&2
    echo '  /* ASX_ANALYZER_WAIVER("reason: brief justification") */' >&2
    exit 1
fi
echo "[asx] gcc-analyzer: PASS"
exit 0
