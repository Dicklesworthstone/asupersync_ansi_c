#!/usr/bin/env bash
# harness.sh — shared e2e harness contract for scenario packs (bd-1md.20)
#
# Source this file from any e2e script to get standardized:
#   - CLI/env contract parsing (seed, profile, codec, scenario-pack, etc.)
#   - Artifact directory layout
#   - Manifest emission (JSONL + summary)
#   - First-failure summary generation
#   - Rerun command generation
#   - Integration with structured log schema (bd-1md.11)
#
# Usage in e2e scripts:
#   #!/usr/bin/env bash
#   SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
#   source "${SCRIPT_DIR}/../harness.sh"
#   e2e_init "core-lifecycle" "E2E-CORE-LIFECYCLE"
#   e2e_scenario "region_open_close" "..." pass
#   e2e_finish
#
# Environment / CLI contract:
#   ASX_E2E_SEED          Deterministic seed (default: 42)
#   ASX_E2E_PROFILE       Profile under test (default: CORE)
#   ASX_E2E_CODEC         Codec under test (default: json)
#   ASX_E2E_SCENARIO_PACK Scenario pack filter (default: all)
#   ASX_E2E_RESOURCE_CLASS Resource class for embedded (default: R3)
#   ASX_E2E_RUN_ID        Override run ID (default: auto-generated)
#   ASX_E2E_ARTIFACT_DIR  Override artifact directory
#   ASX_E2E_LOG_DIR       Override log directory (default: build/test-logs)
#   ASX_E2E_STRICT        Strict mode: fail on warnings (default: 0)
#   ASX_E2E_VERBOSE       Verbose output (default: 0)
#
# SPDX-License-Identifier: MIT

set -euo pipefail

# -------------------------------------------------------------------
# Environment contract defaults
# -------------------------------------------------------------------

E2E_SEED="${ASX_E2E_SEED:-42}"
E2E_PROFILE="${ASX_E2E_PROFILE:-CORE}"
E2E_CODEC="${ASX_E2E_CODEC:-json}"
E2E_SCENARIO_PACK="${ASX_E2E_SCENARIO_PACK:-all}"
E2E_RESOURCE_CLASS="${ASX_E2E_RESOURCE_CLASS:-R3}"
E2E_POLICY_ID="${ASX_E2E_POLICY_ID:-}"
E2E_STRICT="${ASX_E2E_STRICT:-0}"
E2E_VERBOSE="${ASX_E2E_VERBOSE:-0}"

# Detect project root
if [ -z "${E2E_PROJECT_ROOT:-}" ]; then
    # Walk up from this file's location
    E2E_PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
fi

# -------------------------------------------------------------------
# Run ID and timestamp
# -------------------------------------------------------------------

E2E_TIMESTAMP="$(date -u '+%Y%m%dT%H%M%SZ')"
E2E_RUN_ID="${ASX_E2E_RUN_ID:-e2e-${E2E_TIMESTAMP}}"

# -------------------------------------------------------------------
# Environment detection (git rev, compiler, target)
# -------------------------------------------------------------------

_e2e_detect_git_rev() {
    if command -v git >/dev/null 2>&1 && git rev-parse --git-dir >/dev/null 2>&1; then
        git -C "$E2E_PROJECT_ROOT" rev-parse --short=12 HEAD 2>/dev/null || echo "unknown"
    else
        echo "unknown"
    fi
}

_e2e_detect_compiler() {
    local cc="${CC:-gcc}"
    local ver=""
    ver="$($cc --version 2>/dev/null | head -1)" || ver="unknown"
    echo "$cc ($ver)"
}

_e2e_detect_target() {
    local cc="${CC:-gcc}"
    $cc -dumpmachine 2>/dev/null || uname -m 2>/dev/null || echo "unknown"
}

E2E_GIT_REV="$(_e2e_detect_git_rev)"
E2E_COMPILER="$(_e2e_detect_compiler)"
E2E_TARGET="$(_e2e_detect_target)"

# -------------------------------------------------------------------
# Artifact and log directories
# -------------------------------------------------------------------

E2E_ARTIFACT_DIR="${ASX_E2E_ARTIFACT_DIR:-${E2E_PROJECT_ROOT}/build/e2e-artifacts/${E2E_RUN_ID}}"
E2E_LOG_DIR="${ASX_E2E_LOG_DIR:-${E2E_PROJECT_ROOT}/build/test-logs}"

# -------------------------------------------------------------------
# Internal state
# -------------------------------------------------------------------

