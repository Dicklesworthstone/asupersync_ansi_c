#!/usr/bin/env bash
# =============================================================================
# run_static_analysis.sh — Section 10.7 static analysis gate (bd-66l.10)
#
# Runs cppcheck and clang-tidy with curated high-signal flags.
# Supports explicit waivers via ASX_ANALYZER_WAIVER comments.
# Produces machine-readable JSON report for CI consumption.
#
# Exit 0 = pass, Exit 1 = violations, Exit 2 = usage/config error
#
# SPDX-License-Identifier: MIT
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
ARTIFACT_DIR="$REPO_ROOT/tools/ci/artifacts/static-analysis"
REPORT_FILE="$ARTIFACT_DIR/report.json"

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
SCAN_DIRS=("src/core" "src/runtime" "src/channel" "src/time")
WAIVER_PATTERN='ASX_ANALYZER_WAIVER'
JSON_OUTPUT=0
VERBOSE=0
STRICT=0

# Counters
CPPCHECK_FINDINGS=0
CPPCHECK_WAIVERS=0
CLANG_TIDY_FINDINGS=0
CLANG_TIDY_WAIVERS=0
TOTAL_PASS=0
TOTAL_FAIL=0

# ---------------------------------------------------------------------------
# Usage
# ---------------------------------------------------------------------------
usage() {
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  --json        Output results as JSON"
    echo "  --verbose     Show all findings including waived ones"
    echo "  --strict      Fail on any finding (ignore waivers)"
    echo "  --help        Show this help"
    exit 2
}

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --json) JSON_OUTPUT=1; shift ;;
        --verbose) VERBOSE=1; shift ;;
        --strict) STRICT=1; shift ;;
        --help|-h) usage ;;
        *) echo "Unknown option: $1"; usage ;;
    esac
done

mkdir -p "$ARTIFACT_DIR"

# ---------------------------------------------------------------------------
# Waiver checking
# ---------------------------------------------------------------------------
# Check if a finding at file:line has a waiver comment nearby (within 3 lines)
has_waiver() {
    local file="$1"
    local line="$2"
    local start=$((line > 3 ? line - 3 : 1))
    local end=$((line + 1))

    if [[ ! -f "$file" ]]; then
        return 1
    fi

    local context
    context=$(sed -n "${start},${end}p" "$file" 2>/dev/null || true)
    if [[ "$context" == *"$WAIVER_PATTERN"* ]]; then
        return 0
    fi
    return 1
}

# ---------------------------------------------------------------------------
# cppcheck analysis
# ---------------------------------------------------------------------------
run_cppcheck() {
    if ! command -v cppcheck >/dev/null 2>&1; then
        if [[ $JSON_OUTPUT -eq 0 ]]; then
            echo "[asx] static-analysis: cppcheck not available, skipping"
        fi
        return 0
    fi

    local cppcheck_output
    local cppcheck_exit=0
    local tmpfile
    tmpfile=$(mktemp)

    # Run cppcheck with XML output for machine parsing
    cppcheck \
        --enable=warning,performance,portability,style \
        --std=c99 \
        --error-exitcode=0 \
        --suppress=missingIncludeSystem \
        --suppress=unusedFunction \
        --suppress=unmatchedSuppression \
        --suppress=variableScope \
        --suppress=constVariablePointer \
        --suppress=constParameterPointer \
        --suppress=constVariable \
        --suppress=normalCheckLevelMaxBranches \
        --suppress=toomanyconfigs \
        --template='{file}:{line}:{severity}:{id}:{message}' \
        -I "$REPO_ROOT/include" \
        "${SCAN_DIRS[@]/#/$REPO_ROOT/}" \
        2>"$tmpfile" || true

    local finding_count=0
    local waiver_count=0

    while IFS= read -r line; do
        # Skip non-finding lines
        if [[ ! "$line" =~ ^[^:]+:[0-9]+: ]]; then
            continue
        fi

        local file line_num rest
        file=$(echo "$line" | cut -d: -f1)
        line_num=$(echo "$line" | cut -d: -f2)

        if [[ -n "$line_num" && "$line_num" =~ ^[0-9]+$ ]]; then
            if has_waiver "$file" "$line_num" && [[ $STRICT -eq 0 ]]; then
                waiver_count=$((waiver_count + 1))
                if [[ $VERBOSE -eq 1 && $JSON_OUTPUT -eq 0 ]]; then
                    echo "  WAIVED (cppcheck): $line"
                fi
            else
                finding_count=$((finding_count + 1))
                if [[ $JSON_OUTPUT -eq 0 ]]; then
                    echo "  FINDING (cppcheck): $line"
                fi
            fi
        fi
    done < "$tmpfile"

    rm -f "$tmpfile"
    CPPCHECK_FINDINGS=$finding_count
    CPPCHECK_WAIVERS=$waiver_count

    if [[ $JSON_OUTPUT -eq 0 ]]; then
        echo "[asx] cppcheck: $finding_count finding(s), $waiver_count waived"
    fi
}

