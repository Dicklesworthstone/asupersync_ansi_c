#!/usr/bin/env bash
# replay_fuzz_counterexamples.sh - replay durable differential-fuzz corpus cases
#
# Replays minimized fuzz and sanitizer counterexamples through the C fuzz
# harness, optionally enabling a Rust reference binary when available. Each
# fixture is fail-closed: parse errors, missing fields, command failures, and
# mismatched expected counters fail the gate and still emit a JSON report.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

CORPUS_DIR="${REPO_ROOT}/fixtures/fuzz_counterexamples"
REPORT_ROOT="${REPO_ROOT}/build/fuzz/counterexamples"
FUZZ_BIN="${REPO_ROOT}/build/fuzz/fuzz_differential"
RUST_BINARY="${RUST_FUZZ_BINARY:-}"
RUN_ID="fuzz-counterexample-replay-$(date -u +%Y%m%dT%H%M%SZ)-$$"
WORK_DIR=""
CASES_FILE=""
SUMMARY_FILE=""
SHA_TOOL=""

usage() {
    cat <<'USAGE'
Usage: tools/ci/replay_fuzz_counterexamples.sh [options]

Options:
  --corpus-dir <dir>     Corpus directory (default: fixtures/fuzz_counterexamples)
  --report-root <dir>    Report root (default: build/fuzz/counterexamples)
  --fuzz-bin <path>      fuzz_differential binary (default: build/fuzz/fuzz_differential)
  --rust-binary <path>   Optional Rust reference fuzz binary
  --run-id <id>          Stable run id for report paths
  -h, --help             Show this help
USAGE
}

while [ $# -gt 0 ]; do
    case "$1" in
        --corpus-dir) CORPUS_DIR="$2"; shift 2 ;;
        --report-root) REPORT_ROOT="$2"; shift 2 ;;
        --fuzz-bin) FUZZ_BIN="$2"; shift 2 ;;
        --rust-binary) RUST_BINARY="$2"; shift 2 ;;
        --run-id) RUN_ID="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "[asx] fuzz-counterexample-replay: unknown option '$1'" >&2; usage; exit 2 ;;
    esac
done

WORK_DIR="${REPORT_ROOT}/${RUN_ID}"
CASES_FILE="${WORK_DIR}/cases.jsonl"
SUMMARY_FILE="${WORK_DIR}/fuzz_counterexample_replay_report.json"

require_command() {
    local name="$1"
    if ! command -v "$name" >/dev/null 2>&1; then
        echo "[asx] fuzz-counterexample-replay: required command missing: $name" >&2
        exit 2
    fi
}

require_command jq
require_command git
require_command find
require_command sort
require_command date
require_command awk

if command -v sha256sum >/dev/null 2>&1; then
    SHA_TOOL="sha256sum"
elif command -v shasum >/dev/null 2>&1; then
    SHA_TOOL="shasum"
else
    echo "[asx] fuzz-counterexample-replay: required sha256 tool missing" >&2
    exit 2
fi