_E2E_FAMILY_ID=""
_E2E_LANE_ID=""
_E2E_REPORT_FILE=""
_E2E_SUMMARY_FILE=""
_E2E_TOTAL=0
_E2E_PASS=0
_E2E_FAIL=0
_E2E_SKIP=0
_E2E_FIRST_FAILURE=""
_E2E_FIRST_FAILURE_SCENARIO=""
E2E_LAST_OUTPUT=""

# -------------------------------------------------------------------
# JSON helpers
# -------------------------------------------------------------------

_e2e_json_str() {
    # Minimal JSON string escaping
    printf '"%s"' "$(printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g; s/\t/\\t/g')"
}

_e2e_emit_record() {
    local scenario_id="$1"
    local status="$2"
    local diagnostic="${3:-}"
    local digest="${4:-}"

    local ts
    ts="$(date -u '+%Y-%m-%dT%H:%M:%SZ')"

    printf '{' >> "$_E2E_REPORT_FILE"
    printf '"ts":%s' "$(_e2e_json_str "$ts")" >> "$_E2E_REPORT_FILE"
    printf ',"run_id":%s' "$(_e2e_json_str "$E2E_RUN_ID")" >> "$_E2E_REPORT_FILE"
    printf ',"layer":"e2e"' >> "$_E2E_REPORT_FILE"
    printf ',"subsystem":%s' "$(_e2e_json_str "$_E2E_FAMILY_ID")" >> "$_E2E_REPORT_FILE"
    printf ',"suite":%s' "$(_e2e_json_str "$_E2E_LANE_ID")" >> "$_E2E_REPORT_FILE"
    if [ -n "$E2E_POLICY_ID" ]; then
        printf ',"policy_id":%s' "$(_e2e_json_str "$E2E_POLICY_ID")" >> "$_E2E_REPORT_FILE"
    fi
    printf ',"test":%s' "$(_e2e_json_str "$scenario_id")" >> "$_E2E_REPORT_FILE"
    printf ',"status":%s' "$(_e2e_json_str "$status")" >> "$_E2E_REPORT_FILE"
    printf ',"event_index":%d' "$_E2E_TOTAL" >> "$_E2E_REPORT_FILE"
    printf ',"profile":%s' "$(_e2e_json_str "$E2E_PROFILE")" >> "$_E2E_REPORT_FILE"
    printf ',"codec":%s' "$(_e2e_json_str "$E2E_CODEC")" >> "$_E2E_REPORT_FILE"
    printf ',"seed":%d' "$E2E_SEED" >> "$_E2E_REPORT_FILE"

    if [ -n "$digest" ]; then
        printf ',"digest":%s' "$(_e2e_json_str "$digest")" >> "$_E2E_REPORT_FILE"
    fi
    if [ -n "$diagnostic" ]; then
        printf ',"error":{"message":%s}' "$(_e2e_json_str "$diagnostic")" >> "$_E2E_REPORT_FILE"
    fi

    printf '}\n' >> "$_E2E_REPORT_FILE"
}

# -------------------------------------------------------------------
# Initialization
# -------------------------------------------------------------------

e2e_init() {
    local family_id="$1"
    local lane_id="$2"

    _E2E_FAMILY_ID="$family_id"
    _E2E_LANE_ID="$lane_id"
    _E2E_TOTAL=0
    _E2E_PASS=0
    _E2E_FAIL=0
    _E2E_SKIP=0
    _E2E_FIRST_FAILURE=""
    _E2E_FIRST_FAILURE_SCENARIO=""

    # Create directories
    mkdir -p "$E2E_ARTIFACT_DIR"
    mkdir -p "$E2E_LOG_DIR"

    # Report file
    _E2E_REPORT_FILE="${E2E_LOG_DIR}/e2e-${family_id}.jsonl"
    _E2E_SUMMARY_FILE="${E2E_ARTIFACT_DIR}/${family_id}.summary.json"

    if [ "$E2E_VERBOSE" = "1" ]; then
        echo "[e2e] init family=${family_id} lane=${lane_id}"
        echo "[e2e] profile=${E2E_PROFILE} codec=${E2E_CODEC} seed=${E2E_SEED}"
        echo "[e2e] artifacts=${E2E_ARTIFACT_DIR}"
        echo "[e2e] report=${_E2E_REPORT_FILE}"
    fi
}

# -------------------------------------------------------------------
# Scenario result recording
# -------------------------------------------------------------------

