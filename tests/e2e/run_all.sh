#!/usr/bin/env bash
# run_all.sh — canonical e2e aggregation script (bd-1md.10)
#
# Runs ALL e2e scenario families, collects per-family summaries, and
# emits a unified run manifest with git rev, compiler/target, seed,
# scenario IDs, profile/codec matrix, and first-divergence pointers.
#
# One-command reproducible:
#   ./tests/e2e/run_all.sh
#   ASX_E2E_SEED=99 ASX_E2E_PROFILE=HFT ./tests/e2e/run_all.sh
#
# Environment contract (inherited by all families via harness.sh):
#   ASX_E2E_SEED          Deterministic seed (default: 42)
#   ASX_E2E_PROFILE       Profile under test (default: CORE)
#   ASX_E2E_CODEC         Codec under test (default: json)
#   ASX_E2E_SCENARIO_PACK Scenario pack filter (default: all)
#   ASX_E2E_RESOURCE_CLASS Resource class (default: R3)
#   ASX_E2E_RUN_ID        Override run ID (default: auto)
#   ASX_E2E_ARTIFACT_DIR  Override artifact directory
#   ASX_E2E_LOG_DIR       Override log directory
#   ASX_E2E_STRICT        Strict mode (default: 0)
#   ASX_E2E_VERBOSE       Verbose output (default: 0)
#   ASX_E2E_FAMILY_TIMEOUT Seconds before a family is failed as hung (default: 600)
#   ASX_E2E_SELFTEST_TIMEOUT 1 = run only a generated family that sleeps past
#                          the timeout; the suite must report it FAIL (timeout)
#
# The suite also fails if any family rewrites build/lib/libasx.a: families
# that build other profiles must use their own BUILD_DIR.
#
# Hard gate mapping:
#   GATE-E2E-FOUNDATIONAL  foundational_contracts.sh
#   GATE-E2E-LIFECYCLE     core_lifecycle.sh
#   GATE-E2E-LIFECYCLE     nested_regions.sh
#   GATE-E2E-CODEC         codec_parity.sh
#   GATE-E2E-ROBUSTNESS    robustness.sh, robustness_fault.sh,
#                           robustness_endian.sh, robustness_exhaustion.sh
#   GATE-E2E-NATIVE-HOST   native_host.sh
#   GATE-E2E-SERVER        server_shutdown.sh
#   GATE-E2E-PARALLEL-SWARM parallel_swarm.sh
#   GATE-E2E-EXAMPLES      examples_smoke.sh
#   GATE-E2E-BROWSER       browser_smoke.sh
#   GATE-E2E-VERTICAL-HFT  hft_microburst.sh
#   GATE-E2E-VERTICAL-AUTO automotive_watchdog.sh
#   GATE-E2E-CONTINUITY    continuity.sh, continuity_restart.sh
#   GATE-E2E-DEPLOY-ROUTER router_storm.sh
#   GATE-E2E-DEPLOY-HFT   market_open_burst.sh
#   GATE-E2E-DEPLOY-AUTO   automotive_fault_burst.sh
#   GATE-E2E-PACKAGE       openwrt_package.sh
#
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# -------------------------------------------------------------------
# Timestamp and run ID
# -------------------------------------------------------------------

RUN_TIMESTAMP="$(date -u '+%Y%m%dT%H%M%SZ')"
RUN_ID="${ASX_E2E_RUN_ID:-e2e-suite-${RUN_TIMESTAMP}}"
export ASX_E2E_RUN_ID="$RUN_ID"

# -------------------------------------------------------------------
# Environment detection
# -------------------------------------------------------------------

detect_git_rev() {
    if command -v git >/dev/null 2>&1 && git rev-parse --git-dir >/dev/null 2>&1; then
        git -C "$PROJECT_ROOT" rev-parse --short=12 HEAD 2>/dev/null || echo "unknown"
    else
        echo "unknown"
    fi
}

detect_compiler() {
    local cc="${CC:-gcc}"
    local ver=""
    ver="$($cc --version 2>/dev/null | head -1)" || ver="unknown"
    echo "$cc ($ver)"
}

detect_target() {
    local cc="${CC:-gcc}"
    $cc -dumpmachine 2>/dev/null || uname -m 2>/dev/null || echo "unknown"
}

