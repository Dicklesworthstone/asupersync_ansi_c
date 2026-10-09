#!/usr/bin/env bash
# =============================================================================
# check_anti_butchering.sh — semantic-sensitive proof-block gate (bd-66l.7)
#
# Fails when semantic-sensitive changes land without a complete "Guarantee
# Impact" metadata block and linked evidence.
#
# Artifact:
#   build/conformance/anti_butcher_<run_id>.json
#
# Exit codes:
#   0: pass or skip
#   1: gate failure (missing/invalid proof block)
#   2: usage/configuration error
#
# SPDX-License-Identifier: MIT
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
RUN_ID="${ASX_CI_RUN_TAG:-anti-butchering-$(date -u +%Y%m%dT%H%M%SZ)}"
ARTIFACT_DIR="$REPO_ROOT/build/conformance"
ARTIFACT_FILE="$ARTIFACT_DIR/anti_butcher_${RUN_ID}.json"

BASE_REF="${ASX_ANTI_BUTCHER_BASE_REF:-}"
HEAD_REF="${ASX_ANTI_BUTCHER_HEAD_REF:-}"

usage() {
    cat <<'EOF'
Usage: tools/ci/check_anti_butchering.sh [--base <ref>] [--head <ref>] [--run-id <id>]

Options:
  --base <ref>    Git base ref for diff range (default: auto-detect)
  --head <ref>    Git head ref for diff range (default: auto-detect)
  --run-id <id>   Override run id used in artifact filename
  --help          Show this help and exit
EOF
    exit 2
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --base)
            [[ $# -ge 2 ]] || usage
            BASE_REF="$2"
            shift 2
            ;;
        --head)
            [[ $# -ge 2 ]] || usage
            HEAD_REF="$2"
            shift 2
            ;;
        --run-id)
            [[ $# -ge 2 ]] || usage
            RUN_ID="$2"
            ARTIFACT_FILE="$ARTIFACT_DIR/anti_butcher_${RUN_ID}.json"
            shift 2
            ;;
        --help|-h)
            usage
            ;;
        *)
            echo "[asx] anti-butchering: unknown option: $1" >&2
            usage
            ;;
    esac
done

mkdir -p "$ARTIFACT_DIR"

tmp_changed="$(mktemp)"
trap 'rm -f "$tmp_changed"' EXIT

# The resolved diff range. RANGE_UNRESOLVED=1 means a range was requested but
# could not be computed (unknown commit, shallow clone): that is a failure,
# never a skip, because an empty changed-file list would pass every change.
RANGE_BASE=""
RANGE_HEAD=""
RANGE_UNRESOLVED=0

commit_exists() {
    git -C "$REPO_ROOT" cat-file -e "${1}^{commit}" 2>/dev/null
}

add_changed_files_from_range() {
    local base="$1"
    local head="$2"
    if [[ -n "$base" && -n "$head" ]]; then
        if ! commit_exists "$base" || ! commit_exists "$head"; then
            RANGE_UNRESOLVED=1
            return
        fi
        RANGE_BASE="$base"
        RANGE_HEAD="$head"
        git -C "$REPO_ROOT" diff --name-only "${base}...${head}" 2>/dev/null >>"$tmp_changed" ||
            RANGE_UNRESOLVED=1
        return
    fi
    if [[ -n "$base" ]]; then
        if ! commit_exists "$base"; then
            RANGE_UNRESOLVED=1
            return
        fi
        git -C "$REPO_ROOT" diff --name-only "${base}" 2>/dev/null >>"$tmp_changed" ||
            RANGE_UNRESOLVED=1
    fi
}

collect_changed_files() {
    # Explicit refs win.
    if [[ -n "$BASE_REF" || -n "$HEAD_REF" ]]; then
        add_changed_files_from_range "$BASE_REF" "$HEAD_REF"
    elif [[ -n "${GITHUB_EVENT_PATH:-}" && -f "${GITHUB_EVENT_PATH:-}" ]]; then
        local event_base event_head
        event_base="$(jq -r '.pull_request.base.sha // ""' "$GITHUB_EVENT_PATH" 2>/dev/null || true)"
        event_head="$(jq -r '.pull_request.head.sha // ""' "$GITHUB_EVENT_PATH" 2>/dev/null || true)"
        if [[ -z "$event_base" ]]; then
            # Push event: the pushed range is before..after. A branch creation
            # has an all-zero `before`; judge its tip commit.
            event_base="$(jq -r '.before // ""' "$GITHUB_EVENT_PATH" 2>/dev/null || true)"
            event_head="$(jq -r '.after // ""' "$GITHUB_EVENT_PATH" 2>/dev/null || true)"
            if [[ "$event_base" =~ ^0+$ && -n "$event_head" ]]; then
                event_base="${event_head}~1"
            fi
        fi
        if [[ -z "$event_base" || -z "$event_head" ]]; then
            RANGE_UNRESOLVED=1
        else
            add_changed_files_from_range "$event_base" "$event_head"
        fi
    else
        # Developer run: the last commit plus local and staged diffs. A
        # requested range (above) is judged on its own, never on whatever
        # happens to be uncommitted in the working tree.
        if git -C "$REPO_ROOT" rev-parse --verify HEAD~1 >/dev/null 2>&1; then
            git -C "$REPO_ROOT" diff --name-only HEAD~1..HEAD 2>/dev/null >>"$tmp_changed" || true
        fi
        git -C "$REPO_ROOT" diff --name-only 2>/dev/null >>"$tmp_changed" || true
        git -C "$REPO_ROOT" diff --name-only --cached 2>/dev/null >>"$tmp_changed" || true
    fi
}

collect_changed_files

mapfile -t changed_files < <(awk 'NF {print $0}' "$tmp_changed" | sort -u)

is_sensitive_path() {
    local path="$1"
    [[ "$path" =~ ^src/(core|runtime|channel|time)/ ]] && return 0
    [[ "$path" =~ ^include/asx/ ]] && return 0
    [[ "$path" =~ ^tests/invariant/ ]] && return 0
    [[ "$path" =~ ^fixtures/rust_reference/ ]] && return 0
    [[ "$path" =~ ^docs/(EXISTING_ASUPERSYNC_STRUCTURE|LIFECYCLE_TRANSITION_TABLES|INVARIANT_SCHEMA|GUARANTEE_SUBSTITUTION_MATRIX|FEATURE_PARITY|PLAN_EXECUTION_TRACEABILITY_INDEX|RISK_CONTROLS_SEMANTIC_FIDELITY)\.md$ ]] && return 0
    return 1
}

sensitive_files=()
for path in "${changed_files[@]}"; do
    if is_sensitive_path "$path"; then
        sensitive_files+=("$path")
    fi
done

write_artifact() {
    local status="$1"
    local pass="$2"
    local source="$3"
    local reason="$4"
    local missing_json="$5"
    local trace_json="$6"
    local proof_json="$7"
    local proof_existing_json="$8"
    local budget_value="$9"
    local budget_exception_required="${10}"
    local budget_exception_missing_json="${11}"
    local artifact_links_json="${12}"

    local changed_json sensitive_json
    if [[ ${#changed_files[@]} -eq 0 ]]; then
        changed_json='[]'
    else
        changed_json="$(printf '%s\n' "${changed_files[@]}" | jq -R . | jq -s .)"
    fi
    if [[ ${#sensitive_files[@]} -eq 0 ]]; then
        sensitive_json='[]'
    else
        sensitive_json="$(printf '%s\n' "${sensitive_files[@]}" | jq -R . | jq -s .)"
    fi

    jq -n \
      --arg kind "anti_butchering_proof_block" \
      --arg run_id "$RUN_ID" \
      --arg status "$status" \
      --arg source "$source" \
      --arg reason "$reason" \
      --arg rerun "tools/ci/check_anti_butchering.sh --base <base> --head <head>" \
      --argjson anti_butchering_pass "$pass" \
      --argjson changed_files "$changed_json" \
      --argjson sensitive_files "$sensitive_json" \
      --argjson missing_fields "$missing_json" \
      --argjson traceability_refs "$trace_json" \
      --argjson proof_artifact_paths "$proof_json" \
      --argjson proof_artifact_existing_paths "$proof_existing_json" \
      --arg budget_value "$budget_value" \
      --argjson budget_exception_required "$budget_exception_required" \
      --argjson budget_exception_missing_fields "$budget_exception_missing_json" \
      --argjson artifact_links "$artifact_links_json" \
      '{
        kind: $kind,
        run_id: $run_id,
        status: $status,
        anti_butchering_pass: $anti_butchering_pass,
        source: $source,
        reason: $reason,
        rerun: $rerun,
        changed_files: $changed_files,
        sensitive_files: $sensitive_files,
        missing_fields: $missing_fields,
        traceability_refs: $traceability_refs,
        proof_artifact_paths: $proof_artifact_paths,
        proof_artifact_existing_paths: $proof_artifact_existing_paths,
        semantic_delta_budget: {
          value: $budget_value,
          exception_required: $budget_exception_required,
          exception_missing_fields: $budget_exception_missing_fields
        },
        artifact_links: $artifact_links
      }' >"$ARTIFACT_FILE"
}

if [[ $RANGE_UNRESOLVED -eq 1 ]]; then
    write_artifact "fail" false "none" \
        "cannot resolve the diff range (unknown commit or shallow checkout)" \
        '[]' '[]' '[]' '[]' '0' false '[]' '[]'
    echo "[asx] anti-butchering: FAIL — cannot resolve the diff range; check out full history (fetch-depth: 0) artifact=$ARTIFACT_FILE" >&2
    exit 1
fi

if [[ ${#sensitive_files[@]} -eq 0 ]]; then
    write_artifact "skip" true "none" \
        "no semantic-sensitive changes detected" \
        '[]' '[]' '[]' '[]' '0' false '[]' '[]'
    echo "[asx] anti-butchering: status=skip artifact=$ARTIFACT_FILE" >&2
    exit 0
fi

impact_text=""
impact_source="none"

if [[ -n "${ASX_GUARANTEE_IMPACT_TEXT:-}" ]]; then
    impact_text="${ASX_GUARANTEE_IMPACT_TEXT}"
    impact_source="env"
fi

if [[ -z "$impact_text" && -n "${GITHUB_EVENT_PATH:-}" && -f "${GITHUB_EVENT_PATH:-}" ]]; then
    pr_body="$(jq -r '.pull_request.body // ""' "$GITHUB_EVENT_PATH" 2>/dev/null || true)"
    if [[ -n "$pr_body" ]]; then
        impact_text="$pr_body"
        impact_source="pull_request_body"
    fi
fi

# A resolved range (push or explicit refs) is judged by every commit message in
# it, so a multi-commit push is not judged by its last commit alone.
if [[ -z "$impact_text" && -n "$RANGE_BASE" && -n "$RANGE_HEAD" ]]; then
    impact_text="$(git -C "$REPO_ROOT" log --pretty=%B "${RANGE_BASE}..${RANGE_HEAD}" 2>/dev/null || true)"
    if [[ -n "$impact_text" ]]; then
        impact_source="commit_messages_in_range"
    fi
fi

if [[ -z "$impact_text" ]]; then
    impact_text="$(git -C "$REPO_ROOT" log -1 --pretty=%B 2>/dev/null || true)"
    if [[ -n "$impact_text" ]]; then
        impact_source="commit_message"
    fi
fi

missing_fields=()
require_pattern() {
    local field="$1"
    local pattern="$2"
    if ! printf '%s\n' "$impact_text" | grep -Eiq "$pattern"; then
        missing_fields+=("$field")
    fi
}

require_pattern "gs_id" '[[:space:]-]*gs[_ -]?id:[[:space:]]*GS-[0-9]{3}'
require_pattern "change" '[[:space:]-]*change:[[:space:]]*.+'
require_pattern "preserved_invariants" '[[:space:]-]*preserved[[:space:]]+invariants:[[:space:]]*.+'
require_pattern "proof_artifacts" '[[:space:]-]*proof[[:space:]]+artifacts:[[:space:]]*.+'
require_pattern "fallback_mode" '[[:space:]-]*fallback[[:space:]]+mode:[[:space:]]*.+'
require_pattern "deterministic_isomorphism_note" '[[:space:]-]*deterministic[[:space:]]+isomorphism[[:space:]]+note:[[:space:]]*.+'
require_pattern "semantic_delta_budget" '[[:space:]-]*semantic[[:space:]]+delta[[:space:]]+budget:[[:space:]]*.+'
require_pattern "traceability_refs" '[[:space:]-]*traceability[[:space:]]+refs:[[:space:]]*.+'
require_pattern "artifact_links" '[[:space:]-]*artifact[[:space:]]+links:[[:space:]]*.+'

mapfile -t trace_refs < <(printf '%s\n' "$impact_text" | grep -Eo 'TRC-[A-Z0-9-]+' | sort -u || true)
if [[ ${#trace_refs[@]} -eq 0 ]]; then
    missing_fields+=("traceability_refs_values")
fi

proof_line="$(printf '%s\n' "$impact_text" | grep -Ei '^[[:space:]-]*proof[[:space:]]+artifacts:[[:space:]]*.+$' | head -n1 || true)"
mapfile -t proof_paths < <(printf '%s\n' "$proof_line" | grep -Eo '[A-Za-z0-9._/-]+\.[A-Za-z0-9]+' | sort -u || true)
proof_existing=()
for p in "${proof_paths[@]}"; do
    if [[ -f "$REPO_ROOT/$p" ]]; then
        proof_existing+=("$p")
    fi
done
if [[ ${#proof_existing[@]} -eq 0 ]]; then
    missing_fields+=("proof_artifacts_existing_paths")
fi

artifact_line="$(printf '%s\n' "$impact_text" | grep -Ei '^[[:space:]-]*artifact[[:space:]]+links:[[:space:]]*.+$' | head -n1 || true)"
mapfile -t artifact_links < <(printf '%s\n' "$artifact_line" | grep -Eo '(build/[A-Za-z0-9._/-]+|tools/ci/artifacts/[A-Za-z0-9._/-]+)' | sort -u || true)
if [[ ${#artifact_links[@]} -eq 0 ]]; then
    missing_fields+=("artifact_links_values")
fi

budget_value="missing"
budget_exception_required=false
budget_exception_missing=()
budget_line="$(printf '%s\n' "$impact_text" | grep -Ei '^[[:space:]-]*semantic[[:space:]]+delta[[:space:]]+budget:[[:space:]]*.+$' | head -n1 || true)"
if [[ -n "$budget_line" ]]; then
    budget_value="$(printf '%s\n' "$budget_line" | sed -E 's/^[^:]+:[[:space:]]*//')"
fi

if [[ "$budget_value" != "missing" && "$budget_value" != "0" && "$budget_value" != "0.0" ]]; then
    budget_exception_required=true
    if ! printf '%s\n' "$impact_text" | grep -Eiq '^[[:space:]-]*exception[[:space:]]+id:[[:space:]]*.+$'; then
        budget_exception_missing+=("exception_id")
    fi
    if ! printf '%s\n' "$impact_text" | grep -Eiq '^[[:space:]-]*exception[[:space:]]+rationale:[[:space:]]*.+$'; then
        budget_exception_missing+=("exception_rationale")
    fi
    if ! printf '%s\n' "$impact_text" | grep -Eiq '^[[:space:]-]*exception[[:space:]]+approver:[[:space:]]*.+$'; then
        budget_exception_missing+=("exception_approver")
    fi
fi

if [[ ${#missing_fields[@]} -eq 0 ]]; then
    missing_json='[]'
else
    missing_json="$(printf '%s\n' "${missing_fields[@]}" | jq -R . | jq -s .)"
fi
if [[ ${#trace_refs[@]} -eq 0 ]]; then
    trace_json='[]'
else
    trace_json="$(printf '%s\n' "${trace_refs[@]}" | jq -R . | jq -s .)"
fi
if [[ ${#proof_paths[@]} -eq 0 ]]; then
    proof_json='[]'
else
    proof_json="$(printf '%s\n' "${proof_paths[@]}" | jq -R . | jq -s .)"
fi
if [[ ${#proof_existing[@]} -eq 0 ]]; then
    proof_existing_json='[]'
else
    proof_existing_json="$(printf '%s\n' "${proof_existing[@]}" | jq -R . | jq -s .)"
fi
if [[ ${#budget_exception_missing[@]} -eq 0 ]]; then
    budget_missing_json='[]'
else
    budget_missing_json="$(printf '%s\n' "${budget_exception_missing[@]}" | jq -R . | jq -s .)"
fi
if [[ ${#artifact_links[@]} -eq 0 ]]; then
    artifact_links_json='[]'
else
    artifact_links_json="$(printf '%s\n' "${artifact_links[@]}" | jq -R . | jq -s .)"
fi

if [[ ${#missing_fields[@]} -gt 0 || ${#budget_exception_missing[@]} -gt 0 ]]; then
    write_artifact "fail" false "$impact_source" \
        "semantic-sensitive change missing required proof-block metadata/evidence" \
        "$missing_json" "$trace_json" "$proof_json" "$proof_existing_json" \
        "$budget_value" "$budget_exception_required" "$budget_missing_json" "$artifact_links_json"
    echo "[asx] anti-butchering: FAIL artifact=$ARTIFACT_FILE" >&2
    echo "[asx] anti-butchering: missing fields: ${missing_fields[*]:-none}" >&2
    if [[ ${#budget_exception_missing[@]} -gt 0 ]]; then
        echo "[asx] anti-butchering: missing exception fields: ${budget_exception_missing[*]}" >&2
    fi
    exit 1
fi

write_artifact "pass" true "$impact_source" \
    "semantic-sensitive change has complete proof-block metadata and links" \
    "$missing_json" "$trace_json" "$proof_json" "$proof_existing_json" \
    "$budget_value" "$budget_exception_required" "$budget_missing_json" "$artifact_links_json"
echo "[asx] anti-butchering: PASS artifact=$ARTIFACT_FILE" >&2
exit 0