e2e_scenario() {
    local scenario_id="$1"
    local diagnostic="${2:-}"
    local status="${3:-pass}"
    local digest="${4:-}"

    _E2E_TOTAL=$((_E2E_TOTAL + 1))

    case "$status" in
        pass) _E2E_PASS=$((_E2E_PASS + 1)) ;;
        fail)
            _E2E_FAIL=$((_E2E_FAIL + 1))
            if [ -z "$_E2E_FIRST_FAILURE" ]; then
                _E2E_FIRST_FAILURE="$diagnostic"
                _E2E_FIRST_FAILURE_SCENARIO="$scenario_id"
            fi
            ;;
        skip) _E2E_SKIP=$((_E2E_SKIP + 1)) ;;
    esac

    _e2e_emit_record "$scenario_id" "$status" "$diagnostic" "$digest"

    if [ "$E2E_VERBOSE" = "1" ]; then
        printf "  %-6s %s\n" "$(echo "$status" | tr '[:lower:]' '[:upper:]')" "$scenario_id"
    fi
}

# -------------------------------------------------------------------
# Build helper: compile and link a C test against libasx
# -------------------------------------------------------------------

e2e_build() {
    local source="$1"
    local output="$2"
    local extra_flags="${3:-}"
    local lib="${E2E_PROJECT_ROOT}/build/lib/libasx.a"
    local cc="${CC:-gcc}"

    local cflags="-std=c99 -Wall -Wextra -Wpedantic -Werror"
    cflags="$cflags -I${E2E_PROJECT_ROOT}/include"
    cflags="$cflags -I${E2E_PROJECT_ROOT}/tests"
    cflags="$cflags -I${E2E_PROJECT_ROOT}/src"
    cflags="$cflags -DASX_PROFILE_${E2E_PROFILE}"
    cflags="$cflags -DASX_CODEC_$(echo "$E2E_CODEC" | tr '[:lower:]' '[:upper:]')"
    cflags="$cflags -DASX_DETERMINISTIC=1"
    cflags="$cflags $extra_flags"

    if ! $cc $cflags -o "$output" "$source" "$lib" 2>"${output}.build.log"; then
        echo "[e2e] BUILD FAIL: $source" >&2
        cat "${output}.build.log" >&2
        return 1
    fi
    return 0
}

# -------------------------------------------------------------------
# Run helper: execute e2e binary, parse scenario lines, and hard-fail
# on runtime exit or malformed output.
# -------------------------------------------------------------------

e2e_run_binary() {
    local binary="$1"
    local stderr_file="$2"
    local scenario_prefix="$3"
    local output=""
    local rc=0
    local parsed=0

    set +e
    output="$("$binary" 2>"$stderr_file")"
    rc=$?
    set -e

    while IFS= read -r line; do
        if [[ "$line" =~ ^SCENARIO[[:space:]]+([-a-zA-Z0-9_.]+)[[:space:]]+(pass|fail)(.*)$ ]]; then
            scenario_id="${BASH_REMATCH[1]}"
            status="${BASH_REMATCH[2]}"
            diag="${BASH_REMATCH[3]}"
            diag="${diag# }"
            e2e_scenario "$scenario_id" "$diag" "$status"
            parsed=1
        fi
    done <<< "$output"

    E2E_LAST_OUTPUT="$output"

    if [ "$rc" -ne 0 ]; then
        e2e_scenario "${scenario_prefix}.runtime_exit" \
            "binary exited with code ${rc}; see ${stderr_file}" "fail"
    fi

    if [ "$parsed" -eq 0 ]; then
        e2e_scenario "${scenario_prefix}.no_scenarios" \
            "binary emitted no SCENARIO lines" "fail"
    fi

    return 0
}

# -------------------------------------------------------------------
# Rerun command generation
# -------------------------------------------------------------------

e2e_rerun_command() {
    local scenario_id="$1"
    local script_path="${2:-$0}"

    echo "ASX_E2E_SEED=${E2E_SEED} ASX_E2E_PROFILE=${E2E_PROFILE} ASX_E2E_CODEC=${E2E_CODEC} ASX_E2E_SCENARIO_PACK=${scenario_id} ${script_path}"
}

# -------------------------------------------------------------------
# First-failure summary
# -------------------------------------------------------------------

_e2e_first_failure_summary() {
    if [ -n "$_E2E_FIRST_FAILURE_SCENARIO" ]; then
        echo ""
        echo "[e2e] FIRST FAILURE: ${_E2E_FIRST_FAILURE_SCENARIO}"
        echo "[e2e]   diagnostic: ${_E2E_FIRST_FAILURE}"
        echo "[e2e]   rerun: $(e2e_rerun_command "$_E2E_FIRST_FAILURE_SCENARIO")"
    fi
}

# -------------------------------------------------------------------
# Manifest summary emission
# -------------------------------------------------------------------

