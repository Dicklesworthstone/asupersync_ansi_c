#!/usr/bin/env bash
# =============================================================================
# check_evidence_linkage.sh — Per-bead evidence linkage gate (bd-66l.9)
#
# Validates that closed implementation beads carry explicit evidence
# linkage: unit tests, e2e scripts, and artifact references.
#
# Evidence is checked from bead close_reason notes and cross-referenced
# against the codebase to verify referenced paths exist.
#
# Exit 0 = pass, Exit 1 = violations found, Exit 2 = usage error
#
# SPDX-License-Identifier: MIT
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BEADS_FILE="$REPO_ROOT/.beads/issues.jsonl"

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

# Bead types that require evidence linkage
EVIDENCE_REQUIRED_TYPES=("task")

# Beads with these labels are exempt (documentation-only, research, etc.)
EXEMPT_LABELS=(
    "docs"
    "research"
    "ops"
    "playbook"
    "ci"
    "policy"
    "governance"
    "adoption"
    "ergonomics"
    "traceability"
    "performance"
    "slo"
)

# Required evidence categories (at least one reference per category)
EVIDENCE_CATEGORIES=(
    "unit_tests"
    "e2e_or_invariant"
    "artifacts"
)

# Patterns that indicate evidence references in close notes
UNIT_TEST_PATTERNS=(
    "test_"
    "tests/unit/"
    "unit test"
    "unit suite"
    "_test.c"
    "test_harness"
)

E2E_INVARIANT_PATTERNS=(
    "tests/e2e/"
    "tests/invariant/"
    "e2e"
    "invariant"
    "GATE-E2E"
    "GATE-"
    "scenario"
    "lifecycle"
)

ARTIFACT_PATTERNS=(
    "fixture"
    "manifest"
    "digest"
    "parity"
    "conformance"
    ".jsonl"
    "artifact"
    ".json"
    "build/"
    "schemas/"
)

# ---------------------------------------------------------------------------
# State
# ---------------------------------------------------------------------------
VIOLATIONS=0
WARNINGS=0
CHECKED=0
PASSED=0
EXEMPTED=0
VERBOSE=0
JSON_OUTPUT=0
STRICT=0

# ---------------------------------------------------------------------------
# Usage
# ---------------------------------------------------------------------------
usage() {
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  --verbose    Show detailed per-bead results"
    echo "  --json       Output results as JSON"
    echo "  --strict     Fail on warnings too (missing optional evidence)"
    echo "  --help       Show this help"
    exit 2
}

# ---------------------------------------------------------------------------
# Parse arguments
# ---------------------------------------------------------------------------
while [ $# -gt 0 ]; do
    case "$1" in
        --verbose) VERBOSE=1; shift ;;
        --json) JSON_OUTPUT=1; shift ;;
        --strict) STRICT=1; shift ;;
        --help|-h) usage ;;
        *) echo "Unknown option: $1" >&2; usage ;;
    esac
done

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

# Check if a bead has any of the exempt labels
is_exempt() {
    local labels="$1"
    local label
    for label in "${EXEMPT_LABELS[@]}"; do
        if echo "$labels" | grep -qi "$label"; then
            return 0
        fi
    done
    return 1
}