GIT_REV="$(detect_git_rev)"
COMPILER="$(detect_compiler)"
TARGET="$(detect_target)"
SEED="${ASX_E2E_SEED:-42}"
PROFILE="${ASX_E2E_PROFILE:-CORE}"
CODEC="${ASX_E2E_CODEC:-json}"
RESOURCE_CLASS="${ASX_E2E_RESOURCE_CLASS:-R3}"

# -------------------------------------------------------------------
# Artifact directories
# -------------------------------------------------------------------

SUITE_ARTIFACT_DIR="${ASX_E2E_ARTIFACT_DIR:-${PROJECT_ROOT}/build/e2e-artifacts/${RUN_ID}}"
SUITE_LOG_DIR="${ASX_E2E_LOG_DIR:-${PROJECT_ROOT}/build/test-logs}"
mkdir -p "$SUITE_ARTIFACT_DIR" "$SUITE_LOG_DIR"

E2E_FAMILY_TIMEOUT="${ASX_E2E_FAMILY_TIMEOUT:-600}"
E2E_TIMEOUT_CMD=""
if command -v timeout >/dev/null 2>&1; then
    E2E_TIMEOUT_CMD="timeout"
elif command -v gtimeout >/dev/null 2>&1; then
    E2E_TIMEOUT_CMD="gtimeout"
else
    echo "  NOTE: no timeout(1) available; families run without a hang guard"
fi

export ASX_E2E_ARTIFACT_DIR="$SUITE_ARTIFACT_DIR"
export ASX_E2E_LOG_DIR="$SUITE_LOG_DIR"

# -------------------------------------------------------------------
# E2E family registry — maps gate IDs to scripts
# -------------------------------------------------------------------

# Family entries: gate_id:script_name
E2E_FAMILIES=(
    "GATE-E2E-FOUNDATIONAL:foundational_contracts.sh"
    "GATE-E2E-LIFECYCLE:core_lifecycle.sh"
    "GATE-E2E-LIFECYCLE:nested_regions.sh"
    "GATE-E2E-CODEC:codec_parity.sh"
    "GATE-E2E-NETWORK:network_surface.sh"
    "GATE-E2E-ROBUSTNESS:robustness.sh"
    "GATE-E2E-ROBUSTNESS:robustness_fault.sh"
    "GATE-E2E-ROBUSTNESS:robustness_endian.sh"
    "GATE-E2E-ROBUSTNESS:robustness_exhaustion.sh"
    "GATE-E2E-NATIVE-HOST:native_host.sh"
    "GATE-E2E-SERVER:server_shutdown.sh"
    "GATE-E2E-PARALLEL-SWARM:parallel_swarm.sh"
    "GATE-E2E-EXAMPLES:examples_smoke.sh"
    "GATE-E2E-BROWSER:browser_smoke.sh"
    "GATE-E2E-VERTICAL-HFT:hft_microburst.sh"
    "GATE-E2E-VERTICAL-AUTO:automotive_watchdog.sh"
    "GATE-E2E-CONTINUITY:continuity.sh"
    "GATE-E2E-CONTINUITY:continuity_restart.sh"
    "GATE-E2E-DEPLOY-ROUTER:router_storm.sh"
    "GATE-E2E-DEPLOY-HFT:market_open_burst.sh"
    "GATE-E2E-DEPLOY-AUTO:automotive_fault_burst.sh"
    "GATE-E2E-PACKAGE:openwrt_package.sh"
)

# Opt-in self-test of the hang guard: replace the registry with one generated
# family that outlives ASX_E2E_FAMILY_TIMEOUT.
if [ "${ASX_E2E_SELFTEST_TIMEOUT:-0}" = "1" ]; then
    selftest_script="${SUITE_ARTIFACT_DIR}/selftest_hang.sh"
    # exec: timeout(1) signals the sleep itself, so no orphan outlives the test.
    printf '#!/usr/bin/env bash\nexec sleep %d\n' "$((E2E_FAMILY_TIMEOUT + 600))" >"$selftest_script"
    chmod +x "$selftest_script"
    E2E_FAMILIES=("GATE-E2E-SELFTEST:${selftest_script}")
fi