e2e_finish() {
    local exit_status=0

    # Build first-failure block for manifest
    local _ff_scenario="" _ff_diag="" _ff_rerun=""
    if [ -n "$_E2E_FIRST_FAILURE_SCENARIO" ]; then
        _ff_scenario="$_E2E_FIRST_FAILURE_SCENARIO"
        _ff_diag="$_E2E_FIRST_FAILURE"
        _ff_rerun="$(e2e_rerun_command "$_E2E_FIRST_FAILURE_SCENARIO")"
    fi

    # Emit summary JSON (enhanced run manifest)
    {
        printf '{\n'
        printf '  "run_id": %s,\n' "$(_e2e_json_str "$E2E_RUN_ID")"
        printf '  "timestamp": %s,\n' "$(_e2e_json_str "$E2E_TIMESTAMP")"
        printf '  "git_rev": %s,\n' "$(_e2e_json_str "$E2E_GIT_REV")"
        printf '  "compiler": %s,\n' "$(_e2e_json_str "$E2E_COMPILER")"
        printf '  "target": %s,\n' "$(_e2e_json_str "$E2E_TARGET")"
        printf '  "family_id": %s,\n' "$(_e2e_json_str "$_E2E_FAMILY_ID")"
        printf '  "lane_id": %s,\n' "$(_e2e_json_str "$_E2E_LANE_ID")"
        if [ -n "$E2E_POLICY_ID" ]; then
            printf '  "policy_id": %s,\n' "$(_e2e_json_str "$E2E_POLICY_ID")"
        fi
        printf '  "profile": %s,\n' "$(_e2e_json_str "$E2E_PROFILE")"
        printf '  "codec": %s,\n' "$(_e2e_json_str "$E2E_CODEC")"
        printf '  "seed": %d,\n' "$E2E_SEED"
        printf '  "resource_class": %s,\n' "$(_e2e_json_str "$E2E_RESOURCE_CLASS")"
        printf '  "scenario_pack": %s,\n' "$(_e2e_json_str "$E2E_SCENARIO_PACK")"
        printf '  "total": %d,\n' "$_E2E_TOTAL"
        printf '  "pass": %d,\n' "$_E2E_PASS"
        printf '  "fail": %d,\n' "$_E2E_FAIL"
        printf '  "skip": %d,\n' "$_E2E_SKIP"
        printf '  "status": %s,\n' "$(_e2e_json_str "$([ "$_E2E_FAIL" -gt 0 ] && echo "fail" || echo "pass")")"
        if [ -n "$_ff_scenario" ]; then
            printf '  "first_failure": {\n'
            printf '    "scenario": %s,\n' "$(_e2e_json_str "$_ff_scenario")"
            printf '    "diagnostic": %s,\n' "$(_e2e_json_str "$_ff_diag")"
            printf '    "rerun": %s\n' "$(_e2e_json_str "$_ff_rerun")"
            printf '  },\n'
        fi
        printf '  "report_file": %s,\n' "$(_e2e_json_str "$_E2E_REPORT_FILE")"
        printf '  "artifact_dir": %s\n' "$(_e2e_json_str "$E2E_ARTIFACT_DIR")"
        printf '}\n'
    } > "$_E2E_SUMMARY_FILE"

    # Also emit summary as a log record
    _e2e_emit_record "_summary" \
        "$([ "$_E2E_FAIL" -gt 0 ] && echo "fail" || echo "pass")" \
        "total=${_E2E_TOTAL} pass=${_E2E_PASS} fail=${_E2E_FAIL} skip=${_E2E_SKIP}"

    # Human-readable output
    echo ""
    echo "[e2e] ${_E2E_FAMILY_ID}: ${_E2E_PASS}/${_E2E_TOTAL} passed, ${_E2E_FAIL} failed, ${_E2E_SKIP} skipped"
    echo "[e2e] report: ${_E2E_REPORT_FILE}"
    echo "[e2e] summary: ${_E2E_SUMMARY_FILE}"

    if [ "$_E2E_FAIL" -gt 0 ]; then
        _e2e_first_failure_summary
        exit_status=1
    fi

    if [ "$E2E_STRICT" = "1" ] && [ "$_E2E_SKIP" -gt 0 ]; then
        echo "[e2e] STRICT: ${_E2E_SKIP} skipped scenarios treated as failures"
        exit_status=1
    fi

    return $exit_status
}

# -------------------------------------------------------------------
# Scenario filter: check if scenario should run
# -------------------------------------------------------------------

e2e_should_run() {
    local scenario_id="$1"

    if [ "$E2E_SCENARIO_PACK" = "all" ]; then
        return 0
    fi

    # Exact match or prefix match
    case "$scenario_id" in
        ${E2E_SCENARIO_PACK}*) return 0 ;;
        *) return 1 ;;
    esac
}
