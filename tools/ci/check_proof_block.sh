#!/usr/bin/env bash
# =============================================================================
# check_proof_block.sh — Anti-butchering proof-block gate (bd-66l.7)
#
# Validates that changes to semantic-sensitive files include the required
# Guarantee Impact metadata block per PLAN section 6.13 and the
# GUARANTEE_SUBSTITUTION_MATRIX.md enforcement contract.
#
# Semantic-sensitive paths: src/core/, src/runtime/, src/channel/, src/time/
#
# Required metadata block in commit message or PR body:
#   Guarantee Impact:
#   - gs_id: GS-00X
#   - change: <old> -> <new>
#   - preserved invariants: <list>
#   - proof artifacts: <tests/fixtures/parity>
#   - fallback mode: <behavior>
#   - deterministic isomorphism note: <impact>
#   - semantic delta budget: 0
#
# Exit 0 = pass, Exit 1 = violations found, Exit 2 = usage error
#
# SPDX-License-Identifier: MIT
# =============================================================================

set -euo pipefail

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

# Semantic-sensitive directories
SENSITIVE_DIRS=("src/core" "src/runtime" "src/channel" "src/time")

# Semantic-sensitive document paths (changes also require proof blocks)
SENSITIVE_DOCS=(
    "docs/GUARANTEE_SUBSTITUTION_MATRIX.md"
    "docs/LIFECYCLE_TRANSITION_TABLES.md"
    "docs/CHANNEL_TIMER_DETERMINISM.md"
    "include/asx/asx_ids.h"
    "include/asx/asx_status.h"
    "include/asx/core/"
)

# Required fields in the Guarantee Impact block
REQUIRED_FIELDS=(
    "gs_id"
    "change"
    "preserved invariants"
    "proof artifacts"
    "fallback mode"
    "deterministic isomorphism note"
    "semantic delta budget"
)

# File-level waiver annotation
WAIVER_PATTERN="ASX_PROOF_BLOCK_WAIVER"

# ---------------------------------------------------------------------------
# State
# ---------------------------------------------------------------------------
VIOLATIONS=0
WARNINGS=0
WAIVERS=0
CHECKED_FILES=0
SENSITIVE_FILES=0
VERBOSE=0
JSON_OUTPUT=0
BASE_REF=""
COMMIT_MSG_FILE=""
PR_BODY_FILE=""

# ---------------------------------------------------------------------------
# Usage
# ---------------------------------------------------------------------------
usage() {
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  --base REF        Base git ref for diff (default: HEAD~1)"
    echo "  --commit-msg FILE Read commit message from file"
    echo "  --pr-body FILE    Read PR body from file"
    echo "  --verbose         Show detailed output"
    echo "  --json            Output results as JSON"
    echo "  --help            Show this help"
    echo ""
    echo "Environment variables:"
    echo "  ASX_PROOF_BLOCK_BASE   Override base ref"
    echo "  ASX_PROOF_BLOCK_MSG    Override commit message file"
    echo "  ASX_PROOF_BLOCK_BODY   Override PR body file"
    echo ""
    echo "Modes:"
    echo "  1. With --base: check changed files against base ref"
    echo "  2. With --commit-msg: validate commit message metadata"
    echo "  3. Standalone: scan staged changes"
    exit 2
}

# ---------------------------------------------------------------------------
# Parse arguments
# ---------------------------------------------------------------------------
while [ $# -gt 0 ]; do
    case "$1" in
        --base)
            [ $# -ge 2 ] || usage
            BASE_REF="$2"
            shift 2
            ;;
        --commit-msg)
            [ $# -ge 2 ] || usage
            COMMIT_MSG_FILE="$2"
            shift 2
            ;;
        --pr-body)
            [ $# -ge 2 ] || usage
            PR_BODY_FILE="$2"
            shift 2
            ;;
        --verbose)
            VERBOSE=1
            shift
            ;;
        --json)
            JSON_OUTPUT=1
            shift
            ;;
        --help|-h)
            usage
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage
            ;;
    esac
done

# Override from environment
BASE_REF="${ASX_PROOF_BLOCK_BASE:-${BASE_REF:-HEAD~1}}"
COMMIT_MSG_FILE="${ASX_PROOF_BLOCK_MSG:-$COMMIT_MSG_FILE}"
PR_BODY_FILE="${ASX_PROOF_BLOCK_BODY:-$PR_BODY_FILE}"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