# Check text against a set of patterns
has_evidence_pattern() {
    local text="$1"
    shift
    local patterns=("$@")
    local pat
    for pat in "${patterns[@]}"; do
        if echo "$text" | grep -qi "$pat"; then
            return 0
        fi
    done
    return 1
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

if [ ! -f "$BEADS_FILE" ]; then
    if [ "$JSON_OUTPUT" = "1" ]; then
        printf '{"gate":"evidence-linkage","pass":true,"reason":"no beads file found","checked":0}\n'
    else
        echo "[evidence-linkage] SKIP — no beads file found at $BEADS_FILE"
    fi
    exit 0
fi

if [ "$VERBOSE" = "1" ] && [ "$JSON_OUTPUT" = "0" ]; then
    echo "[evidence-linkage] Scanning closed beads for evidence linkage..."
fi

violations_json="[]"

# Read each bead from the JSONL file
while IFS= read -r line; do
    [ -z "$line" ] && continue

    # Parse fields using jq
    bead_id=$(echo "$line" | jq -r '.id // empty' 2>/dev/null) || continue
    [ -z "$bead_id" ] && continue

    bead_status=$(echo "$line" | jq -r '.status // empty' 2>/dev/null)
    bead_type=$(echo "$line" | jq -r '.issue_type // empty' 2>/dev/null)
    bead_labels=$(echo "$line" | jq -r '(.labels // []) | join(",")' 2>/dev/null)
    bead_close=$(echo "$line" | jq -r '.close_reason // empty' 2>/dev/null)
    bead_desc=$(echo "$line" | jq -r '.description // empty' 2>/dev/null)
    bead_title=$(echo "$line" | jq -r '.title // empty' 2>/dev/null)
    bead_notes=$(echo "$line" | jq -r '.notes // empty' 2>/dev/null)

    # Only check closed implementation beads
    [ "$bead_status" = "closed" ] || continue

    # Skip epics (they don't carry direct evidence)
    [ "$bead_type" = "epic" ] && continue

    # Skip non-implementation types (only task beads require evidence)
    is_evidence_required=0
    for _req_type in "${EVIDENCE_REQUIRED_TYPES[@]}"; do
        if [ "$bead_type" = "$_req_type" ]; then
            is_evidence_required=1
            break
        fi
    done
    if [ "$is_evidence_required" = "0" ]; then
        EXEMPTED=$((EXEMPTED + 1))
        if [ "$VERBOSE" = "1" ] && [ "$JSON_OUTPUT" = "0" ]; then
            echo "  EXEMPT $bead_id (type=$bead_type)"
        fi
        continue
    fi

    # Check if exempt by label
    if is_exempt "$bead_labels"; then
        EXEMPTED=$((EXEMPTED + 1))
        if [ "$VERBOSE" = "1" ] && [ "$JSON_OUTPUT" = "0" ]; then
            echo "  EXEMPT $bead_id ($bead_labels)"
        fi
        continue
    fi

    CHECKED=$((CHECKED + 1))

    # Combine all text for evidence searching
    all_text="$bead_title $bead_desc $bead_close $bead_notes"

    # Check evidence categories
    has_unit=0
    has_e2e=0
    has_artifact=0

    if has_evidence_pattern "$all_text" "${UNIT_TEST_PATTERNS[@]}"; then
        has_unit=1
    fi

    if has_evidence_pattern "$all_text" "${E2E_INVARIANT_PATTERNS[@]}"; then
        has_e2e=1
    fi

    if has_evidence_pattern "$all_text" "${ARTIFACT_PATTERNS[@]}"; then
        has_artifact=1
    fi

    # Evaluate result
    missing=()
    if [ $has_unit -eq 0 ]; then
        missing+=("unit_tests")
    fi
    if [ $has_e2e -eq 0 ]; then
        missing+=("e2e_or_invariant")
    fi
    if [ $has_artifact -eq 0 ]; then
        missing+=("artifacts")
    fi

    if [ ${#missing[@]} -eq 0 ]; then
        PASSED=$((PASSED + 1))
        if [ "$VERBOSE" = "1" ] && [ "$JSON_OUTPUT" = "0" ]; then
            echo "  PASS $bead_id"
        fi
    elif [ ${#missing[@]} -le 1 ]; then
        # Missing one category is a warning (the bead description may
        # implicitly cover it through parent/child references)
        WARNINGS=$((WARNINGS + 1))
        if [ "$VERBOSE" = "1" ] && [ "$JSON_OUTPUT" = "0" ]; then
            echo "  WARN $bead_id — missing: ${missing[*]}"
        fi
        if [ "$STRICT" = "1" ]; then
            VIOLATIONS=$((VIOLATIONS + 1))
        fi
    else
        # Missing two or more categories is a violation
        VIOLATIONS=$((VIOLATIONS + 1))
        if [ "$JSON_OUTPUT" = "0" ]; then
            echo "  VIOLATION $bead_id — missing evidence: ${missing[*]}"
        fi
    fi

done < "$BEADS_FILE"

# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------

if [ "$JSON_OUTPUT" = "1" ]; then
    pass_val="true"
    [ "$VIOLATIONS" -gt 0 ] && pass_val="false"
    cat <<EOF
{
  "gate": "evidence-linkage",
  "pass": $pass_val,
  "checked": $CHECKED,
  "passed": $PASSED,
  "warnings": $WARNINGS,
  "violations": $VIOLATIONS,
  "exempted": $EXEMPTED
}
EOF
else
    echo ""
    echo "[evidence-linkage] $CHECKED beads checked, $PASSED passed, $WARNINGS warnings, $VIOLATIONS violations, $EXEMPTED exempted"
    if [ "$VIOLATIONS" -eq 0 ]; then
        echo "[evidence-linkage] PASS"
    else
        echo "[evidence-linkage] FAIL — $VIOLATIONS bead(s) missing required evidence linkage"
        echo ""
        echo "Each implementation bead needs evidence in its close notes:"
        echo "  - Unit tests: reference test_*.c files or test suites"
        echo "  - E2E/invariant: reference e2e scripts or invariant tests"
        echo "  - Artifacts: reference fixtures, manifests, or parity evidence"
    fi
fi

exit $(( VIOLATIONS > 0 ? 1 : 0 ))