sha256_file() {
    if [ "$SHA_TOOL" = "sha256sum" ]; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

sha256_text() {
    if [ "$SHA_TOOL" = "sha256sum" ]; then
        printf '%s' "$1" | sha256sum | awk '{print $1}'
    else
        printf '%s' "$1" | shasum -a 256 | awk '{print $1}'
    fi
}

finish_report() {
    local status="$1"
    local diagnostic="${2:-}"
    local git_commit
    local rust_status
    local case_count
    local fail_count

    if [ -n "${ASX_GIT_COMMIT:-}" ]; then
        git_commit="$ASX_GIT_COMMIT"
    else
        git_commit="$(git -C "$REPO_ROOT" show -s --format=%H HEAD 2>/dev/null || git -C "$REPO_ROOT" rev-parse HEAD 2>/dev/null || echo unknown)"
    fi
    rust_status="not_requested"
    if [ -n "$RUST_BINARY" ]; then
        if [ -x "$RUST_BINARY" ]; then
            rust_status="available"
        else
            rust_status="missing"
        fi
    fi

    case_count="$(jq -s 'length' "$CASES_FILE" 2>/dev/null || echo 0)"
    fail_count="$(jq -s '[.[] | select(.status != "pass")] | length' "$CASES_FILE" 2>/dev/null || echo 0)"

    jq -s \
        --arg schema "asx.fuzz_counterexample_replay.v1" \
        --arg run_id "$RUN_ID" \
        --arg status "$status" \
        --arg diagnostic "$diagnostic" \
        --arg generated_at "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
        --arg git_commit "$git_commit" \
        --arg corpus_dir "$CORPUS_DIR" \
        --arg fuzz_bin "$FUZZ_BIN" \
        --arg rust_binary "$RUST_BINARY" \
        --arg rust_status "$rust_status" \
        --argjson case_count "$case_count" \
        --argjson fail_count "$fail_count" \
        '{
          schema: $schema,
          run_id: $run_id,
          status: $status,
          diagnostic: (if $diagnostic == "" then null else $diagnostic end),
          generated_at: $generated_at,
          git: {commit: $git_commit},
          corpus: {dir: $corpus_dir, case_count: $case_count, failure_count: $fail_count},
          harness: {fuzz_bin: $fuzz_bin, rust_binary: (if $rust_binary == "" then null else $rust_binary end), rust_status: $rust_status},
          cases: .
        }' "$CASES_FILE" >"$SUMMARY_FILE"

    echo "[asx] fuzz-counterexample-replay: status=$status report=$SUMMARY_FILE"
}

record_case() {
    local status="$1"
    local fixture="$2"
    local scenario_id="$3"
    local input_digest="$4"
    local seed="$5"
    local iterations="$6"
    local max_ops="$7"
    local mutations="$8"
    local command="$9"
    local report_path="${10}"
    local diagnostic="${11}"
    local c_exit_code="${12}"
    local determinism_failures="${13}"
    local crashes="${14}"
    local rust_divergences="${15}"
    local expected_exit="${16}"
    local expected_det="${17}"
    local expected_crashes="${18}"
    local expected_rust="${19}"
    local fixture_digest="${20}"
    local minimization_json="${21}"

    jq -cn \
        --arg status "$status" \
        --arg fixture "$fixture" \
        --arg scenario_id "$scenario_id" \
        --arg input_digest "$input_digest" \
        --arg fixture_digest "$fixture_digest" \
        --arg command "$command" \
        --arg report_path "$report_path" \
        --arg diagnostic "$diagnostic" \
        --argjson minimization "$minimization_json" \
        --argjson seed "$seed" \
        --argjson iterations "$iterations" \
        --argjson max_ops "$max_ops" \
        --argjson mutations "$mutations" \
        --argjson c_exit_code "$c_exit_code" \
        --argjson determinism_failures "$determinism_failures" \
        --argjson crashes "$crashes" \
        --argjson rust_divergences "$rust_divergences" \
        --argjson expected_exit "$expected_exit" \
        --argjson expected_det "$expected_det" \
        --argjson expected_crashes "$expected_crashes" \
        --argjson expected_rust "$expected_rust" \
        '{
          scenario_id: $scenario_id,
          status: $status,
          fixture: $fixture,
          fixture_digest: ("sha256:" + $fixture_digest),
          input_digest: ("sha256:" + $input_digest),
          input: {seed: $seed, iterations: $iterations, max_ops: $max_ops, mutations: $mutations},
          minimization: $minimization,
          expected: {
            c_exit_code: $expected_exit,
            determinism_failures: $expected_det,
            crashes: $expected_crashes,
            rust_divergences: $expected_rust
          },
          actual: {
            c_exit_code: $c_exit_code,
            determinism_failures: $determinism_failures,
            crashes: $crashes,
            rust_divergences: $rust_divergences
          },
          command: $command,
          report_path: $report_path,
          diagnostic: (if $diagnostic == "" then null else $diagnostic end)
        }' >>"$CASES_FILE"
}