# -------------------------------------------------------------------
# JSON helper
# -------------------------------------------------------------------

json_str() {
    printf '"%s"' "$(printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g; s/\t/\\t/g')"
}

# -------------------------------------------------------------------
# Run all families
# -------------------------------------------------------------------

echo "================================================================="
echo " ASX E2E Suite — ${RUN_ID}"
echo "================================================================="
echo "  git_rev:   ${GIT_REV}"
echo "  compiler:  ${COMPILER}"
echo "  target:    ${TARGET}"
echo "  profile:   ${PROFILE}"
echo "  codec:     ${CODEC}"
echo "  seed:      ${SEED}"
echo "  class:     ${RESOURCE_CLASS}"
echo "  artifacts: ${SUITE_ARTIFACT_DIR}"
echo "================================================================="
echo ""

lib_digest() {
    if [ -f "${PROJECT_ROOT}/build/lib/libasx.a" ]; then
        { sha256sum "${PROJECT_ROOT}/build/lib/libasx.a" 2>/dev/null ||
            shasum -a 256 "${PROJECT_ROOT}/build/lib/libasx.a"; } | cut -d' ' -f1
    else
        echo "absent"
    fi
}
lib_digest_before="$(lib_digest)"

total_families=0
passed_families=0
failed_families=0
first_failure_family=""
first_failure_log=""
family_results=""  # accumulated JSON array entries

