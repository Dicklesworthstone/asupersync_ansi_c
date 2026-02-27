#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

usage() {
  cat <<'EOF'
Usage: run_embedded_matrix.sh [--run-id <id>] [--log-file <path>] [--dry-run]

Runs cross-builds for router-class triplets:
  - mipsel-openwrt-linux-musl
  - armv7-openwrt-linux-muslgnueabi
  - aarch64-openwrt-linux-musl

Each row emits JSONL with status, diagnostics, binary size, and startup metric status.
EOF
}

json_escape() {
  printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g'
}

emit_jsonl() {
  local log_file="$1"
  local run_id="$2"
  local triplet="$3"
  local compiler="$4"
  local resource_class="$5"
  local status="$6"
  local exit_code="$7"
  local diagnostic="$8"
  local command="$9"
  local output_path="${10}"
  local binary_size="${11}"
  local startup_us="${12}"
  local startup_status="${13}"

  mkdir -p "$(dirname "$log_file")"
  printf '{"kind":"embedded-matrix","run_id":"%s","triplet":"%s","compiler":"%s","profile":"ASX_PROFILE_EMBEDDED_ROUTER","resource_class":"%s","status":"%s","exit_code":%s,"diagnostic":"%s","command":"%s","output":"%s","binary_size_bytes":%s,"startup_metric_us":%s,"startup_metric_status":"%s"}\n' \
    "$(json_escape "$run_id")" \
    "$(json_escape "$triplet")" \
    "$(json_escape "$compiler")" \
    "$(json_escape "$resource_class")" \
    "$(json_escape "$status")" \
    "$exit_code" \
    "$(json_escape "$diagnostic")" \
    "$(json_escape "$command")" \
    "$(json_escape "$output_path")" \
    "$binary_size" \
    "$startup_us" \
    "$(json_escape "$startup_status")" >>"$log_file"
}

run_id="embedded-$(date -u +%Y%m%dT%H%M%SZ)"
dry_run=0
artifact_root="${ARTIFACT_ROOT:-$SCRIPT_DIR/artifacts/build}"
log_file=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --run-id)
      run_id="$2"
      shift 2
      ;;
    --log-file)
      log_file="$2"
      shift 2
      ;;
    --dry-run)
      dry_run=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

mkdir -p "$artifact_root"
if [[ -z "$log_file" ]]; then
  log_file="$artifact_root/${run_id}.jsonl"
fi
: >"$log_file"

declare -a MATRIX=(
  "mipsel-openwrt-linux-musl:mipsel-openwrt-linux-musl-gcc:R1"
  "armv7-openwrt-linux-muslgnueabi:armv7-openwrt-linux-muslgnueabi-gcc:R2"
  "aarch64-openwrt-linux-musl:aarch64-openwrt-linux-musl-gcc:R3"
)

total=0
failed=0
planned=0
unsupported=0

for entry in "${MATRIX[@]}"; do
  IFS=':' read -r triplet compiler resource_class <<<"$entry"
  ((total += 1))
  output_log="$artifact_root/${run_id}-${triplet}.log"
  make_cmd=(make -C "$REPO_ROOT" build "TARGET=$triplet" "PROFILE=EMBEDDED_ROUTER" "CODEC=BIN" "DETERMINISTIC=1" "CFLAGS=-DASX_CLASS_${resource_class}")

  if [[ "$dry_run" == "1" ]]; then
    emit_jsonl "$log_file" "$run_id" "$triplet" "$compiler" "$resource_class" "planned" 0 \
      "dry-run: build/metrics skipped" "${make_cmd[*]}" "$output_log" "0" "null" "planned"
    ((planned += 1))
    continue
  fi

  if ! command -v "$compiler" >/dev/null 2>&1; then
    diagnostic="missing toolchain '$compiler'; install OpenWrt SDK for $triplet or override PATH/toolchain prefix"
    emit_jsonl "$log_file" "$run_id" "$triplet" "$compiler" "$resource_class" "unsupported" 127 \
      "$diagnostic" "n/a" "$output_log" "0" "null" "missing_toolchain"
    ((unsupported += 1))
    ((failed += 1))
    continue
  fi

  if "${make_cmd[@]}" >"$output_log" 2>&1; then
    binary_size=0
    if [[ -f "$REPO_ROOT/build/lib/libasx.a" ]]; then
      binary_size="$(wc -c <"$REPO_ROOT/build/lib/libasx.a" | tr -d ' ')"
    fi
    emit_jsonl "$log_file" "$run_id" "$triplet" "$compiler" "$resource_class" "pass" 0 \
      "embedded cross-build passed" "${make_cmd[*]}" "$output_log" "$binary_size" "null" "pending_qemu_harness"
  else
    diagnostic="cross-build failed; inspect $output_log for include/toolchain/assumption diagnostics"
    emit_jsonl "$log_file" "$run_id" "$triplet" "$compiler" "$resource_class" "fail" 1 \
      "$diagnostic" "${make_cmd[*]}" "$output_log" "0" "null" "build_failed"
    ((failed += 1))
  fi
done

echo "[asx-embedded-matrix] run_id=$run_id log_file=$log_file total=$total planned=$planned unsupported=$unsupported failed=$failed"
if [[ "$failed" -ne 0 ]]; then
  exit 1
fi