fail_gate() {
    finish_report "fail" "$1"
    exit 1
}

mkdir -p "$WORK_DIR"
: >"$CASES_FILE"

if [ ! -d "$CORPUS_DIR" ]; then
    fail_gate "corpus directory is missing"
fi
if [ ! -x "$FUZZ_BIN" ]; then
    fail_gate "fuzz harness is missing or not executable; run make fuzz-build"
fi

mapfile -t FIXTURES < <(find "$CORPUS_DIR" -type f -name '*.json' | sort)
if [ "${#FIXTURES[@]}" -eq 0 ]; then
    fail_gate "no counterexample corpus fixtures found"
fi

for fixture in "${FIXTURES[@]}"; do
    fixture_digest="$(sha256_file "$fixture")"
    fixture_config="$(jq -ec '
      if .schema != "asx.fuzz_counterexample.v1" then
        error("unsupported fixture schema")
      elif (.scenario_id | type) != "string" or (.scenario_id | length) == 0 then
        error("missing scenario_id")
      elif (.input.seed | type) != "number" then
        error("missing input.seed")
      elif (.input.iterations | type) != "number" then
        error("missing input.iterations")
      elif (.input.max_ops | type) != "number" then
        error("missing input.max_ops")
      elif (.input.mutations | type) != "number" then
        error("missing input.mutations")
      elif (.expected.c_exit_code | type) != "number" then
        error("missing expected.c_exit_code")
      elif (.expected.determinism_failures | type) != "number" then
        error("missing expected.determinism_failures")
      elif (.expected.crashes | type) != "number" then
        error("missing expected.crashes")
      elif (.expected.rust_divergences | type) != "number" then
        error("missing expected.rust_divergences")
      elif (.minimization.source | type) != "string" then
        error("missing minimization.source")
      elif (.minimization.source_failure_class | type) != "string" then
        error("missing minimization.source_failure_class")
      else
        {
          scenario_id,
          seed: .input.seed,
          iterations: .input.iterations,
          max_ops: .input.max_ops,
          mutations: .input.mutations,
          expected_exit: .expected.c_exit_code,
          expected_det: .expected.determinism_failures,
          expected_crashes: .expected.crashes,
          expected_rust: .expected.rust_divergences,
          rust_mode: (.rust_reference.mode // "optional"),
          input: .input,
          minimization: .minimization
        }
      end
    ' "$fixture" 2>/dev/null)" || {
        record_case "fail" "$fixture" "unknown" "0" 0 0 0 0 "jq fixture contract" "" \
            "missing or invalid fixture fields" 2 0 0 0 0 0 0 0 "$fixture_digest" '{}'
        fail_gate "fixture parse failed"
    }

    scenario_id="$(printf '%s\n' "$fixture_config" | jq -r '.scenario_id')"
    seed="$(printf '%s\n' "$fixture_config" | jq -r '.seed')"
    iterations="$(printf '%s\n' "$fixture_config" | jq -r '.iterations')"
    max_ops="$(printf '%s\n' "$fixture_config" | jq -r '.max_ops')"
    mutations="$(printf '%s\n' "$fixture_config" | jq -r '.mutations')"
    expected_exit="$(printf '%s\n' "$fixture_config" | jq -r '.expected_exit')"
    expected_det="$(printf '%s\n' "$fixture_config" | jq -r '.expected_det')"
    expected_crashes="$(printf '%s\n' "$fixture_config" | jq -r '.expected_crashes')"
    expected_rust="$(printf '%s\n' "$fixture_config" | jq -r '.expected_rust')"
    rust_mode="$(printf '%s\n' "$fixture_config" | jq -r '.rust_mode')"
    input_json="$(printf '%s\n' "$fixture_config" | jq -c '.input')"
    input_digest="$(sha256_text "$input_json")"
    minimization_json="$(printf '%s\n' "$fixture_config" | jq -c '.minimization')"
    minimization_source="$(printf '%s\n' "$minimization_json" | jq -r '.source')"
    minimization_failure="$(printf '%s\n' "$minimization_json" | jq -r '.source_failure_class')"

    case "$rust_mode" in
        optional|required|disabled) ;;
        *)
            record_case "fail" "$fixture" "$scenario_id" "0" "$seed" "$iterations" "$max_ops" \
                "$mutations" "jq .rust_reference.mode" "" "invalid rust_reference.mode" 2 0 0 0 \
                "$expected_exit" "$expected_det" "$expected_crashes" "$expected_rust" \
                "$fixture_digest" "$minimization_json"
            fail_gate "fixture rust_reference.mode invalid"
            ;;
    esac

    case_report="${WORK_DIR}/${scenario_id}.jsonl"
    command_text="${FUZZ_BIN} --seed ${seed} --iterations ${iterations} --max-ops ${max_ops} --mutations ${mutations} --report ${case_report}"
    cmd=("$FUZZ_BIN" --seed "$seed" --iterations "$iterations" --max-ops "$max_ops" --mutations "$mutations" --report "$case_report")

    if [ "$rust_mode" = "required" ]; then
        if [ -z "$RUST_BINARY" ] || [ ! -x "$RUST_BINARY" ]; then
            record_case "fail" "$fixture" "$scenario_id" "$input_digest" "$seed" "$iterations" \
                "$max_ops" "$mutations" "$command_text" "$case_report" \
                "Rust reference binary required but unavailable" 2 0 0 0 "$expected_exit" \
                "$expected_det" "$expected_crashes" "$expected_rust" "$fixture_digest" \
                "$minimization_json"
            fail_gate "required Rust reference binary unavailable"
        fi
    fi

    if [ "$rust_mode" != "disabled" ] && [ -n "$RUST_BINARY" ] && [ -x "$RUST_BINARY" ]; then
        cmd+=(--rust-binary "$RUST_BINARY")
        command_text="${command_text} --rust-binary ${RUST_BINARY}"
    fi

    c_exit=0
    "${cmd[@]}" >/dev/null 2>&1 || c_exit=$?

    summary="$(jq -sc 'map(select(.kind == "summary")) | last // {}' "$case_report" 2>/dev/null || echo '{}')"
    actual_det="$(printf '%s\n' "$summary" | jq -r '.determinism_failures // -1')"
    actual_crashes="$(printf '%s\n' "$summary" | jq -r '.crashes // -1')"
    actual_rust="$(printf '%s\n' "$summary" | jq -r '.rust_divergences // -1')"

    diagnostic=""
    status="pass"
    if [ "$c_exit" -ne "$expected_exit" ]; then
        status="fail"
        diagnostic="exit code mismatch"
    elif [ "$actual_det" -ne "$expected_det" ]; then
        status="fail"
        diagnostic="determinism failure count mismatch"
    elif [ "$actual_crashes" -ne "$expected_crashes" ]; then
        status="fail"
        diagnostic="crash count mismatch"
    elif [ "$actual_rust" -ne "$expected_rust" ]; then
        status="fail"
        diagnostic="Rust divergence count mismatch"
    fi

    record_case "$status" "$fixture" "$scenario_id" "$input_digest" "$seed" "$iterations" \
        "$max_ops" "$mutations" "$command_text" "$case_report" "$diagnostic" "$c_exit" \
        "$actual_det" "$actual_crashes" "$actual_rust" "$expected_exit" "$expected_det" \
        "$expected_crashes" "$expected_rust" "$fixture_digest" "$minimization_json"

    if [ "$status" != "pass" ]; then
        fail_gate "$scenario_id failed: $diagnostic"
    fi

    printf '[asx] fuzz-counterexample-replay: pass scenario=%s seed=%s minimization=%s/%s\n' \
        "$scenario_id" "$seed" "$minimization_source" "$minimization_failure"
done

finish_report "pass" ""
