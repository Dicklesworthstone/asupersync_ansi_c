#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

usage() {
  cat <<'EOF'
Usage: run_embedded_matrix.sh [--run-id <id>] [--log-file <path>] [--dry-run]

Runs cross-builds for router-class triplets (OpenWrt SDK musl compiler when on
PATH, else the Debian glibc cross compiler of the same ISA class):
  - mipsel-openwrt-linux-musl        (fallback mipsel-linux-gnu)
  - armv7-openwrt-linux-muslgnueabi  (fallback arm-linux-gnueabihf)
  - aarch64-openwrt-linux-musl       (fallback aarch64-linux-gnu)
A target with neither compiler is reported as unsupported and fails the run.

Each row emits JSONL with status, diagnostics, binary size, and startup metric status.

Environment:
  FAIL_ON_LAYOUT_REGRESSION 1 to fail the run when invariant layout deltas are flagged
  BUILD_DIR=<dir>           build output directory (default: build)
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
  local layout_report="${14:-}"
  local layout_status="${15:-not_run}"

  mkdir -p "$(dirname "$log_file")"
  printf '{"kind":"embedded-matrix","run_id":"%s","triplet":"%s","compiler":"%s","profile":"ASX_PROFILE_EMBEDDED_ROUTER","resource_class":"%s","status":"%s","exit_code":%s,"diagnostic":"%s","command":"%s","output":"%s","binary_size_bytes":%s,"startup_metric_us":%s,"startup_metric_status":"%s","layout_report":"%s","layout_status":"%s"}\n' \
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
    "$(json_escape "$startup_status")" \
    "$(json_escape "$layout_report")" \
    "$(json_escape "$layout_status")" >>"$log_file"
}

emit_delta_jsonl() {
  local log_file="$1"
  local run_id="$2"
  local status="$3"
  local exit_code="$4"
  local diagnostic="$5"
  local report_path="$6"
  local index_path="$7"

  mkdir -p "$(dirname "$log_file")"
  printf '{"kind":"embedded-layout-delta","run_id":"%s","status":"%s","exit_code":%s,"diagnostic":"%s","delta_report":"%s","index_path":"%s"}\n' \
    "$(json_escape "$run_id")" \
    "$(json_escape "$status")" \
    "$exit_code" \
    "$(json_escape "$diagnostic")" \
    "$(json_escape "$report_path")" \
    "$(json_escape "$index_path")" >>"$log_file"
}

run_id="embedded-$(date -u +%Y%m%dT%H%M%SZ)"
dry_run=0
artifact_root="${ARTIFACT_ROOT:-$SCRIPT_DIR/artifacts/build}"
log_file=""
FAIL_ON_LAYOUT_REGRESSION="${FAIL_ON_LAYOUT_REGRESSION:-0}"
BUILD_DIR="${BUILD_DIR:-build}"