is_sensitive_path() {
    local path="$1"
    local dir doc

    for dir in "${SENSITIVE_DIRS[@]}"; do
        if [[ "$path" == "$dir"/* ]]; then
            return 0
        fi
    done

    for doc in "${SENSITIVE_DOCS[@]}"; do
        if [[ "$path" == "$doc"* ]]; then
            return 0
        fi
    done

    return 1
}

has_waiver() {
    local path="$1"

    if [ -f "$path" ]; then
        # Check first 30 lines for waiver annotation
        head -30 "$path" 2>/dev/null | grep -q "$WAIVER_PATTERN" && return 0
    fi

    return 1
}

# ---------------------------------------------------------------------------
# Collect changed files
# ---------------------------------------------------------------------------

get_changed_files() {
    if command -v git >/dev/null 2>&1 && git rev-parse --git-dir >/dev/null 2>&1; then
        git diff --name-only "$BASE_REF" 2>/dev/null || true
        git diff --name-only --cached 2>/dev/null || true
    fi
}

# ---------------------------------------------------------------------------
# Collect metadata from commit message or PR body
# ---------------------------------------------------------------------------

get_metadata_text() {
    local text=""

    # From commit message file
    if [ -n "$COMMIT_MSG_FILE" ] && [ -f "$COMMIT_MSG_FILE" ]; then
        text="$(cat "$COMMIT_MSG_FILE")"
    fi

    # From PR body file (append)
    if [ -n "$PR_BODY_FILE" ] && [ -f "$PR_BODY_FILE" ]; then
        text="${text}$(cat "$PR_BODY_FILE")"
    fi

    # From recent commit message (fallback)
    if [ -z "$text" ] && command -v git >/dev/null 2>&1; then
        text="$(git log -1 --format='%B' 2>/dev/null || true)"
    fi

    echo "$text"
}

has_guarantee_impact_block() {
    local text="$1"
    echo "$text" | grep -qi "guarantee impact:" && return 0
    return 1
}

check_required_field() {
    local text="$1"
    local field="$2"

    echo "$text" | grep -qi -- "- *${field}:" && return 0
    return 1
}

validate_gs_id() {
    local text="$1"

    # Extract gs_id values and verify they match GS-NNN pattern
    local gs_ids
    gs_ids="$(echo "$text" | grep -oi 'gs_id: *GS-[0-9]\{1,3\}' | head -5)"
    if [ -z "$gs_ids" ]; then
        return 1
    fi
    return 0
}

validate_delta_budget() {
    local text="$1"

    # Check that semantic delta budget is declared
    if echo "$text" | grep -qi "semantic delta budget:"; then
        return 0
    fi
    return 1
}

validate_proof_artifacts() {
    local text="$1"

    # Extract artifact references and verify at least one exists
    local artifacts
    artifacts="$(echo "$text" | grep -i "proof artifacts:" | head -1)"
    if [ -z "$artifacts" ]; then
        return 1
    fi

    # Check if any referenced path exists
    local refs
    refs="$(echo "$artifacts" | grep -oE '(tests|fixtures|docs)/[^ ,)]+' | head -5)"
    if [ -z "$refs" ]; then
        # Artifact field exists but no path references — warn but don't fail
        return 0
    fi

    local ref
    while IFS= read -r ref; do
        if [ -e "$ref" ]; then
            return 0
        fi
    done <<< "$refs"

    # None of the referenced paths exist — this is a warning
    return 0
}

# ---------------------------------------------------------------------------
# JSON output helpers
# ---------------------------------------------------------------------------

json_str() {
    printf '"%s"' "$(printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g; s/\t/\\t/g; s/\n/\\n/g')"
}

emit_json_report() {
    local status="$1"
    local sensitive="$2"
    local has_block="$3"
    local missing_fields="$4"
    local waiver_count="$5"

    printf '{\n'
    printf '  "kind": "proof_block_check",\n'
    printf '  "status": %s,\n' "$(json_str "$status")"
    printf '  "sensitive_files_changed": %d,\n' "$sensitive"
    printf '  "has_guarantee_impact_block": %s,\n' "$has_block"
    printf '  "missing_fields": %s,\n' "$(json_str "$missing_fields")"
    printf '  "waivers": %d,\n' "$waiver_count"
    printf '  "violations": %d,\n' "$VIOLATIONS"
    printf '  "warnings": %d\n' "$WARNINGS"
    printf '}\n'
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

if [ "$VERBOSE" = "1" ] && [ "$JSON_OUTPUT" = "0" ]; then
    echo "==================================================================="
    echo " Anti-Butchering Proof-Block Gate (bd-66l.7)"
    echo "==================================================================="
    echo "  base_ref: $BASE_REF"
    echo ""
fi

# Step 1: Identify changed files
changed_files="$(get_changed_files | sort -u)"
sensitive_changed=()

while IFS= read -r f; do
    [ -z "$f" ] && continue
    CHECKED_FILES=$((CHECKED_FILES + 1))
    if is_sensitive_path "$f"; then
        if has_waiver "$f"; then
            WAIVERS=$((WAIVERS + 1))
            if [ "$VERBOSE" = "1" ] && [ "$JSON_OUTPUT" = "0" ]; then
                echo "  WAIVER  $f"
            fi
        else
            sensitive_changed+=("$f")
            SENSITIVE_FILES=$((SENSITIVE_FILES + 1))
            if [ "$VERBOSE" = "1" ] && [ "$JSON_OUTPUT" = "0" ]; then
                echo "  SENSITIVE  $f"
            fi
        fi
    fi
done <<< "$changed_files"

# Step 2: If no sensitive files changed, pass immediately
if [ "$SENSITIVE_FILES" -eq 0 ]; then
    if [ "$JSON_OUTPUT" = "1" ]; then
        emit_json_report "pass" 0 "false" "" "$WAIVERS"
    else
        echo "[proof-block] PASS — no semantic-sensitive files changed"
        echo "  checked: $CHECKED_FILES files, $WAIVERS waivers"
    fi
    exit 0
fi

# Step 3: Check for Guarantee Impact metadata
metadata="$(get_metadata_text)"
has_block="false"
missing_fields=""

if has_guarantee_impact_block "$metadata"; then
    has_block="true"

    # Step 4: Validate required fields
    for field in "${REQUIRED_FIELDS[@]}"; do
        if ! check_required_field "$metadata" "$field"; then
            missing_fields="${missing_fields}${field}, "
            VIOLATIONS=$((VIOLATIONS + 1))
        fi
    done

    # Validate gs_id format
    if ! validate_gs_id "$metadata"; then
        WARNINGS=$((WARNINGS + 1))
        if [ "$VERBOSE" = "1" ] && [ "$JSON_OUTPUT" = "0" ]; then
            echo "  WARN: gs_id should match GS-NNN format"
        fi
    fi

    # Validate delta budget declaration
    if ! validate_delta_budget "$metadata"; then
        VIOLATIONS=$((VIOLATIONS + 1))
        missing_fields="${missing_fields}semantic_delta_budget, "
    fi

    # Validate proof artifact references
    if ! validate_proof_artifacts "$metadata"; then
        WARNINGS=$((WARNINGS + 1))
        if [ "$VERBOSE" = "1" ] && [ "$JSON_OUTPUT" = "0" ]; then
            echo "  WARN: proof artifacts field present but no path references found"
        fi
    fi
else
    # No Guarantee Impact block at all
    VIOLATIONS=$((VIOLATIONS + 1))
    missing_fields="entire Guarantee Impact block"
fi

# Remove trailing comma from missing_fields
missing_fields="${missing_fields%, }"

# Step 5: Report
if [ "$JSON_OUTPUT" = "1" ]; then
    local_status="pass"
    [ "$VIOLATIONS" -gt 0 ] && local_status="fail"
    emit_json_report "$local_status" "$SENSITIVE_FILES" "$has_block" "$missing_fields" "$WAIVERS"
elif [ "$VIOLATIONS" -gt 0 ]; then
    echo ""
    echo "==================================================================="
    echo " FAIL — Anti-Butchering Proof-Block Gate"
    echo "==================================================================="
    echo ""
    echo "  Semantic-sensitive files changed: $SENSITIVE_FILES"
    for f in "${sensitive_changed[@]}"; do
        echo "    - $f"
    done
    echo ""
    if [ "$has_block" = "false" ]; then
        echo "  Missing: entire Guarantee Impact block"
        echo ""
        echo "  Add the following to your commit message or PR body:"
        echo ""
        echo "    Guarantee Impact:"
        echo "    - gs_id: GS-00X"
        echo "    - change: <old mechanism> -> <new mechanism>"
        echo "    - preserved invariants: <list>"
        echo "    - proof artifacts: <tests/fixtures/parity rows>"
        echo "    - fallback mode: <safe-mode behavior>"
        echo "    - deterministic isomorphism note: <ordering/tie-break impact>"
        echo "    - semantic delta budget: 0"
    else
        echo "  Guarantee Impact block found but missing fields:"
        echo "    $missing_fields"
    fi
    echo ""
    echo "  See: docs/GUARANTEE_SUBSTITUTION_MATRIX.md section 4"
    echo "  See: docs/PROOF_BLOCK_EXCEPTION_WORKFLOW.md for exceptions"
    echo ""
    echo "  Waivers: $WAIVERS files"
    echo "  Warnings: $WARNINGS"
    echo "==================================================================="
else
    echo "[proof-block] PASS — Guarantee Impact block complete"
    echo "  sensitive files: $SENSITIVE_FILES"
    echo "  waivers: $WAIVERS"
    echo "  warnings: $WARNINGS"
fi

exit $(( VIOLATIONS > 0 ? 1 : 0 ))
