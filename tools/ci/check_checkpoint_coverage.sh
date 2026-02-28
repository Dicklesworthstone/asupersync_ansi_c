#!/usr/bin/env bash
# =============================================================================
# check_checkpoint_coverage.sh — Checkpoint-coverage lint gate (bd-66l.6)
#
# Flags loops in kernel/runtime source paths that are potentially long-running
# but do not contain asx_checkpoint() or an explicit ASX_CHECKPOINT_WAIVER.
#
# Per PLAN section 6.8.G:
#   "every long-running loop must call asx_checkpoint() or explicit waiver macro"
#
# Exit 0 = pass, Exit 1 = violations found, Exit 2 = usage error
#
# SPDX-License-Identifier: MIT
# =============================================================================

set -euo pipefail

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

# Directories to scan (kernel-path source files)
SCAN_DIRS=("src/runtime" "src/core" "src/channel" "src/time")

# File extensions to scan
SCAN_EXTS=("*.c")

# Patterns that indicate a loop is bounded by a compile-time constant
# and therefore exempt from the checkpoint requirement.
BOUNDED_PATTERNS=(
    'ASX_MAX_'
    'ASX_CLEANUP_STACK_CAPACITY'
    'ASX_GHOST_'
    'ASX_ADAPTIVE_'
    'ASX_AFFINITY_TABLE_CAPACITY'
    'ASX_TRACE_CAPACITY'
    'ASX_HINDSIGHT_CAPACITY'
    'ASX_ERROR_LEDGER_'
    'sizeof('
    '< 4'
    '< 8'
    '< 10'
    '< 16'
    '< 20'
    '< 24'
    '< 64'
    '<= 10'
    '<= 16'
    '<= 20'
)

# Patterns that indicate a checkpoint or waiver is present in the loop body
CHECKPOINT_PATTERNS=(
    'asx_checkpoint'
    'asx_is_cancelled'
    'ASX_CHECKPOINT_WAIVER'
)

# ---------------------------------------------------------------------------
# State
# ---------------------------------------------------------------------------
VIOLATIONS=0
WAIVERS=0
BOUNDED_SKIPS=0
CHECKED_LOOPS=0
VERBOSE=0
JSON_OUTPUT=0

# ---------------------------------------------------------------------------
# Usage
# ---------------------------------------------------------------------------
usage() {
    echo "Usage: $0 [OPTIONS] [DIR...]"
    echo ""
    echo "Options:"
    echo "  --verbose     Show all loops including passing ones"
    echo "  --json        Output results as JSON"
    echo "  --help        Show this help"
    echo ""
    echo "If DIR is specified, scan those directories instead of defaults."
    exit 2
}

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
CUSTOM_DIRS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --verbose) VERBOSE=1; shift ;;
        --json) JSON_OUTPUT=1; shift ;;
        --help|-h) usage ;;
        -*) echo "Unknown option: $1"; usage ;;
        *) CUSTOM_DIRS+=("$1"); shift ;;
    esac
done

