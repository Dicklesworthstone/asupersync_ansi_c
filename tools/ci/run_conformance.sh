#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

usage() {
  cat <<'EOF'
Usage: run_conformance.sh [options]

Run deterministic conformance/parity checks over canonical fixture metadata and
emit machine-readable JSONL artifacts consumable by CI.

Modes (be precise about what each one proves):
  fixture-integrity   Fixture schema, provenance pinning, digest recomputation
                      from the fixture's own events, capture_run_id format,
                      unknown-op detection, and a C codec encode/decode round
                      trip. Does NOT execute the C runtime.
  conformance         Rust parity. Requires fixtures executed through the C
                      runtime and compared against Rust-captured results.
                      Until the C conformance interpreter exists (bridge
                      program W1), there is no such evidence and this mode
                      FAILS rather than passing with zero comparisons.
  codec-equivalence   Compares semantic digests recorded in JSON vs BIN fixture
                      files. Does NOT execute the runtime (real gate: W1.8).
  profile-parity      Compares semantic digests recorded across profile
                      variants. Does NOT execute the runtime (real gate: W1.8).

Options:
  --mode <fixture-integrity|conformance|codec-equivalence|profile-parity>
  --fixtures-root <dir>       (default: fixtures/rust_reference)
  --report-dir <dir>          (default: tools/ci/artifacts/conformance)
  --run-id <id>               (default: <mode>-<utc timestamp>)
  --strict                    (fail on empty fixtures and incomplete parity sets)
  --help

Environment:
  FAIL_ON_EMPTY_FIXTURES=1      fail when no fixture JSON files are present (default: 1)
  FAIL_ON_INCOMPLETE_PARITY=1   fail when parity mode has no comparable pairs (default: 1)
  FAIL_ON_MISSING_JQ=1          fail when jq is missing (default: 1)
  BUILD_DIR=<dir>               build output directory (default: build)
  ASX_SEMANTIC_DELTA_BUDGET=N   default semantic-delta budget (default: 0)
  ASX_SEMANTIC_DELTA_EXCEPTION_FILE=<path>
                                approved exception ledger (default: docs/SEMANTIC_DELTA_EXCEPTIONS.json)
  SEMANTIC_DELTA_ARTIFACT_DIR=<dir>
                                semantic-delta artifact output directory
                                (default: <BUILD_DIR>/conformance)
  CONFORMANCE_CFLAGS="..."      override compile flags for smoke binary
EOF
}

sanitize_name() {
  local value="$1"
  value="$(printf '%s' "$value" | tr -cs 'A-Za-z0-9._-' '_')"
  value="${value#_}"
  value="${value%_}"
  if [[ -z "$value" ]]; then
    value="record"
  fi
  printf '%s\n' "$value"
}

hash_hex() {
  local payload="$1"
  if command -v sha256sum >/dev/null 2>&1; then
    printf '%s' "$payload" | sha256sum | awk '{print $1}'
  else
    printf '%s' "$payload" | shasum -a 256 | awk '{print $1}'
  fi
}