for entry in "${E2E_FAMILIES[@]}"; do
    gate="${entry%%:*}"
    script="${entry##*:}"
    if [[ "$script" = /* ]]; then
        script_path="$script"
        script="$(basename "$script")"
    else
        script_path="${SCRIPT_DIR}/${script}"
    fi

    if [ ! -x "$script_path" ]; then
        echo "  SKIP ${script} (not executable)"
        family_results="${family_results}$(printf '    {"gate": %s, "script": %s, "status": "skip"},\n' \
            "$(json_str "$gate")" "$(json_str "$script")")"
        continue
    fi

    total_families=$((total_families + 1))
    family_log="${SUITE_LOG_DIR}/${script%.sh}.log"

    printf "  RUN  %-35s [%s] " "$script" "$gate"

    # A hung family must fail with its log instead of stalling the suite
    # until the outer runner's kill. 124 is timeout(1)'s exit status.
    family_start=$(date +%s)
    set +e
    if [ -n "$E2E_TIMEOUT_CMD" ]; then
        "$E2E_TIMEOUT_CMD" "$E2E_FAMILY_TIMEOUT" "$script_path" > "$family_log" 2>&1
    else
        "$script_path" > "$family_log" 2>&1
    fi
    family_rc=$?
    set -e
    family_secs=$(( $(date +%s) - family_start ))

    if [ "$family_rc" -eq 0 ]; then
        echo "PASS (${family_secs}s)"
        passed_families=$((passed_families + 1))
        family_results="${family_results}$(printf '    {"gate": %s, "script": %s, "status": "pass", "seconds": %d, "log": %s},\n' \
            "$(json_str "$gate")" "$(json_str "$script")" "$family_secs" "$(json_str "$family_log")")"
    else
        if [ "$family_rc" -eq 124 ] && [ -n "$E2E_TIMEOUT_CMD" ]; then
            echo "FAIL (timeout after ${E2E_FAMILY_TIMEOUT}s)"
        else
            echo "FAIL (rc=${family_rc}, ${family_secs}s)"
        fi
        failed_families=$((failed_families + 1))
        if [ -z "$first_failure_family" ]; then
            first_failure_family="$script"
            first_failure_log="$family_log"
        fi
        family_results="${family_results}$(printf '    {"gate": %s, "script": %s, "status": "fail", "exit_code": %d, "seconds": %d, "log": %s},\n' \
            "$(json_str "$gate")" "$(json_str "$script")" "$family_rc" "$family_secs" "$(json_str "$family_log")")"
    fi
done

# -------------------------------------------------------------------
# Collect per-family summary manifests
# -------------------------------------------------------------------

summary_files=""
for f in "$SUITE_ARTIFACT_DIR"/*.summary.json; do
    [ -f "$f" ] && summary_files="${summary_files}$(printf '    %s,\n' "$(json_str "$f")")" || true
done
# Strip trailing comma+newline
summary_files="$(echo "$summary_files" | sed '$ s/,$//')"

# Strip trailing comma from family_results
family_results="$(echo "$family_results" | sed '$ s/,$//')"

# -------------------------------------------------------------------
# Emit unified run manifest
# -------------------------------------------------------------------

MANIFEST_FILE="${SUITE_ARTIFACT_DIR}/run_manifest.json"

{
    printf '{\n'
    printf '  "run_id": %s,\n' "$(json_str "$RUN_ID")"
    printf '  "timestamp": %s,\n' "$(json_str "$RUN_TIMESTAMP")"
    printf '  "git_rev": %s,\n' "$(json_str "$GIT_REV")"
    printf '  "compiler": %s,\n' "$(json_str "$COMPILER")"
    printf '  "target": %s,\n' "$(json_str "$TARGET")"
    printf '  "profile": %s,\n' "$(json_str "$PROFILE")"
    printf '  "codec": %s,\n' "$(json_str "$CODEC")"
    printf '  "seed": %d,\n' "$SEED"
    printf '  "resource_class": %s,\n' "$(json_str "$RESOURCE_CLASS")"
    printf '  "scenario_pack": %s,\n' "$(json_str "${ASX_E2E_SCENARIO_PACK:-all}")"
    printf '  "total_families": %d,\n' "$total_families"
    printf '  "passed_families": %d,\n' "$passed_families"
    printf '  "failed_families": %d,\n' "$failed_families"
    printf '  "status": %s,\n' "$(json_str "$([ "$failed_families" -gt 0 ] && echo "fail" || echo "pass")")"
    printf '  "families": [\n'
    printf '%s\n' "$family_results"
    printf '  ],\n'
    if [ -n "$first_failure_family" ]; then
        printf '  "first_failure": {\n'
        printf '    "family": %s,\n' "$(json_str "$first_failure_family")"
        printf '    "log": %s,\n' "$(json_str "$first_failure_log")"
        printf '    "rerun": %s\n' "$(json_str "ASX_E2E_SEED=${SEED} ASX_E2E_PROFILE=${PROFILE} ASX_E2E_CODEC=${CODEC} ASX_E2E_RESOURCE_CLASS=${RESOURCE_CLASS} ${SCRIPT_DIR}/${first_failure_family}")"
        printf '  },\n'
    fi
    printf '  "family_summaries": [\n'
    printf '%s\n' "$summary_files"
    printf '  ],\n'
    printf '  "artifact_dir": %s,\n' "$(json_str "$SUITE_ARTIFACT_DIR")"
    printf '  "log_dir": %s\n' "$(json_str "$SUITE_LOG_DIR")"
    printf '}\n'
} > "$MANIFEST_FILE"

# -------------------------------------------------------------------
# Final summary
# -------------------------------------------------------------------

echo ""
echo "================================================================="
echo " E2E Suite Summary"
echo "================================================================="
echo "  families: ${passed_families}/${total_families} passed, ${failed_families} failed"
echo "  manifest: ${MANIFEST_FILE}"
echo "  logs:     ${SUITE_LOG_DIR}/"
echo "  artifacts: ${SUITE_ARTIFACT_DIR}/"

if [ -n "$first_failure_family" ]; then
    echo ""
    echo "  FIRST FAILURE: ${first_failure_family}"
    echo "    log: ${first_failure_log}"
    echo "    rerun: ASX_E2E_SEED=${SEED} ASX_E2E_PROFILE=${PROFILE} ASX_E2E_CODEC=${CODEC} ASX_E2E_RESOURCE_CLASS=${RESOURCE_CLASS} ${SCRIPT_DIR}/${first_failure_family}"
fi

echo "================================================================="

lib_digest_after="$(lib_digest)"
if [ "$lib_digest_before" != "absent" ] && [ "$lib_digest_after" != "$lib_digest_before" ]; then
    echo "  FAIL: a family rewrote build/lib/libasx.a (${lib_digest_before} -> ${lib_digest_after});"
    echo "        builds for other profiles must use their own BUILD_DIR"
    exit 1
fi

if [ "$failed_families" -gt 0 ]; then
    exit 1
fi
exit 0