if [[ "$BUILD_DIR" != /* ]]; then
  BUILD_DIR="$REPO_ROOT/$BUILD_DIR"
fi

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
layout_index_file="$artifact_root/${run_id}-layout-index.tsv"
: >"$layout_index_file"

# arch:preferred triplet (OpenWrt SDK, musl):fallback triplet (Debian cross
# gcc, glibc):resource class. The fallback keeps the same ISA class (armv7 is
# gnueabihf, not the armv5 gnueabi port) so CI cross-builds every architecture
# even without an OpenWrt SDK; the JSONL row records which compiler was used.
declare -a MATRIX=(
  "mipsel:mipsel-openwrt-linux-musl:mipsel-linux-gnu:R1"
  "armv7:armv7-openwrt-linux-muslgnueabi:arm-linux-gnueabihf:R2"
  "aarch64:aarch64-openwrt-linux-musl:aarch64-linux-gnu:R3"
)

total=0
failed=0
planned=0
unsupported=0

for entry in "${MATRIX[@]}"; do
  IFS=':' read -r arch preferred_triplet fallback_triplet resource_class <<<"$entry"
  triplet="$preferred_triplet"
  if ! command -v "${preferred_triplet}-gcc" >/dev/null 2>&1 &&
    command -v "${fallback_triplet}-gcc" >/dev/null 2>&1; then
    triplet="$fallback_triplet"
  fi
  compiler="${triplet}-gcc"
  ((total += 1))
  output_log="$artifact_root/${run_id}-${triplet}.log"
  make_cmd=(env "ASX_E2E_RESOURCE_CLASS=$resource_class" make -C "$REPO_ROOT" build "BUILD_DIR=$BUILD_DIR" "TARGET=$triplet" "PROFILE=EMBEDDED_ROUTER" "CODEC=BIN" "DETERMINISTIC=1")

  if [[ "$dry_run" == "1" ]]; then
    emit_jsonl "$log_file" "$run_id" "$triplet" "$compiler" "$resource_class" "planned" 0 \
      "dry-run: build/metrics skipped" "${make_cmd[*]}" "$output_log" "0" "null" "planned" "" "planned"
    ((planned += 1))
    continue
  fi

  if ! command -v "$compiler" >/dev/null 2>&1; then
    diagnostic="missing $arch toolchain: neither ${preferred_triplet}-gcc (OpenWrt SDK) nor ${fallback_triplet}-gcc (Debian cross gcc) is on PATH"
    emit_jsonl "$log_file" "$run_id" "$triplet" "$compiler" "$resource_class" "unsupported" 127 \
      "$diagnostic" "n/a" "$output_log" "0" "null" "missing_toolchain" "" "not_run"
    ((unsupported += 1))
    ((failed += 1))
    continue
  fi

  row_status="pass"
  row_exit_code=0
  row_diag="embedded cross-build passed"
  layout_report=""
  layout_status="not_run"

  if "${make_cmd[@]}" >"$output_log" 2>&1; then
    binary_size=0
    if [[ -f "$BUILD_DIR/lib/libasx.a" ]]; then
      binary_size="$(wc -c <"$BUILD_DIR/lib/libasx.a" | tr -d ' ')"
    fi
    layout_err_file="$artifact_root/${run_id}-${triplet}.layout.err"
    if layout_report="$("$SCRIPT_DIR/generate_layout_budget_report.sh" \
        --compiler "$compiler" \
        --target-id "$triplet" \
        --profile "EMBEDDED_ROUTER" \
        --resource-class "$resource_class" \
        --run-id "$run_id" \
        --out-dir "$artifact_root" 2>"$layout_err_file")"; then
      layout_status="pass"
      printf '%s\t%s\n' "$triplet" "${layout_report%.json}.kv" >>"$layout_index_file"
    else
      layout_status="fail"
      row_status="fail"
      row_exit_code=1
      layout_diag="$(tr '\n' ' ' <"$layout_err_file" | sed -e 's/[[:space:]]\+/ /g' -e 's/^ //;s/ $//')"
      if [[ -z "$layout_diag" ]]; then
        layout_diag="layout/budget report generation failed"
      fi
      row_diag="$layout_diag"
      ((failed += 1))
    fi
    emit_jsonl "$log_file" "$run_id" "$triplet" "$compiler" "$resource_class" "$row_status" "$row_exit_code" \
      "$row_diag" "${make_cmd[*]}" "$output_log" "$binary_size" "null" "pending_qemu_harness" "$layout_report" "$layout_status"
  else
    diagnostic="cross-build failed; inspect $output_log for include/toolchain/assumption diagnostics"
    emit_jsonl "$log_file" "$run_id" "$triplet" "$compiler" "$resource_class" "fail" 1 \
      "$diagnostic" "${make_cmd[*]}" "$output_log" "0" "null" "build_failed" "" "not_run"
    ((failed += 1))
  fi
done

delta_report="$artifact_root/${run_id}-layout-budget-delta.json"
delta_status="not_run"
delta_exit=0
delta_diag="layout/budget delta report skipped"

if [[ "$dry_run" == "1" ]]; then
  delta_status="planned"
  delta_diag="dry-run: delta report not computed"
elif [[ -s "$layout_index_file" ]]; then
  set +e
  "$SCRIPT_DIR/compute_layout_budget_delta.sh" \
    --run-id "$run_id" \
    --index-file "$layout_index_file" \
    --output "$delta_report"
  delta_rc=$?
  set -e
  case "$delta_rc" in
    0)
      delta_status="pass"
      delta_diag="layout/budget delta report generated"
      ;;
    3)
      delta_status="regression"
      delta_diag="invariant layout/budget regressions flagged (see delta report)"
      if [[ "$FAIL_ON_LAYOUT_REGRESSION" == "1" ]]; then
        ((failed += 1))
        delta_exit=1
      fi
      ;;
    *)
      delta_status="fail"
      delta_diag="failed to compute layout/budget delta report"
      delta_exit=1
      ((failed += 1))
      ;;
  esac
else
  delta_status="not_run"
  delta_diag="no layout reports were generated"
fi

emit_delta_jsonl "$log_file" "$run_id" "$delta_status" "$delta_exit" "$delta_diag" "$delta_report" "$layout_index_file"

echo "[asx-embedded-matrix] run_id=$run_id log_file=$log_file total=$total planned=$planned unsupported=$unsupported failed=$failed delta_status=$delta_status"
if [[ "$failed" -ne 0 ]]; then
  exit 1
fi