MODE="conformance"
FIXTURES_ROOT="${FIXTURES_ROOT:-$REPO_ROOT/fixtures/rust_reference}"
REPORT_DIR="${REPORT_DIR:-$REPO_ROOT/tools/ci/artifacts/conformance}"
RUN_ID=""
STRICT=0
FAIL_ON_EMPTY_FIXTURES="${FAIL_ON_EMPTY_FIXTURES:-1}"
FAIL_ON_INCOMPLETE_PARITY="${FAIL_ON_INCOMPLETE_PARITY:-1}"
FAIL_ON_MISSING_JQ="${FAIL_ON_MISSING_JQ:-1}"
BASELINE_FILE="$REPO_ROOT/docs/rust_baseline_inventory.json"
SCHEMA_FILE="$REPO_ROOT/schemas/canonical_fixture.schema.json"
SEMANTIC_DELTA_BUDGET_DEFAULT="${ASX_SEMANTIC_DELTA_BUDGET:-0}"
SEMANTIC_DELTA_EXCEPTION_FILE="${ASX_SEMANTIC_DELTA_EXCEPTION_FILE:-$REPO_ROOT/docs/SEMANTIC_DELTA_EXCEPTIONS.json}"
BUILD_DIR="${BUILD_DIR:-build}"
if [[ "$BUILD_DIR" != /* ]]; then
  BUILD_DIR="$REPO_ROOT/$BUILD_DIR"
fi
SEMANTIC_DELTA_ARTIFACT_DIR="${SEMANTIC_DELTA_ARTIFACT_DIR:-$BUILD_DIR/conformance}"
if [[ "$SEMANTIC_DELTA_ARTIFACT_DIR" != /* ]]; then
  SEMANTIC_DELTA_ARTIFACT_DIR="$REPO_ROOT/$SEMANTIC_DELTA_ARTIFACT_DIR"
fi
LIB_ASX="$BUILD_DIR/lib/libasx.a"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --mode)
      MODE="$2"
      shift 2
      ;;
    --fixtures-root)
      FIXTURES_ROOT="$2"
      shift 2
      ;;
    --report-dir)
      REPORT_DIR="$2"
      shift 2
      ;;
    --run-id)
      RUN_ID="$2"
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
      echo "[asx] conformance: unknown argument '$1'" >&2
      usage >&2
      exit 2
      ;;
  esac
done

case "$MODE" in
  fixture-integrity|conformance|codec-equivalence|profile-parity) ;;
  *)
    echo "[asx] conformance: unsupported mode '$MODE'" >&2
    exit 2
    ;;
esac

if [[ "$STRICT" == "1" ]]; then
  FAIL_ON_EMPTY_FIXTURES=1
  FAIL_ON_INCOMPLETE_PARITY=1
fi

if [[ -z "$RUN_ID" ]]; then
  RUN_ID="${MODE}-$(date -u +%Y%m%dT%H%M%SZ)"
fi

mkdir -p "$REPORT_DIR"
REPORT_FILE="$REPORT_DIR/${RUN_ID}-${MODE}.jsonl"
SUMMARY_FILE="$REPORT_DIR/${RUN_ID}-${MODE}.summary.json"
NORMALIZED_FILE="$REPORT_DIR/${RUN_ID}-${MODE}.normalized.jsonl"
DIFF_FILE="$REPORT_DIR/${RUN_ID}-${MODE}.diffs.jsonl"
BUILD_LOG="$REPORT_DIR/${RUN_ID}-${MODE}.build.log"
SMOKE_BUILD_LOG="$REPORT_DIR/${RUN_ID}-${MODE}.smoke-build.log"
SMOKE_RUN_LOG="$REPORT_DIR/${RUN_ID}-${MODE}.smoke-run.log"
RUNNER_BUILD_LOG="$REPORT_DIR/${RUN_ID}-${MODE}.fixture-runner-build.log"
RUNNER_RUN_LOG_DIR="$REPORT_DIR/${RUN_ID}-${MODE}.fixture-runner-logs"
DIFF_DIR="$REPORT_DIR/${RUN_ID}-${MODE}-diffs"

: >"$REPORT_FILE"
: >"$NORMALIZED_FILE"
mkdir -p "$RUNNER_RUN_LOG_DIR"
mkdir -p "$DIFF_DIR"
: >"$DIFF_FILE"

if ! command -v jq >/dev/null 2>&1; then
  msg="jq is required to parse canonical fixture metadata"
  if [[ "$FAIL_ON_MISSING_JQ" == "1" ]]; then
    echo "[asx] conformance: FAIL ($msg)" >&2
    exit 1
  fi
  printf '{"kind":"runner","run_id":"%s","mode":"%s","status":"skip","diagnostic":"%s"}\n' \
    "$RUN_ID" "$MODE" "$msg" >>"$REPORT_FILE"
  jq -n \
    --arg run_id "$RUN_ID" \
    --arg mode "$MODE" \
    --arg report "$REPORT_FILE" \
    --arg diag "$msg" \
    '{run_id:$run_id,mode:$mode,status:"skip",pass:0,fail:0,skip:1,total:1,report_file:$report,diagnostic:$diag}' \
    >"$SUMMARY_FILE"
  exit 0
fi

if [[ ! -f "$BASELINE_FILE" ]]; then
  echo "[asx] conformance: FAIL (missing baseline inventory: $BASELINE_FILE)" >&2
  exit 1
fi

if [[ ! -f "$SCHEMA_FILE" ]]; then
  echo "[asx] conformance: FAIL (missing canonical fixture schema: $SCHEMA_FILE)" >&2
  exit 1
fi

baseline_commit="$(jq -r '.source_repo.commit // ""' "$BASELINE_FILE")"
baseline_toolchain_hash="$(jq -r '.rust_toolchain.commit_hash // ""' "$BASELINE_FILE")"
baseline_toolchain_release="$(jq -r '.rust_toolchain.release // ""' "$BASELINE_FILE")"
baseline_toolchain_host="$(jq -r '.rust_toolchain.host // ""' "$BASELINE_FILE")"
baseline_cargo_sha="$(jq -r '.cargo_lock.sha256 // ""' "$BASELINE_FILE")"
# A rebase in progress: fixtures captured on it live in build/fixture_staging
# until `make fixtures-promote` makes it the active baseline.
pending_commit="$(jq -r '.pending_rebase.source_repo.commit // ""' "$BASELINE_FILE")"

if [[ -z "$baseline_commit" || -z "$baseline_toolchain_hash" || -z "$baseline_cargo_sha" ]]; then
  echo "[asx] conformance: FAIL (baseline inventory missing required provenance fields)" >&2
  exit 1
fi

fixture_runner_bin=""
cc_bin="${CC:-cc}"
read -r -a cflags_arr <<<"${CONFORMANCE_CFLAGS:- -std=c99 -Wall -Wextra -Wpedantic}"

echo "[asx] conformance[$MODE]: build + smoke gate..." >&2
if ! make -C "$REPO_ROOT" build "BUILD_DIR=$BUILD_DIR" >"$BUILD_LOG" 2>&1; then
  jq -n -c \
    --arg run_id "$RUN_ID" \
    --arg mode "$MODE" \
    --arg log "$BUILD_LOG" \
    '{kind:"smoke",run_id:$run_id,mode:$mode,status:"fail",parity:"fail",delta_classification:"harness_defect",diagnostic:("make build failed; inspect " + $log)}' \
    >>"$REPORT_FILE"
else
  smoke_bin="$BUILD_DIR/tests/conformance/codec_json_baseline_test"
  mkdir -p "$(dirname "$smoke_bin")"
  smoke_cmd=(
    "$cc_bin"
    "${cflags_arr[@]}"
    -I"$REPO_ROOT/include"
    -I"$REPO_ROOT/tests"
    -o "$smoke_bin"
    "$REPO_ROOT/tests/conformance/codec_json_baseline_test.c"
    "$LIB_ASX"
  )

  set +e
  "${smoke_cmd[@]}" >"$SMOKE_BUILD_LOG" 2>&1
  smoke_build_rc=$?
  set -e

  if [[ "$smoke_build_rc" -ne 0 ]]; then
    jq -n -c \
      --arg run_id "$RUN_ID" \
      --arg mode "$MODE" \
      --arg log "$SMOKE_BUILD_LOG" \
      '{kind:"smoke",run_id:$run_id,mode:$mode,status:"fail",parity:"fail",delta_classification:"harness_defect",diagnostic:("conformance smoke compile failed; inspect " + $log)}' \
      >>"$REPORT_FILE"
  else
    set +e
    "$smoke_bin" >"$SMOKE_RUN_LOG" 2>&1
    smoke_run_rc=$?
    set -e
    if [[ "$smoke_run_rc" -ne 0 ]]; then
      jq -n -c \
        --arg run_id "$RUN_ID" \
        --arg mode "$MODE" \
        --arg log "$SMOKE_RUN_LOG" \
        --argjson rc "$smoke_run_rc" \
        '{kind:"smoke",run_id:$run_id,mode:$mode,status:"fail",parity:"fail",delta_classification:"harness_defect",exit_code:$rc,diagnostic:("conformance smoke test failed; inspect " + $log)}' \
        >>"$REPORT_FILE"
    else
      jq -n -c \
        --arg run_id "$RUN_ID" \
        --arg mode "$MODE" \
        --arg log "$SMOKE_RUN_LOG" \
        '{kind:"smoke",run_id:$run_id,mode:$mode,status:"pass",parity:"pass",delta_classification:"none",diagnostic:("codec_json_baseline_test passed; log: " + $log)}' \
        >>"$REPORT_FILE"
    fi
  fi
fi

if [[ -f "$LIB_ASX" ]]; then
  fixture_runner_src="$REPORT_DIR/${RUN_ID}-${MODE}.fixture_runner.c"
  fixture_runner_bin="$REPORT_DIR/${RUN_ID}-${MODE}.fixture_runner"

  cat >"$fixture_runner_src" <<'EOF'
#include <asx/asx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file_all(const char *path, size_t *out_len)
{
    FILE *fp = fopen(path, "rb");
    long size;
    char *buf;
    if (fp == NULL) {
        return NULL;
    }
    if (fseek(fp, 0L, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    size = ftell(fp);
    if (size < 0) {
        fclose(fp);
        return NULL;
    }
    if (fseek(fp, 0L, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }
    buf = (char *)malloc((size_t)size + 1u);
    if (buf == NULL) {
        fclose(fp);
        return NULL;
    }
    if (fread(buf, 1u, (size_t)size, fp) != (size_t)size) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    buf[size] = '\0';
    if (out_len != NULL) {
        *out_len = (size_t)size;
    }
    return buf;
}

int main(int argc, char **argv)
{
    asx_canonical_fixture fixture;
    asx_codec_buffer replay_key;
    asx_codec_equiv_report report;
    asx_status st;
    char *payload;
    size_t payload_len = 0u;

    if (argc != 2) {
        fprintf(stderr, "usage: fixture_runner <fixture.json>\n");
        return 64;
    }

    payload = read_file_all(argv[1], &payload_len);
    if (payload == NULL) {
        fprintf(stderr, "failed to read fixture: %s\n", argv[1]);
        return 65;
    }

    asx_canonical_fixture_init(&fixture);
    asx_codec_buffer_init(&replay_key);
    asx_codec_equiv_report_init(&report);

    st = asx_codec_decode_fixture(ASX_CODEC_KIND_JSON, payload, payload_len, &fixture);
    if (st != ASX_OK) {
        fprintf(stderr, "decode_failed:%s\n", asx_status_str(st));
        free(payload);
        asx_codec_buffer_reset(&replay_key);
        asx_canonical_fixture_reset(&fixture);
        return 2;
    }

    st = asx_codec_fixture_replay_key(&fixture, &replay_key);
    if (st != ASX_OK) {
        fprintf(stderr, "replay_key_failed:%s\n", asx_status_str(st));
        free(payload);
        asx_codec_buffer_reset(&replay_key);
        asx_canonical_fixture_reset(&fixture);
        return 3;
    }

    st = asx_codec_cross_codec_verify(&fixture, &report);
    if (st != ASX_OK) {
        if (st == ASX_E_EQUIVALENCE_MISMATCH) {
            const char *field = (report.count > 0u) ? report.diffs[0].field_name : "";
            fprintf(stderr, "equivalence_mismatch:first_field=%s count=%u\n", field, (unsigned)report.count);
            free(payload);
            asx_codec_buffer_reset(&replay_key);
            asx_canonical_fixture_reset(&fixture);
            return 4;
        }
        fprintf(stderr, "cross_codec_failed:%s\n", asx_status_str(st));
        free(payload);
        asx_codec_buffer_reset(&replay_key);
        asx_canonical_fixture_reset(&fixture);
        return 5;
    }

    printf("ok\tscenario=%s\treplay_key=%s\n",
           fixture.scenario_id != NULL ? fixture.scenario_id : "",
           replay_key.data != NULL ? replay_key.data : "");

    free(payload);
    asx_codec_buffer_reset(&replay_key);
    asx_canonical_fixture_reset(&fixture);
    return 0;
}
EOF

  set +e
  "$cc_bin" "${cflags_arr[@]}" \
    -I"$REPO_ROOT/include" \
    -o "$fixture_runner_bin" \
    "$fixture_runner_src" \
    "$LIB_ASX" >"$RUNNER_BUILD_LOG" 2>&1
  runner_build_rc=$?
  set -e

  if [[ "$runner_build_rc" -ne 0 ]]; then
    fixture_runner_bin=""
    echo "[asx] conformance[$MODE]: fixture replay runner compilation failed; tail of $RUNNER_BUILD_LOG" >&2
    tail -n 40 "$RUNNER_BUILD_LOG" >&2 || true
    jq -n -c \
      --arg run_id "$RUN_ID" \
      --arg mode "$MODE" \
      --arg log "$RUNNER_BUILD_LOG" \
      '{kind:"fixture_replay_runner",run_id:$run_id,mode:$mode,status:"fail",parity:"fail",delta_classification:"harness_defect",diagnostic:("failed to compile fixture runner; inspect " + $log)}' \
      >>"$REPORT_FILE"
  fi
fi

declare -a fixture_files
if [[ -d "$FIXTURES_ROOT" ]]; then
  mapfile -t fixture_files < <(
    find "$FIXTURES_ROOT" -type f -name '*.json' \
      ! -name 'manifest.json' \
      ! -name 'provenance.json' \
      ! -path '*/reports/*' \
      | LC_ALL=C sort
  )
fi

if [[ "${#fixture_files[@]}" -eq 0 ]]; then
  jq -n -c \
    --arg run_id "$RUN_ID" \
    --arg mode "$MODE" \
    --arg root "$FIXTURES_ROOT" \
    '{kind:"fixture_scan",run_id:$run_id,mode:$mode,status:"skip",parity:"skip",delta_classification:"none",diagnostic:("no fixture json files found under " + $root)}' \
    >>"$REPORT_FILE"
else
  for fixture in "${fixture_files[@]}"; do
    rel_fixture="${fixture#$REPO_ROOT/}"
    # The capture tool's digest is SHA-256 over compact, key-sorted JSON of
    # {error_codes, events, snapshot}. A fixture whose digest does not
    # recompute from its own contents is not what it claims to be.
    recomputed_digest="sha256:$(hash_hex "$(jq -cS '{error_codes: .expected_error_codes, events: .expected_events, snapshot: .expected_final_snapshot}' "$fixture")")"
    record="$(jq -c \
      --arg run_id "$RUN_ID" \
      --arg mode "$MODE" \
      --arg file "$rel_fixture" \
      --arg recomputed_digest "$recomputed_digest" \
      --arg baseline_commit "$baseline_commit" \
      --arg pending_commit "$pending_commit" \
      --arg baseline_toolchain_hash "$baseline_toolchain_hash" \
      --arg baseline_toolchain_release "$baseline_toolchain_release" \
      --arg baseline_toolchain_host "$baseline_toolchain_host" \
      --arg baseline_cargo_sha "$baseline_cargo_sha" \
      '
      {
        kind: "fixture",
        run_id: $run_id,
        mode: $mode,
        file: $file,
        scenario_id: (.scenario_id // ""),
        codec: (.codec // ""),
        profile: (.profile // ""),
        semantic_digest: (.semantic_digest // ""),
        fixture_schema_version: (.fixture_schema_version // ""),
        scenario_dsl_version: (.scenario_dsl_version // ""),
        rust_baseline_commit: (.provenance.rust_baseline_commit // ""),
        rust_toolchain_commit_hash: (.provenance.rust_toolchain_commit_hash // ""),
        rust_toolchain_release: (.provenance.rust_toolchain_release // ""),
        rust_toolchain_host: (.provenance.rust_toolchain_host // ""),
        cargo_lock_sha256: (.provenance.cargo_lock_sha256 // ""),
        capture_run_id: (.provenance.capture_run_id // ""),
        unknown_ops: ([.. | strings | select(startswith("unknown_op:"))] | length)
      }
      | .captured = (.capture_run_id
          | test("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"))
      | .digest_recomputes = (.semantic_digest == $recomputed_digest)
      | .diagnostics = [
          (if .scenario_id == "" then "missing scenario_id" else empty end),
          (if .codec != "json" and .codec != "bin" then "invalid codec" else empty end),
          (if .profile == "" then "missing profile" else empty end),
          (if (.semantic_digest | test("^sha256:[0-9a-f]{64}$") | not) then "invalid semantic_digest" else empty end),
          (if .fixture_schema_version == "" then "missing fixture_schema_version" else empty end),
          (if .scenario_dsl_version == "" then "missing scenario_dsl_version" else empty end),
          (if .rust_baseline_commit == "" then "missing provenance.rust_baseline_commit" else empty end),
          (if .rust_toolchain_commit_hash == "" then "missing provenance.rust_toolchain_commit_hash" else empty end),
          (if .rust_toolchain_release == "" then "missing provenance.rust_toolchain_release" else empty end),
          (if .rust_toolchain_host == "" then "missing provenance.rust_toolchain_host" else empty end),
          (if .cargo_lock_sha256 == "" then "missing provenance.cargo_lock_sha256" else empty end),
          (if .rust_baseline_commit != $baseline_commit then "rust_baseline_commit mismatch" else empty end),
          (if $pending_commit != "" and .rust_baseline_commit == $pending_commit then "captured on the pending rebase baseline but committed before promotion (mixing generations; use make fixtures-promote)" else empty end),
          (if .rust_toolchain_commit_hash != $baseline_toolchain_hash then "rust_toolchain_commit_hash mismatch" else empty end),
          (if .rust_toolchain_release != $baseline_toolchain_release then "rust_toolchain_release mismatch" else empty end),
          (if .rust_toolchain_host != $baseline_toolchain_host then "rust_toolchain_host mismatch" else empty end),
          (if .cargo_lock_sha256 != $baseline_cargo_sha then "cargo_lock_sha256 mismatch" else empty end),
          (if .unknown_ops > 0 then "fixture records unknown ops (capture tool traced an op it could not execute)" else empty end),
          (if .captured and (.digest_recomputes | not) then "semantic_digest does not recompute from expected_events/snapshot/error_codes" else empty end)
        ]
      # A fixture whose capture_run_id is not a UUID was not produced by a
      # capture run (e.g. written by hand). It is reported and excluded,
      # never counted as Rust evidence.
      | .status = (if (.diagnostics | length) > 0 then "fail"
                   elif (.captured | not) then "skip"
                   else "pass" end)
      | .parity = .status
      | .delta_classification = (if .status == "fail" then "harness_defect" else "none" end)
      | .provenance_class = (if .captured then "captured" else "hand_authored" end)
      | .diagnostic = (if (.diagnostics | length) > 0 then (.diagnostics | join("; "))
                       elif (.captured | not) then "hand-authored fixture (capture_run_id is not a UUID" + (if .digest_recomputes then "" else "; semantic_digest does not recompute" end) + "): excluded from Rust parity; recapture via the twin-run oracle (W1.7)"
                       else "fixture metadata/provenance valid; digest recomputes" end)
      | del(.diagnostics)
      ' \
      "$fixture")"

    echo "$record" >>"$REPORT_FILE"
    echo "$record" >>"$NORMALIZED_FILE"

    scenario_id="$(jq -r '.scenario_id // ""' <<<"$record")"
    codec_value="$(jq -r '.codec // ""' <<<"$record")"
    profile_value="$(jq -r '.profile // ""' <<<"$record")"
    if [[ -z "$scenario_id" ]]; then
      scenario_id="$(basename "$rel_fixture" .json)"
    fi

    if [[ -n "$fixture_runner_bin" && -x "$fixture_runner_bin" ]]; then
      safe_name="$(sanitize_name "${scenario_id}_${codec_value}_${profile_value}")"
      runner_log="$RUNNER_RUN_LOG_DIR/${safe_name}.log"
      set +e
      "$fixture_runner_bin" "$fixture" >"$runner_log" 2>&1
      runner_rc=$?
      set -e

      runner_diag="$(tr '\n' ' ' <"$runner_log" | sed -e 's/[[:space:]]\+/ /g' -e 's/^ //;s/ $//')"
      if [[ -z "$runner_diag" ]]; then
        runner_diag="fixture replay helper completed"
      fi

      replay_status="fail"
      replay_parity="fail"
      replay_delta="harness_defect"
      if [[ "$runner_rc" -eq 0 ]]; then
        replay_status="pass"
        replay_parity="pass"
        replay_delta="none"
      elif [[ "$runner_rc" -eq 4 ]]; then
        replay_delta="c_regression"
      fi

      jq -n -c \
        --arg run_id "$RUN_ID" \
        --arg mode "$MODE" \
        --arg file "$rel_fixture" \
        --arg scenario_id "$scenario_id" \
        --arg codec "$codec_value" \
        --arg profile "$profile_value" \
        --arg status "$replay_status" \
        --arg parity "$replay_parity" \
        --arg delta "$replay_delta" \
        --arg diagnostic "$runner_diag" \
        --argjson exit_code "$runner_rc" \
        '{kind:"fixture_replay",run_id:$run_id,mode:$mode,file:$file,scenario_id:$scenario_id,codec:$codec,profile:$profile,status:$status,parity:$parity,delta_classification:$delta,exit_code:$exit_code,diagnostic:$diagnostic}' \
        >>"$REPORT_FILE"
    fi
  done
fi

if [[ "$MODE" == "codec-equivalence" ]]; then
  jq -s -c \
    --arg run_id "$RUN_ID" \
    --arg mode "$MODE" \
    '
    map(select(.kind == "fixture" and .status == "pass"))
    | group_by(.scenario_id + "|" + .profile)
    | map({
        kind: "codec_equivalence",
        run_id: $run_id,
        mode: $mode,
        scenario_id: .[0].scenario_id,
        profile: .[0].profile,
        json_digest: (map(select(.codec == "json") | .semantic_digest) | first // ""),
        bin_digest: (map(select(.codec == "bin") | .semantic_digest) | first // "")
      }
      | if .json_digest == "" or .bin_digest == "" then
          . + {status:"skip", parity:"skip", delta_classification:"none", diagnostic:"missing json/bin pair for scenario"}
        elif .json_digest == .bin_digest then
          . + {status:"pass", parity:"pass", semantic_digest:.json_digest, delta_classification:"none", diagnostic:"json/bin semantic digest match"}
        else
          . + {status:"fail", parity:"fail", semantic_digest:.json_digest, delta_classification:"c_regression", diagnostic:"json/bin semantic digest mismatch"}
        end
      | del(.json_digest, .bin_digest)
    )[]' \
    "$NORMALIZED_FILE" >>"$REPORT_FILE"
fi

if [[ "$MODE" == "profile-parity" ]]; then
  jq -s -c \
    --arg run_id "$RUN_ID" \
    --arg mode "$MODE" \
    '
    map(select(.kind == "fixture" and .status == "pass"))
    | group_by(.scenario_id + "|" + .codec)
    | map({
        kind: "profile_parity",
        run_id: $run_id,
        mode: $mode,
        scenario_id: .[0].scenario_id,
        codec: .[0].codec,
        profiles: (map(.profile) | unique),
        digests: (map(.semantic_digest) | unique)
      }
      | if (.profiles | length) < 2 then
          . + {status:"skip", parity:"skip", delta_classification:"none", diagnostic:"fewer than two profiles present for scenario/codec"}
        elif (.digests | length) == 1 then
          . + {status:"pass", parity:"pass", semantic_digest:(.digests[0]), delta_classification:"none", diagnostic:"profiles share identical semantic digest"}
        else
          . + {status:"fail", parity:"fail", semantic_digest:(.digests[0]), delta_classification:"c_regression", diagnostic:"profile semantic digest mismatch"}
        end
      | del(.digests)
    )[]' \
    "$NORMALIZED_FILE" >>"$REPORT_FILE"
fi

ENRICHED_REPORT_FILE="$REPORT_DIR/${RUN_ID}-${MODE}.enriched.jsonl"
: >"$ENRICHED_REPORT_FILE"
line_no=0
while IFS= read -r line || [[ -n "$line" ]]; do
  if [[ -z "$line" ]]; then
    continue
  fi
  line_no=$((line_no + 1))

  enriched="$(jq -c \
    --arg baseline_commit "$baseline_commit" \
    --arg baseline_toolchain_hash "$baseline_toolchain_hash" \
    --arg baseline_toolchain_release "$baseline_toolchain_release" \
    --arg baseline_toolchain_host "$baseline_toolchain_host" \
    --arg baseline_cargo_sha "$baseline_cargo_sha" \
    '. + {
      baseline_rust_baseline_commit: $baseline_commit,
      baseline_rust_toolchain_commit_hash: $baseline_toolchain_hash,
      baseline_rust_toolchain_release: $baseline_toolchain_release,
      baseline_rust_toolchain_host: $baseline_toolchain_host,
      baseline_cargo_lock_sha256: $baseline_cargo_sha
    }' <<<"$line")"

  rec_status="$(jq -r '.status // ""' <<<"$enriched")"
  rec_delta="$(jq -r '.delta_classification // "none"' <<<"$enriched")"

  if [[ "$rec_status" == "fail" || "$rec_delta" != "none" ]]; then
    rec_kind="$(jq -r '.kind // "record"' <<<"$enriched")"
    rec_scenario="$(jq -r '.scenario_id // ""' <<<"$enriched")"
    rec_file="$(jq -r '.file // ""' <<<"$enriched")"
    if [[ -z "$rec_scenario" ]]; then
      rec_scenario="record-$line_no"
    fi
    safe_scenario="$(sanitize_name "$rec_scenario")"
    class_hash="$(hash_hex "$RUN_ID|$MODE|$rec_kind|$safe_scenario|$line_no" | cut -c1-24)"
    class_id="cls-${class_hash}"
    diff_file="$DIFF_DIR/${line_no}-${rec_kind}-${safe_scenario}.json"

    jq -n \
      --arg run_id "$RUN_ID" \
      --arg mode "$MODE" \
      --arg kind "$rec_kind" \
      --arg scenario_id "$rec_scenario" \
      --arg file "$rec_file" \
      --arg classification_record_id "$class_id" \
      --arg baseline_commit "$baseline_commit" \
      --arg baseline_toolchain_hash "$baseline_toolchain_hash" \
      --arg baseline_toolchain_release "$baseline_toolchain_release" \
      --arg baseline_toolchain_host "$baseline_toolchain_host" \
      --arg baseline_cargo_sha "$baseline_cargo_sha" \
      --argjson line_no "$line_no" \
      --argjson record "$enriched" \
      '{
        kind: "parity_diff_artifact",
        run_id: $run_id,
        mode: $mode,
        line_no: $line_no,
        source_kind: $kind,
        scenario_id: $scenario_id,
        file: $file,
        classification_record_id: $classification_record_id,
        expected_provenance: {
          rust_baseline_commit: $baseline_commit,
          rust_toolchain_commit_hash: $baseline_toolchain_hash,
          rust_toolchain_release: $baseline_toolchain_release,
          rust_toolchain_host: $baseline_toolchain_host,
          cargo_lock_sha256: $baseline_cargo_sha
        },
        record: $record
      }' >"$diff_file"

    enriched="$(jq -c \
      --arg class_id "$class_id" \
      --arg diff_file "$diff_file" \
      '.classification_record_id = (.classification_record_id // $class_id)
       | .diff_artifact = (.diff_artifact // $diff_file)' <<<"$enriched")"
  fi

  echo "$enriched" >>"$ENRICHED_REPORT_FILE"
done <"$REPORT_FILE"

mv "$ENRICHED_REPORT_FILE" "$REPORT_FILE"

jq -c \
  '
  select(.status == "fail")
  | {
      kind,
      run_id,
      mode,
      file: (.file // ""),
      scenario_id: (.scenario_id // ""),
      codec: (.codec // ""),
      profile: (.profile // ""),
      status,
      parity,
      delta_classification,
      semantic_digest: (.semantic_digest // ""),
      diagnostic: (.diagnostic // "")
    }
  ' \
  "$REPORT_FILE" >"$DIFF_FILE"

jq -s \
  --arg run_id "$RUN_ID" \
  --arg mode "$MODE" \
  --arg report_file "$REPORT_FILE" \
  --arg summary_file "$SUMMARY_FILE" \
  --arg diff_file "$DIFF_FILE" \
  '
  {
    run_id: $run_id,
    mode: $mode,
    total: length,
    pass: (map(select(.status == "pass")) | length),
    fail: (map(select(.status == "fail")) | length),
    skip: (map(select(.status == "skip")) | length),
    fixture_records: (map(select(.kind == "fixture")) | length),
    hand_authored_records: (map(select(.kind == "fixture" and .provenance_class == "hand_authored")) | length),
    # Comparisons of C-runtime execution against Rust-captured results.
    # Nothing produces these until the conformance interpreter exists (W1).
    executed_parity_records: (map(select(.kind == "executed_parity")) | length),
    parity_records: (map(select(.kind == "codec_equivalence" or .kind == "profile_parity")) | length),
    comparable_parity_records: (
      map(select((.kind == "codec_equivalence" or .kind == "profile_parity") and .status != "skip"))
      | length
    ),
    diff_records: (map(select(.status == "fail")) | length),
    fail_by_kind: (
      reduce (map(select(.status == "fail"))[]) as $row
        ({}; .[$row.kind] = ((.[$row.kind] // 0) + 1))
    ),
    provenance_mismatch_records: (
      map(
        select(
          .kind == "fixture"
          and .status == "fail"
          and (
            (.diagnostic | contains("rust_baseline_commit mismatch"))
            or (.diagnostic | contains("rust_toolchain_commit_hash mismatch"))
            or (.diagnostic | contains("rust_toolchain_release mismatch"))
            or (.diagnostic | contains("rust_toolchain_host mismatch"))
            or (.diagnostic | contains("cargo_lock_sha256 mismatch"))
          )
        )
      )
      | length
    ),
    report_file: $report_file,
    diff_file: $diff_file,
    summary_file: $summary_file
  }' \
  "$REPORT_FILE" >"$SUMMARY_FILE"

if [[ ! "$SEMANTIC_DELTA_BUDGET_DEFAULT" =~ ^[0-9]+$ ]]; then
  echo "[asx] conformance[$MODE]: FAIL (ASX_SEMANTIC_DELTA_BUDGET must be an integer >= 0)" >&2
  exit 2
fi

mkdir -p "$SEMANTIC_DELTA_ARTIFACT_DIR"
SEMANTIC_DELTA_FILE="$SEMANTIC_DELTA_ARTIFACT_DIR/semantic_delta_${RUN_ID}.json"

if [[ -s "$DIFF_FILE" ]]; then
  semantic_delta_records_json="$(jq -sc '[.[] | select((.delta_classification // "none") != "harness_defect")]' "$DIFF_FILE")"
  non_budgetable_records_json="$(jq -sc '[.[] | select((.delta_classification // "none") == "harness_defect")]' "$DIFF_FILE")"
else
  semantic_delta_records_json='[]'
  non_budgetable_records_json='[]'
fi

semantic_delta_count="$(jq -r 'length' <<<"$semantic_delta_records_json")"
non_budgetable_count="$(jq -r 'length' <<<"$non_budgetable_records_json")"
changed_units_json="$(jq -c '[.[] | {kind,scenario_id,codec,profile,delta_classification,diagnostic}]' <<<"$semantic_delta_records_json")"
impacted_scenarios_json="$(jq -c '[.[] | .scenario_id | select(. != "")] | unique' <<<"$semantic_delta_records_json")"

allowed_budget="$SEMANTIC_DELTA_BUDGET_DEFAULT"
matched_exceptions_json='[]'
matched_exception_ids_json='[]'
exception_file_status="not_found"

if [[ -f "$SEMANTIC_DELTA_EXCEPTION_FILE" ]]; then
  exception_file_status="loaded"
  if ! jq empty "$SEMANTIC_DELTA_EXCEPTION_FILE" >/dev/null 2>&1; then
    echo "[asx] conformance[$MODE]: FAIL (invalid semantic delta exception file JSON: $SEMANTIC_DELTA_EXCEPTION_FILE)" >&2
    exit 1
  fi

  matched_exceptions_json="$(jq -c \
    --arg mode "$MODE" \
    --argjson impacted "$impacted_scenarios_json" \
    'if type != "array" then [] else . end
     | map(
         select((.status // "") == "approved")
         | select((.id // "") != "")
         | select((.approver // "") != "")
         | select((.budget // -1) | type == "number" and . >= 0)
         | select((.mode // "any") == $mode or (.mode // "any") == "any")
         | select((.scenario_ids // []) | type == "array")
         | select(
             if ($impacted | length) == 0 then
               true
             else
               ((.scenario_ids | index("*")) != null)
               or (($impacted - (.scenario_ids | map(tostring))) | length == 0)
             end
           )
       )' \
    "$SEMANTIC_DELTA_EXCEPTION_FILE")"

  matched_budget="$(jq -r '[.[].budget] | if length == 0 then -1 else max end' <<<"$matched_exceptions_json")"
  if [[ "$matched_budget" =~ ^-?[0-9]+$ ]] && [[ "$matched_budget" -ge 0 ]] && [[ "$matched_budget" -gt "$allowed_budget" ]]; then
    allowed_budget="$matched_budget"
  fi
  matched_exception_ids_json="$(jq -c '[.[].id]' <<<"$matched_exceptions_json")"
fi

semantic_delta_pass=true
semantic_delta_diagnostic="semantic delta within budget"
if [[ "$non_budgetable_count" -gt 0 ]]; then
  semantic_delta_pass=false
  semantic_delta_diagnostic="non-budgetable conformance failures detected"
  echo "[asx] conformance[$MODE]: non-budgetable semantic-delta records detected:" >&2
  jq -r \
    --arg mode "$MODE" \
    '.[] |
      "[asx] conformance[" + $mode + "]: non-budgetable scenario=\(.scenario_id // "unknown") codec=\(.codec // "unknown") profile=\(.profile // "unknown") diagnostic=\(.diagnostic // "n/a")"' \
    <<<"$non_budgetable_records_json" >&2
elif [[ "$semantic_delta_count" -gt "$allowed_budget" ]]; then
  semantic_delta_pass=false
  semantic_delta_diagnostic="semantic delta budget exceeded"
  echo "[asx] conformance[$MODE]: semantic delta budget exceeded (count=$semantic_delta_count, allowed=$allowed_budget)" >&2
  jq -r \
    --arg mode "$MODE" \
    '.[] |
      "[asx] conformance[" + $mode + "]: delta scenario=\(.scenario_id // "unknown") codec=\(.codec // "unknown") profile=\(.profile // "unknown") classification=\(.delta_classification // "n/a") diagnostic=\(.diagnostic // "n/a")"' \
    <<<"$semantic_delta_records_json" >&2
fi

jq -n \
  --arg run_id "$RUN_ID" \
  --arg mode "$MODE" \
  --arg exception_file "$SEMANTIC_DELTA_EXCEPTION_FILE" \
  --arg exception_file_status "$exception_file_status" \
  --arg diagnostic "$semantic_delta_diagnostic" \
  --arg report_file "$REPORT_FILE" \
  --arg summary_file "$SUMMARY_FILE" \
  --arg diff_file "$DIFF_FILE" \
  --arg artifact_file "$SEMANTIC_DELTA_FILE" \
  --argjson semantic_delta_pass "$semantic_delta_pass" \
  --argjson semantic_delta_count "$semantic_delta_count" \
  --argjson non_budgetable_count "$non_budgetable_count" \
  --argjson default_budget "$SEMANTIC_DELTA_BUDGET_DEFAULT" \
  --argjson allowed_budget "$allowed_budget" \
  --argjson impacted_scenarios "$impacted_scenarios_json" \
  --argjson changed_units "$changed_units_json" \
  --argjson matched_exceptions "$matched_exceptions_json" \
  --argjson matched_exception_ids "$matched_exception_ids_json" \
  '{
    kind: "semantic_delta_budget",
    run_id: $run_id,
    mode: $mode,
    semantic_delta_pass: $semantic_delta_pass,
    semantic_delta_count: $semantic_delta_count,
    non_budgetable_fail_count: $non_budgetable_count,
    default_budget: $default_budget,
    allowed_budget: $allowed_budget,
    impacted_scenarios: $impacted_scenarios,
    changed_semantic_units: $changed_units,
    matched_exceptions: $matched_exceptions,
    matched_exception_ids: $matched_exception_ids,
    exception_file: $exception_file,
    exception_file_status: $exception_file_status,
    diagnostic: $diagnostic,
    evidence: {
      report_file: $report_file,
      summary_file: $summary_file,
      diff_file: $diff_file
    }
  }' >"$SEMANTIC_DELTA_FILE"

jq \
  --arg semantic_delta_artifact "$SEMANTIC_DELTA_FILE" \
  --argjson semantic_delta_count "$semantic_delta_count" \
  --argjson non_budgetable_count "$non_budgetable_count" \
  --argjson semantic_delta_budget_default "$SEMANTIC_DELTA_BUDGET_DEFAULT" \
  --argjson semantic_delta_budget_allowed "$allowed_budget" \
  --argjson semantic_delta_pass "$semantic_delta_pass" \
  --argjson semantic_delta_exception_ids "$matched_exception_ids_json" \
  '. + {
    semantic_delta_count: $semantic_delta_count,
    non_budgetable_fail_count: $non_budgetable_count,
    semantic_delta_budget_default: $semantic_delta_budget_default,
    semantic_delta_budget_allowed: $semantic_delta_budget_allowed,
    semantic_delta_pass: $semantic_delta_pass,
    semantic_delta_exception_ids: $semantic_delta_exception_ids,
    semantic_delta_artifact: $semantic_delta_artifact
  }' \
  "$SUMMARY_FILE" >"${SUMMARY_FILE}.tmp"
mv "${SUMMARY_FILE}.tmp" "$SUMMARY_FILE"

fail_count="$(jq -r '.fail' "$SUMMARY_FILE")"
fixture_count="$(jq -r '.fixture_records' "$SUMMARY_FILE")"
parity_count="$(jq -r '.parity_records' "$SUMMARY_FILE")"
comparable_parity_count="$(jq -r '.comparable_parity_records' "$SUMMARY_FILE")"
hand_authored_count="$(jq -r '.hand_authored_records' "$SUMMARY_FILE")"
executed_parity_count="$(jq -r '.executed_parity_records' "$SUMMARY_FILE")"
diff_count="$(jq -r '.diff_records' "$SUMMARY_FILE")"
semantic_delta_pass_value="$(jq -r '.semantic_delta_pass' "$SUMMARY_FILE")"
semantic_delta_count_value="$(jq -r '.semantic_delta_count' "$SUMMARY_FILE")"
semantic_delta_budget_allowed_value="$(jq -r '.semantic_delta_budget_allowed' "$SUMMARY_FILE")"
non_budgetable_count_value="$(jq -r '.non_budgetable_fail_count' "$SUMMARY_FILE")"

echo "[asx] conformance[$MODE]: report=$REPORT_FILE summary=$SUMMARY_FILE" >&2
echo "[asx] conformance[$MODE]: fixture_records=$fixture_count hand_authored_records=$hand_authored_count executed_parity_records=$executed_parity_count parity_records=$parity_count comparable_parity_records=$comparable_parity_count fail=$fail_count diff_records=$diff_count" >&2
echo "[asx] conformance[$MODE]: semantic_delta_count=$semantic_delta_count_value allowed_budget=$semantic_delta_budget_allowed_value non_budgetable_fail_count=$non_budgetable_count_value pass=$semantic_delta_pass_value artifact=$SEMANTIC_DELTA_FILE" >&2

exit_code=0
if [[ "$semantic_delta_pass_value" != "true" ]]; then
  echo "[asx] conformance[$MODE]: FAIL (semantic delta budget gate)" >&2
  exit_code=1
fi
if [[ "$fixture_count" -eq 0 && "$FAIL_ON_EMPTY_FIXTURES" == "1" ]]; then
  echo "[asx] conformance[$MODE]: FAIL (no fixtures and strict empty-fixture policy enabled)" >&2
  exit_code=1
fi
if [[ ( "$MODE" == "codec-equivalence" || "$MODE" == "profile-parity" ) && "$comparable_parity_count" -eq 0 && "$FAIL_ON_INCOMPLETE_PARITY" == "1" ]]; then
  echo "[asx] conformance[$MODE]: FAIL (no comparable parity pairs and strict parity policy enabled)" >&2
  exit_code=1
fi
case "$MODE" in
  codec-equivalence|profile-parity)
    echo "[asx] conformance[$MODE]: scope: compares semantic digests recorded in fixture files; the C runtime is not executed (real gate: bridge bead W1.8)" >&2
    ;;
  fixture-integrity)
    echo "[asx] conformance[$MODE]: scope: fixture schema/provenance/digest integrity and codec round trip; hand_authored_records=$hand_authored_count (excluded from Rust parity); the C runtime is not executed" >&2
    ;;
  conformance)
    # Rust parity means: fixtures executed through the C runtime and compared
    # with Rust-captured results. Zero such comparisons is not a pass.
    if [[ "$executed_parity_count" -eq 0 ]]; then
      echo "[asx] conformance[$MODE]: FAIL (NO RUST PARITY EVIDENCE: executed_parity_records=0; no fixture was executed through the C runtime. The conformance interpreter and twin-run oracle are bridge program W1, beads bd-9kll.2.*. Fixture integrity is checked by 'make fixture-integrity'.)" >&2
      exit_code=1
    fi
    ;;
esac

exit "$exit_code"
