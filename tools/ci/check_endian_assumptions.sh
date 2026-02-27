#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

usage() {
  cat <<'EOF'
Usage: check_endian_assumptions.sh --compiler <cc> [--bits <32|64>] [--run-id <id>] [--target <name>] [--log-file <path>] [--dry-run]
EOF
}

json_escape() {
  printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g'
}

emit_jsonl() {
  local log_file="$1"
  local run_id="$2"
  local target="$3"
  local compiler="$4"
  local bits="$5"
  local endian="$6"
  local status="$7"
  local exit_code="$8"
  local diagnostic="$9"
  if [[ -z "$log_file" ]]; then
    return 0
  fi
  mkdir -p "$(dirname "$log_file")"
  printf '{"kind":"endian-check","run_id":"%s","target":"%s","compiler":"%s","bits":"%s","endian":"%s","status":"%s","exit_code":%s,"diagnostic":"%s"}\n' \
    "$(json_escape "$run_id")" \
    "$(json_escape "$target")" \
    "$(json_escape "$compiler")" \
    "$(json_escape "$bits")" \
    "$(json_escape "$endian")" \
    "$(json_escape "$status")" \
    "$exit_code" \
    "$(json_escape "$diagnostic")" >>"$log_file"
}

compiler=""
bits="64"
run_id="endian-$(date -u +%Y%m%dT%H%M%SZ)"
target="unspecified"
log_file=""
dry_run=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --compiler)
      compiler="$2"
      shift 2
      ;;
    --bits)
      bits="$2"
      shift 2
      ;;
    --run-id)
      run_id="$2"
      shift 2
      ;;
    --target)
      target="$2"
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

if [[ -z "$compiler" ]]; then
  echo "--compiler is required" >&2
  usage >&2
  exit 2
fi

if [[ "$bits" != "32" && "$bits" != "64" ]]; then
  echo "--bits must be 32 or 64" >&2
  exit 2
fi

if ! command -v "$compiler" >/dev/null 2>&1; then
  emit_jsonl "$log_file" "$run_id" "$target" "$compiler" "$bits" "unknown" "unsupported" 127 \
    "compiler '$compiler' not found; install toolchain or pass --compiler with an absolute binary path"
  echo "unknown"
  exit 127
fi

if [[ "$dry_run" == "1" ]]; then
  emit_jsonl "$log_file" "$run_id" "$target" "$compiler" "$bits" "unknown" "planned" 0 \
    "dry-run: compile probe skipped"
  echo "unknown"
  exit 0
fi

tmp_dir="$(mktemp -d "${TMPDIR:-/tmp}/asx-endian-probe.XXXXXX")"
trap 'rm -rf "$tmp_dir"' EXIT

probe_c="$tmp_dir/endian_probe.c"
probe_o="$tmp_dir/endian_probe.o"
probe_log="$tmp_dir/endian_probe.log"

cat >"$probe_c" <<'EOF'
#include <limits.h>
#include <stdint.h>

#if CHAR_BIT != 8
#error "CHAR_BIT != 8 is unsupported for asx core assumptions"
#endif

#if UINT8_MAX != 0xffu
#error "uint8_t width mismatch"
#endif

#if UINT32_MAX != 0xffffffffu
#error "uint32_t width mismatch"
#endif

int asx_endian_probe(void) {
    return 0;
}
EOF

compile_cmd=("$compiler" "-std=c99" "-Werror" "-c" "$probe_c" "-o" "$probe_o")
case "$(basename "$compiler")" in
  gcc|clang)
    compile_cmd+=("-m${bits}")
    ;;
esac

if "${compile_cmd[@]}" >"$probe_log" 2>&1; then
  :
else
  diagnostic="$(tr '\n' ' ' <"$probe_log" | sed -e 's/[[:space:]]\+/ /g' -e 's/^ //;s/ $//')"
  if [[ -z "$diagnostic" ]]; then
    diagnostic="endian probe compile failed; check compiler support for requested bitness/toolchain"
  fi
  emit_jsonl "$log_file" "$run_id" "$target" "$compiler" "$bits" "unknown" "fail" 1 "$diagnostic"
  echo "unknown"
  exit 1
fi

macro_dump="$("$compiler" -dM -E -x c /dev/null 2>/dev/null || true)"
endian="unknown"
if printf '%s\n' "$macro_dump" | grep -q "__BYTE_ORDER__"; then
  if printf '%s\n' "$macro_dump" | grep -q "__ORDER_LITTLE_ENDIAN__"; then
    if printf '%s\n' "$macro_dump" | grep -Eq "__BYTE_ORDER__[[:space:]]+__ORDER_LITTLE_ENDIAN__"; then
      endian="little"
    fi
  fi
  if printf '%s\n' "$macro_dump" | grep -q "__ORDER_BIG_ENDIAN__"; then
    if printf '%s\n' "$macro_dump" | grep -Eq "__BYTE_ORDER__[[:space:]]+__ORDER_BIG_ENDIAN__"; then
      endian="big"
    fi
  fi
fi

if [[ "$endian" == "unknown" ]]; then
  case "$(basename "$compiler")" in
    cl|cl.exe)
      endian="little"
      ;;
  esac
fi

emit_jsonl "$log_file" "$run_id" "$target" "$compiler" "$bits" "$endian" "pass" 0 "endian assumptions validated"
echo "$endian"