if [[ ${#CUSTOM_DIRS[@]} -gt 0 ]]; then
    SCAN_DIRS=("${CUSTOM_DIRS[@]}")
fi

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

# Check if a string matches any pattern in an array
matches_any() {
    local text="$1"
    shift
    local patterns=("$@")
    for pat in "${patterns[@]}"; do
        if [[ "$text" == *"$pat"* ]]; then
            return 0
        fi
    done
    return 1
}

# Extract the body of a loop starting at a given line number.
# Tracks brace depth to find the matching close brace.
# Returns the loop body text (including the loop header line).
extract_loop_body() {
    local file="$1"
    local start_line="$2"
    local total_lines
    total_lines=$(wc -l < "$file")

    local depth=0
    local started=0
    local body=""
    local line_num=0

    while IFS= read -r line; do
        line_num=$((line_num + 1))
        if [[ $line_num -lt $start_line ]]; then
            continue
        fi

        body+="$line"$'\n'

        # Count braces (simplified — doesn't handle braces in strings/comments,
        # but good enough for C source that passes -Werror compilation)
        local opens="${line//[^\{]/}"
        local closes="${line//[^\}]/}"
        depth=$((depth + ${#opens} - ${#closes}))

        if [[ ${#opens} -gt 0 ]]; then
            started=1
        fi

        # If we've entered the body and returned to depth 0, we're done
        if [[ $started -eq 1 && $depth -le 0 ]]; then
            break
        fi

        # Safety: don't scan more than 200 lines for a single loop body
        if [[ $((line_num - start_line)) -gt 200 ]]; then
            break
        fi
    done < "$file"

    echo "$body"
}

# Check if we're inside a #ifdef ASX_DEBUG_* block (debug-only code is exempt)
is_debug_only() {
    local file="$1"
    local target_line="$2"

    local ifdef_depth=0
    local in_debug=0
    local line_num=0

    while IFS= read -r line; do
        line_num=$((line_num + 1))

        if [[ $line_num -ge $target_line ]]; then
            break
        fi

        # Track preprocessor conditionals
        if [[ "$line" =~ ^[[:space:]]*#[[:space:]]*(ifdef|if)[[:space:]].*ASX_DEBUG ]]; then
            in_debug=$((in_debug + 1))
            ifdef_depth=$((ifdef_depth + 1))
        elif [[ "$line" =~ ^[[:space:]]*#[[:space:]]*(ifdef|if)[[:space:]] ]]; then
            ifdef_depth=$((ifdef_depth + 1))
        elif [[ "$line" =~ ^[[:space:]]*#[[:space:]]*endif ]]; then
            if [[ $ifdef_depth -gt 0 ]]; then
                ifdef_depth=$((ifdef_depth - 1))
                if [[ $in_debug -gt $ifdef_depth ]]; then
                    in_debug=$ifdef_depth
                fi
            fi
        fi
    done < "$file"

    [[ $in_debug -gt 0 ]]
}

# ---------------------------------------------------------------------------
# Main scan
# ---------------------------------------------------------------------------

violations_json="[]"

scan_file() {
    local file="$1"
    local line_num=0

    # Check for file-level waiver (ASX_CHECKPOINT_WAIVER_FILE in first 30 lines)
    local file_waiver=0
    local head_lines
    head_lines=$(head -30 "$file")
    if [[ "$head_lines" == *"ASX_CHECKPOINT_WAIVER_FILE"* ]]; then
        file_waiver=1
        WAIVERS=$((WAIVERS + 1))
        if [[ $VERBOSE -eq 1 ]]; then
            echo "  WAIV (file-level)  $file"
        fi
        return
    fi

    while IFS= read -r line; do
        line_num=$((line_num + 1))

        # Detect loop headers: for(...), while(...), do {
        # Match: for ( ... ), while ( ... ), do {
        if [[ "$line" =~ ^[[:space:]]*(for|while)[[:space:]]*\( ]] || \
           [[ "$line" =~ ^[[:space:]]*do[[:space:]]*\{ ]]; then

            local loop_type
            if [[ "$line" =~ ^[[:space:]]*(for|while) ]]; then
                loop_type="${BASH_REMATCH[1]}"
            else
                loop_type="do"
            fi

            CHECKED_LOOPS=$((CHECKED_LOOPS + 1))

            # 1. Check if inside #ifdef ASX_DEBUG_* block
            if is_debug_only "$file" "$line_num"; then
                if [[ $VERBOSE -eq 1 ]]; then
                    echo "  SKIP (debug-only) $file:$line_num: $loop_type loop"
                fi
                continue
            fi

            # 2. Check if the loop condition uses a bounded pattern
            if matches_any "$line" "${BOUNDED_PATTERNS[@]}"; then
                BOUNDED_SKIPS=$((BOUNDED_SKIPS + 1))
                if [[ $VERBOSE -eq 1 ]]; then
                    echo "  SKIP (bounded)    $file:$line_num: $loop_type loop"
                fi
                continue
            fi

            # 3. For non-bounded loops, extract body and check for checkpoint/waiver
            local body
            body=$(extract_loop_body "$file" "$line_num")

            local has_checkpoint=0
            local has_waiver=0

            for cpat in "${CHECKPOINT_PATTERNS[@]}"; do
                if [[ "$body" == *"$cpat"* ]]; then
                    if [[ "$cpat" == "ASX_CHECKPOINT_WAIVER" ]]; then
                        has_waiver=1
                    else
                        has_checkpoint=1
                    fi
                fi
            done

            if [[ $has_checkpoint -eq 1 ]]; then
                if [[ $VERBOSE -eq 1 ]]; then
                    echo "  PASS (checkpoint) $file:$line_num: $loop_type loop"
                fi
                continue
            fi

            if [[ $has_waiver -eq 1 ]]; then
                WAIVERS=$((WAIVERS + 1))
                if [[ $VERBOSE -eq 1 ]]; then
                    echo "  WAIV              $file:$line_num: $loop_type loop"
                fi
                continue
            fi

            # 4. Check if the loop condition looks bounded by a nearby context
            #    (e.g., the next few lines after the header reveal a small bound)
            local next_lines
            next_lines=$(sed -n "$((line_num)),$((line_num + 3))p" "$file")
            if matches_any "$next_lines" "${BOUNDED_PATTERNS[@]}"; then
                BOUNDED_SKIPS=$((BOUNDED_SKIPS + 1))
                if [[ $VERBOSE -eq 1 ]]; then
                    echo "  SKIP (bounded-ctx) $file:$line_num: $loop_type loop"
                fi
                continue
            fi

            # 5. This is a violation
            VIOLATIONS=$((VIOLATIONS + 1))
            local trimmed
            trimmed=$(echo "$line" | sed 's/^[[:space:]]*//')

            if [[ $JSON_OUTPUT -eq 0 ]]; then
                echo "  VIOLATION $file:$line_num - $loop_type loop missing asx_checkpoint() or ASX_CHECKPOINT_WAIVER()"
                echo "            $trimmed"
                echo "            Suggested fix: add asx_checkpoint() call or ASX_CHECKPOINT_WAIVER(\"reason\") annotation"
            fi
        fi
    done < "$file"
}

# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

if [[ $JSON_OUTPUT -eq 0 ]]; then
    echo "[asx] checkpoint-coverage: scanning kernel paths for uncovered loops..."
fi

for dir in "${SCAN_DIRS[@]}"; do
    if [[ ! -d "$dir" ]]; then
        if [[ $JSON_OUTPUT -eq 0 ]]; then
            echo "  SKIP $dir (not found)"
        fi
        continue
    fi

    for ext in "${SCAN_EXTS[@]}"; do
        while IFS= read -r -d '' file; do
            if [[ $VERBOSE -eq 1 && $JSON_OUTPUT -eq 0 ]]; then
                echo "  SCAN $file"
            fi
            scan_file "$file"
        done < <(find "$dir" -name "$ext" -print0 2>/dev/null)
    done
done

# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------

if [[ $JSON_OUTPUT -eq 1 ]]; then
    cat <<EOF
{
  "gate": "checkpoint-coverage",
  "pass": $([ "$VIOLATIONS" -eq 0 ] && echo "true" || echo "false"),
  "loops_checked": $CHECKED_LOOPS,
  "violations": $VIOLATIONS,
  "waivers": $WAIVERS,
  "bounded_skips": $BOUNDED_SKIPS
}
EOF
else
    echo ""
    echo "[asx] checkpoint-coverage: $CHECKED_LOOPS loops checked, $BOUNDED_SKIPS bounded (skipped), $WAIVERS waived, $VIOLATIONS violation(s)"
    if [[ $VIOLATIONS -eq 0 ]]; then
        echo "[asx] checkpoint-coverage: PASS"
    else
        echo "[asx] checkpoint-coverage: FAIL — $VIOLATIONS loop(s) missing checkpoint or waiver"
        echo ""
        echo "To fix:"
        echo "  1. Add asx_checkpoint(self, &cr) inside the loop, OR"
        echo "  2. Add ASX_CHECKPOINT_WAIVER(\"reason\") as a comment/macro in the loop body"
        echo ""
        echo "Waiver format:"
        echo "  ASX_CHECKPOINT_WAIVER(\"kernel-scheduler: this IS the scheduler loop\")"
    fi
fi

exit $VIOLATIONS
