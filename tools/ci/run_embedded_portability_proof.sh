#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

RUN_ID="embedded-portability-$(date -u +%Y%m%dT%H%M%SZ)"
BUILD_DIR="${BUILD_DIR:-build}"
STRICT="${ASX_PORTABILITY_STRICT:-0}"
OUTPUT=""
ARTIFACT_DIR=""

usage() {
  cat <<'EOF'
Usage: run_embedded_portability_proof.sh [--run-id <id>] [--output <file>] [--artifact-dir <dir>] [--strict]

Builds a compact embedded portability proof:
  - EMBEDDED_ROUTER/R1 deterministic probe with semantic digest evidence
  - host alignment/portability compile canary
  - optional -m32 portability canary with explicit unsupported-platform evidence
  - profile-parity evidence link

Environment:
  CC=<compiler>              C compiler (default: cc)
  BUILD_DIR=<dir>            build output root (default: build)
  ASX_PORTABILITY_STRICT=1   treat unsupported optional lanes as failures
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --run-id)
      RUN_ID="$2"
      shift 2
      ;;
    --output)
      OUTPUT="$2"
      shift 2
      ;;
    --artifact-dir)
      ARTIFACT_DIR="$2"
      shift 2
      ;;
    --strict)
      STRICT=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "[asx] embedded-portability-proof: unknown argument '$1'" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ "$BUILD_DIR" != /* ]]; then
  BUILD_DIR="$REPO_ROOT/$BUILD_DIR"
fi
if [[ -z "$ARTIFACT_DIR" ]]; then
  ARTIFACT_DIR="$BUILD_DIR/embedded-portability/$RUN_ID"
elif [[ "$ARTIFACT_DIR" != /* ]]; then
  ARTIFACT_DIR="$REPO_ROOT/$ARTIFACT_DIR"
fi
if [[ -z "$OUTPUT" ]]; then
  OUTPUT="$ARTIFACT_DIR/embedded_portability_proof.json"
elif [[ "$OUTPUT" != /* ]]; then
  OUTPUT="$REPO_ROOT/$OUTPUT"
fi

JSONL="$ARTIFACT_DIR/embedded_portability_proof.jsonl"
mkdir -p "$ARTIFACT_DIR"
: >"$JSONL"

if ! command -v jq >/dev/null 2>&1; then
  echo "[asx] embedded-portability-proof: FAIL (jq is required)" >&2
  exit 2
fi

CC_BIN="${CC:-cc}"
COMPILER_VERSION="$($CC_BIN --version 2>/dev/null | head -1 || echo "$CC_BIN")"
TARGET_TRIPLE="$($CC_BIN -dumpmachine 2>/dev/null || uname -m 2>/dev/null || echo unknown)"
GIT_REV="$(git -C "$REPO_ROOT" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"

join_command() {
  printf '%q ' "$@"
}

emit_step() {
  local step="$1"
  local status="$2"
  local command="$3"
  local artifact="$4"
  local diagnostic="$5"
  local extra="${6:-}"
  local extra_file="$ARTIFACT_DIR/embedded-portability-step-extra.json"

  if [[ -z "$extra" ]]; then
    extra="{}"
  fi

  printf '%s\n' "$extra" >"$extra_file"
  if ! jq -e 'type == "object"' "$extra_file" >/dev/null; then
    echo "[asx] embedded-portability-proof: invalid step metadata for '$step'" >&2
    exit 2
  fi

  jq -n -c \
    --arg kind "embedded_portability_step" \
    --arg run_id "$RUN_ID" \
    --arg step "$step" \
    --arg status "$status" \
    --arg profile "ASX_PROFILE_EMBEDDED_ROUTER" \
    --arg resource_class "ASX_CLASS_R1" \
    --arg compiler "$COMPILER_VERSION" \
    --arg target "$TARGET_TRIPLE" \
    --arg command "$command" \
    --arg artifact "$artifact" \
    --arg diagnostic "$diagnostic" \
    --slurpfile extra "$extra_file" \
    '{
      kind: $kind,
      run_id: $run_id,
      step: $step,
      status: $status,
      profile: $profile,
      resource_class: $resource_class,
      compiler: $compiler,
      target_triple: $target,
      command: $command,
      artifact: $artifact,
      diagnostic: $diagnostic
    } + ($extra[0] // {})' >>"$JSONL"
}

ROUTER_BUILD_DIR="$ARTIFACT_DIR/build-router-r1"
ROUTER_BUILD_LOG="$ARTIFACT_DIR/embedded-router-r1-build.log"
router_build_cmd=(make -C "$REPO_ROOT" build "BUILD_DIR=$ROUTER_BUILD_DIR" PROFILE=EMBEDDED_ROUTER CODEC=BIN DETERMINISTIC=1)

set +e
"${router_build_cmd[@]}" >"$ROUTER_BUILD_LOG" 2>&1
router_build_rc=$?
set -e
if [[ "$router_build_rc" -eq 0 ]]; then
  emit_step "embedded_router_r1_build" "pass" "$(join_command "${router_build_cmd[@]}")" "$ROUTER_BUILD_LOG" "EMBEDDED_ROUTER/R1 build passed"
else
  emit_step "embedded_router_r1_build" "fail" "$(join_command "${router_build_cmd[@]}")" "$ROUTER_BUILD_LOG" "EMBEDDED_ROUTER/R1 build failed"
fi

PROBE_BIN="$ARTIFACT_DIR/embedded_portability_probe"
PROBE_BUILD_LOG="$ARTIFACT_DIR/embedded-portability-probe-build.log"
PROBE_JSON="$ARTIFACT_DIR/embedded-portability-probe.json"
probe_cmd=(
  "$CC_BIN"
  -std=c99
  -Wall
  -Wextra
  -Wpedantic
  -Werror
  -Wno-unused-parameter
  -I"$REPO_ROOT/include"
  -I"$REPO_ROOT/tests"
  -I"$REPO_ROOT/src"
  -DASX_PROFILE_EMBEDDED_ROUTER
  -DASX_CODEC_BIN
  -DASX_DETERMINISTIC=1
  -o "$PROBE_BIN"
  "$REPO_ROOT/tests/embedded/embedded_portability_probe.c"
  "$ROUTER_BUILD_DIR/lib/libasx.a"
)

if [[ "$router_build_rc" -eq 0 ]]; then
  set +e
  "${probe_cmd[@]}" >"$PROBE_BUILD_LOG" 2>&1
  probe_build_rc=$?
  set -e
  if [[ "$probe_build_rc" -eq 0 ]]; then
    set +e
    "$PROBE_BIN" R1 >"$PROBE_JSON" 2>"$ARTIFACT_DIR/embedded-portability-probe.err"
    probe_run_rc=$?
    set -e
    if [[ "$probe_run_rc" -eq 0 ]] && jq -e '.scenario.status == "pass" and .scenario.semantic_digest != "fnv64:0000000000000000"' "$PROBE_JSON" >/dev/null; then
      probe_extra="$(jq -c '{probe_report: .}' "$PROBE_JSON")"
      emit_step "embedded_router_r1_deterministic_probe" "pass" "$(join_command "${probe_cmd[@]}") && $PROBE_BIN R1" "$PROBE_JSON" "minimal deterministic scenario produced a nonzero semantic digest" "$probe_extra"
    else
      emit_step "embedded_router_r1_deterministic_probe" "fail" "$(join_command "${probe_cmd[@]}") && $PROBE_BIN R1" "$PROBE_JSON" "probe failed or emitted invalid digest"
    fi
  else
    emit_step "embedded_router_r1_deterministic_probe" "fail" "$(join_command "${probe_cmd[@]}")" "$PROBE_BUILD_LOG" "probe build failed"
  fi
else
  emit_step "embedded_router_r1_deterministic_probe" "fail" "$(join_command "${probe_cmd[@]}")" "$PROBE_BUILD_LOG" "probe skipped because embedded router build failed"
fi

ALIGN_OBJ="$ARTIFACT_DIR/portability_check_host.o"
ALIGN_LOG="$ARTIFACT_DIR/portability-check-host.log"
align_cmd=("$CC_BIN" -std=c99 -Wall -Wextra -Werror -c "$REPO_ROOT/tools/ci/portability_check.c" -o "$ALIGN_OBJ")
set +e
"${align_cmd[@]}" >"$ALIGN_LOG" 2>&1
align_rc=$?
set -e
if [[ "$align_rc" -eq 0 ]]; then
  emit_step "host_alignment_portability_canary" "pass" "$(join_command "${align_cmd[@]}")" "$ALIGN_OBJ" "host alignment and integer portability canary compiled"
else
  emit_step "host_alignment_portability_canary" "fail" "$(join_command "${align_cmd[@]}")" "$ALIGN_LOG" "host alignment and integer portability canary failed"
fi

M32_OBJ="$ARTIFACT_DIR/portability_check_m32.o"
M32_LOG="$ARTIFACT_DIR/portability-check-m32.log"
m32_cmd=("$CC_BIN" -m32 -std=c99 -Wall -Wextra -Werror -c "$REPO_ROOT/tools/ci/portability_check.c" -o "$M32_OBJ")
set +e
"${m32_cmd[@]}" >"$M32_LOG" 2>&1
m32_rc=$?
set -e
if [[ "$m32_rc" -eq 0 ]]; then
  emit_step "optional_32bit_portability_canary" "pass" "$(join_command "${m32_cmd[@]}")" "$M32_OBJ" "32-bit portability canary compiled"
else
  m32_diag="32-bit compile lane unsupported or unavailable on this host; see log for missing multilib/toolchain diagnostics"
  emit_step "optional_32bit_portability_canary" "unsupported" "$(join_command "${m32_cmd[@]}")" "$M32_LOG" "$m32_diag" '{"unsupported_platform_report":true}'
fi

PROFILE_LOG="$ARTIFACT_DIR/profile-parity.log"
profile_cmd=(make -C "$REPO_ROOT" profile-parity "BUILD_DIR=$ARTIFACT_DIR/profile-parity-build")
set +e
"${profile_cmd[@]}" >"$PROFILE_LOG" 2>&1
profile_rc=$?
set -e
PROFILE_SUMMARY="$(ls -t "$REPO_ROOT"/tools/ci/artifacts/conformance/profile-parity-*-profile-parity.summary.json 2>/dev/null | head -1 || true)"
profile_extra="{}"
if [[ -n "$PROFILE_SUMMARY" && -f "$PROFILE_SUMMARY" ]]; then
  profile_extra="$(jq -c --arg summary "$PROFILE_SUMMARY" '{profile_parity_summary_file: $summary, profile_parity_summary: {run_id, fail, comparable_parity_records, diff_records, semantic_delta_count, adapter_isomorphism_status}}' "$PROFILE_SUMMARY")"
fi
if [[ "$profile_rc" -eq 0 ]]; then
  emit_step "profile_parity_linkage" "pass" "$(join_command "${profile_cmd[@]}")" "$PROFILE_LOG" "profile-parity completed; semantic digest linkage is fresh" "$profile_extra"
else
  emit_step "profile_parity_linkage" "fail" "$(join_command "${profile_cmd[@]}")" "$PROFILE_LOG" "profile-parity failed; inspect log" "$profile_extra"
fi

summary_json="$(jq -s \
  --argjson strict "$STRICT" \
  '{
    total: length,
    passed: map(select(.status == "pass")) | length,
    failed: map(select(.status == "fail")) | length,
    unsupported: map(select(.status == "unsupported")) | length,
    strict: ($strict == 1)
  } + {
    status: (
      if (map(select(.status == "fail")) | length) > 0 then "fail"
      elif ($strict == 1 and (map(select(.status == "unsupported")) | length) > 0) then "fail"
      elif (map(select(.status == "unsupported")) | length) > 0 then "pass_with_unsupported"
      else "pass"
      end
    )
  }' "$JSONL")"

jq -n \
  --arg schema "asx.embedded_portability_proof.v1" \
  --arg run_id "$RUN_ID" \
  --arg generated_at "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
  --arg git_rev "$GIT_REV" \
  --arg artifact_dir "$ARTIFACT_DIR" \
  --arg report_jsonl "$JSONL" \
  --argjson summary "$summary_json" \
  --slurpfile steps "$JSONL" \
  '{
    schema: $schema,
    run_id: $run_id,
    generated_at: $generated_at,
    git_rev: $git_rev,
    status: $summary.status,
    summary: $summary,
    artifacts: {
      artifact_dir: $artifact_dir,
      jsonl: $report_jsonl
    },
    requirements: {
      constrained_profile: "EMBEDDED_ROUTER/R1",
      deterministic_digest: "embedded_router_r1_deterministic_probe",
      portability_lane: "host_alignment_portability_canary plus optional_32bit_portability_canary",
      semantic_parity_link: "profile_parity_linkage"
    },
    unsupported_platforms: ($steps | map(select(.status == "unsupported"))),
    steps: $steps
  }' >"$OUTPUT"

echo "[asx] embedded-portability-proof: status=$(jq -r '.status' "$OUTPUT") report=$OUTPUT"

if jq -e '.status == "fail"' "$OUTPUT" >/dev/null; then
  exit 1
fi