# ---------------------------------------------------------------------------
# clang-tidy analysis (if available)
# ---------------------------------------------------------------------------
run_clang_tidy() {
    if ! command -v clang-tidy >/dev/null 2>&1; then
        if [[ $JSON_OUTPUT -eq 0 ]]; then
            echo "[asx] static-analysis: clang-tidy not available, skipping"
        fi
        return 0
    fi

    local finding_count=0
    local waiver_count=0
    local tmpfile
    tmpfile=$(mktemp)

    # Build the list of source files
    local src_files=()
    for dir in "${SCAN_DIRS[@]}"; do
        while IFS= read -r -d '' f; do
            src_files+=("$f")
        done < <(find "$REPO_ROOT/$dir" -name '*.c' -print0 2>/dev/null)
    done

    if [[ ${#src_files[@]} -eq 0 ]]; then
        if [[ $JSON_OUTPUT -eq 0 ]]; then
            echo "[asx] clang-tidy: no source files found, skipping"
        fi
        return 0
    fi

    # Run clang-tidy with curated checks
    clang-tidy \
        --checks='-*,bugprone-*,cert-*,misc-*,performance-*,portability-*,-bugprone-easily-swappable-parameters,-cert-dcl37-c,-cert-dcl51-c,-cert-err33-c,-misc-unused-parameters,-misc-include-cleaner' \
        "${src_files[@]}" \
        -- -std=c99 -I"$REPO_ROOT/include" \
           -DASX_PROFILE_CORE -DASX_CODEC_JSON -DASX_DETERMINISTIC=1 \
        2>"$tmpfile" || true

    while IFS= read -r line; do
        if [[ "$line" =~ ^[^:]+:[0-9]+:[0-9]+:\ (warning|error): ]]; then
            local file line_num
            file=$(echo "$line" | cut -d: -f1)
            line_num=$(echo "$line" | cut -d: -f2)

            if [[ -n "$line_num" && "$line_num" =~ ^[0-9]+$ ]]; then
                if has_waiver "$file" "$line_num" && [[ $STRICT -eq 0 ]]; then
                    waiver_count=$((waiver_count + 1))
                    if [[ $VERBOSE -eq 1 && $JSON_OUTPUT -eq 0 ]]; then
                        echo "  WAIVED (clang-tidy): $line"
                    fi
                else
                    finding_count=$((finding_count + 1))
                    if [[ $JSON_OUTPUT -eq 0 ]]; then
                        echo "  FINDING (clang-tidy): $line"
                    fi
                fi
            fi
        fi
    done < "$tmpfile"

    rm -f "$tmpfile"
    CLANG_TIDY_FINDINGS=$finding_count
    CLANG_TIDY_WAIVERS=$waiver_count

    if [[ $JSON_OUTPUT -eq 0 ]]; then
        echo "[asx] clang-tidy: $finding_count finding(s), $waiver_count waived"
    fi
}

# ---------------------------------------------------------------------------
# Warning-as-error policy verification
# ---------------------------------------------------------------------------
verify_warning_policy() {
    local compiler="${1:-gcc}"
    local exit_code=0

    if ! command -v "$compiler" >/dev/null 2>&1; then
        if [[ $JSON_OUTPUT -eq 0 ]]; then
            echo "[asx] warning-policy ($compiler): not available, skipping"
        fi
        return 0
    fi

    # Verify that the build succeeds with -Werror
    if make -C "$REPO_ROOT" build CC="$compiler" 2>&1 | tail -1 | grep -q "build complete"; then
        TOTAL_PASS=$((TOTAL_PASS + 1))
        if [[ $JSON_OUTPUT -eq 0 ]]; then
            echo "[asx] warning-policy ($compiler): PASS (clean build with -Werror)"
        fi
    else
        TOTAL_FAIL=$((TOTAL_FAIL + 1))
        exit_code=1
        if [[ $JSON_OUTPUT -eq 0 ]]; then
            echo "[asx] warning-policy ($compiler): FAIL (warnings present)"
        fi
    fi

    return $exit_code
}

# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

if [[ $JSON_OUTPUT -eq 0 ]]; then
    echo "[asx] static-analysis: section 10.7 gates..."
    echo ""
fi

# Run all analysis steps
run_cppcheck
run_clang_tidy

# Compute totals
total_findings=$((CPPCHECK_FINDINGS + CLANG_TIDY_FINDINGS))
total_waivers=$((CPPCHECK_WAIVERS + CLANG_TIDY_WAIVERS))
pass=$([[ $total_findings -eq 0 ]] && echo "true" || echo "false")

# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------
if [[ $JSON_OUTPUT -eq 1 ]]; then
    cat > "$REPORT_FILE" <<EOF
{
  "gate": "static-analysis",
  "section": "10.7",
  "pass": $pass,
  "cppcheck": {
    "findings": $CPPCHECK_FINDINGS,
    "waivers": $CPPCHECK_WAIVERS
  },
  "clang_tidy": {
    "findings": $CLANG_TIDY_FINDINGS,
    "waivers": $CLANG_TIDY_WAIVERS
  },
  "total_findings": $total_findings,
  "total_waivers": $total_waivers,
  "strict_mode": $([[ $STRICT -eq 1 ]] && echo "true" || echo "false"),
  "timestamp": "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
}
EOF
    cat "$REPORT_FILE"
else
    echo ""
    echo "[asx] static-analysis: $total_findings finding(s), $total_waivers waived"
    if [[ $total_findings -eq 0 ]]; then
        echo "[asx] static-analysis: PASS"
    else
        echo "[asx] static-analysis: FAIL — $total_findings unwaived finding(s)"
        echo ""
        echo "To waive a finding, add before the flagged line:"
        echo '  /* ASX_ANALYZER_WAIVER("reason: brief justification") */'
    fi
fi

exit $total_findings
