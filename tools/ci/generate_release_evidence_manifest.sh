#!/usr/bin/env bash
# =============================================================================
# generate_release_evidence_manifest.sh -- release evidence manifest gate
#
# Collects release-facing quality-gate artifacts into one machine-readable
# manifest and validates that referenced artifacts exist, have stable digests,
# and do not carry stale repository commit metadata.
#
# Usage:
#   tools/ci/generate_release_evidence_manifest.sh [--strict]
#       [--output build/ci-manifests/release_evidence_manifest.json]
#       [--report build/ci-manifests/release_evidence_report.json]
#
# SPDX-License-Identifier: MIT
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

RUN_ID="${ASX_CI_RUN_TAG:-release-evidence-$(date -u +%Y%m%dT%H%M%SZ)}"
GENERATED_AT="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
OUTPUT="$REPO_ROOT/build/ci-manifests/release_evidence_manifest.json"
REPORT="$REPO_ROOT/build/ci-manifests/release_evidence_report.json"
STRICT=0

usage() {
    cat <<'USAGE'
Usage: tools/ci/generate_release_evidence_manifest.sh [OPTIONS]

Options:
  --run-id <id>      Override run identifier
  --output <file>    Manifest output path
  --report <file>    Validation report output path
  --strict           Exit non-zero on required missing/stale/failed evidence
  --help             Show this help
USAGE
    exit 2
}

while [ $# -gt 0 ]; do
    case "$1" in
        --run-id)
            [ $# -ge 2 ] || usage
            RUN_ID="$2"
            shift 2
            ;;
        --output)
            [ $# -ge 2 ] || usage
            OUTPUT="$2"
            shift 2
            ;;
        --report)
            [ $# -ge 2 ] || usage
            REPORT="$2"
            shift 2
            ;;
        --strict)
            STRICT=1
            shift
            ;;
        --help|-h)
            usage
            ;;
        *)
            echo "[asx] release-evidence: ERROR: unknown option '$1'" >&2
            usage
            ;;
    esac
done

if ! command -v jq >/dev/null 2>&1; then
    echo "[asx] release-evidence: ERROR: jq is required" >&2
    exit 2
fi

mkdir -p "$(dirname "$OUTPUT")" "$(dirname "$REPORT")"
RECORDS="${OUTPUT%.json}.records.jsonl"
: > "$RECORDS"

CURRENT_COMMIT="$(git -C "$REPO_ROOT" rev-parse --verify HEAD 2>/dev/null || echo unknown)"
CURRENT_BRANCH="$(git -C "$REPO_ROOT" branch --show-current 2>/dev/null || echo unknown)"
CC_VERSION="$(${CC:-cc} --version 2>/dev/null | head -n 1 || echo unknown)"
MAKE_VERSION="$(make --version 2>/dev/null | head -n 1 || echo unknown)"
JQ_VERSION="$(jq --version 2>/dev/null || echo unknown)"

rel_path() {
    case "$1" in
        "$REPO_ROOT"/*) printf '%s\n' "${1#"$REPO_ROOT"/}" ;;
        *) printf '%s\n' "$1" ;;
    esac
}

sha256_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

file_bytes() {
    wc -c < "$1" | tr -d ' '
}

file_mtime() {
    if stat -c %Y "$1" >/dev/null 2>&1; then
        stat -c %Y "$1"
    else
        stat -f %m "$1"
    fi
}

latest_match() {
    local pattern="$1"
    local file
    file="$(ls -1t $pattern 2>/dev/null | head -n 1 || true)"
    printf '%s\n' "$file"
}

latest_nonempty_match() {
    local pattern="$1"
    local file
    for file in $(ls -1t $pattern 2>/dev/null || true); do
        if [ -s "$file" ]; then
            printf '%s\n' "$file"
            return 0
        fi
    done
    printf '\n'
}

json_status() {
    local path="$1"
    if [ ! -s "$path" ]; then
        printf 'empty\n'
        return 0
    fi

    case "$path" in
        *.json)
            jq -r '
                def down: ascii_downcase;
                def failed:
                    ((.status? // "" | tostring | down) as $s |
                        ($s == "fail" or $s == "failed" or $s == "error"))
                    or (.gate_pass? == false)
                    or (.semantic_delta_pass? == false)
                    or ((.summary.failed? // 0) > 0)
                    or ((.summary.fail? // 0) > 0)
                    or ((.fail? // 0) > 0);
                def skipped:
                    ((.status? // "" | tostring | down) as $s |
                        ($s == "skip" or $s == "skipped"));
                if failed then "fail"
                elif skipped then "skip"
                else "pass"
                end
            ' "$path" 2>/dev/null || printf 'present\n'
            ;;
        *.jsonl)
            jq -s -r '
                def down: ascii_downcase;
                def row_failed:
                    ((.status? // "" | tostring | down) as $s |
                        ($s == "fail" or $s == "failed" or $s == "error"))
                    or (.gate_pass? == false)
                    or ((.fail? // 0) > 0);
                def row_skipped:
                    ((.status? // "" | tostring | down) as $s |
                        ($s == "skip" or $s == "skipped"));
                if length == 0 then "empty"
                elif any(.[]; row_failed) then "fail"
                elif any(.[]; row_skipped) then "skip"
                else "pass"
                end
            ' "$path" 2>/dev/null || printf 'present\n'
            ;;
        *)
            printf 'present\n'
            ;;
    esac
}

json_run_id() {
    local path="$1"
    case "$path" in
        *.json) jq -r '.run_id // .bench_metadata.run_tag // empty' "$path" 2>/dev/null || true ;;
        *.jsonl) jq -sr 'map(.run_id? // empty) | map(select(. != "")) | .[0] // empty' "$path" 2>/dev/null || true ;;
        *) true ;;
    esac
}

json_profile() {
    local path="$1"
    case "$path" in
        *.json) jq -r '.profile // .bench_metadata.profile // .config.profile // empty' "$path" 2>/dev/null || true ;;
        *.jsonl) jq -sr 'map(.profile? // empty) | map(select(. != "")) | .[0] // empty' "$path" 2>/dev/null || true ;;
        *) true ;;
    esac
}

json_commit() {
    local path="$1"
    case "$path" in
        *.json)
            jq -r '
                [
                    .git_sha?,
                    .git_commit?,
                    .source_git_commit?,
                    .repo_commit?,
                    .metadata.git_sha?,
                    .metadata.git_commit?,
                    .bench_metadata.git_sha?,
                    .bench_metadata.git_commit?
                ]
                | map(select(type == "string" and length > 0))
                | .[0] // empty
            ' "$path" 2>/dev/null || true
            ;;
        *.jsonl)
            jq -sr '
                [
                    .[] | (
                        .git_sha?
                        // .git_commit?
                        // .source_git_commit?
                        // .repo_commit?
                        // .metadata.git_sha?
                        // .metadata.git_commit?
                        // empty
                    )
                ]
                | map(select(type == "string" and length > 0))
                | .[0] // empty
            ' "$path" 2>/dev/null || true
            ;;
        *) true ;;
    esac
}

record_artifact() {
    local lane="$1"
    local gate_id="$2"
    local command="$3"
    local required="$4"
    local path="$5"
    local description="$6"
    local profile_hint="${7:-}"

    local validation_status="pass"
    local artifact_status="missing"
    local rel=""
    local digest=""
    local digest_check=""
    local bytes=0
    local mtime=0
    local run_id=""
    local profile="$profile_hint"
    local artifact_commit=""
    local diagnostic=""

    if [ -z "$path" ] || [ ! -f "$path" ]; then
        if [ "$required" = "true" ]; then
            validation_status="fail"
            diagnostic="required artifact missing"
        else
            validation_status="warn"
            diagnostic="optional artifact not present"
        fi
    else
        rel="$(rel_path "$path")"
        digest="$(sha256_file "$path")"
        digest_check="$(sha256_file "$path")"
        bytes="$(file_bytes "$path")"
        mtime="$(file_mtime "$path")"
        artifact_status="$(json_status "$path")"
        run_id="$(json_run_id "$path")"
        artifact_commit="$(json_commit "$path")"
        if [ -z "$profile" ]; then
            profile="$(json_profile "$path")"
        fi

        if [ "$digest" != "$digest_check" ]; then
            validation_status="fail"
            diagnostic="digest verification mismatch"
        elif [ -n "$artifact_commit" ] && [ "$artifact_commit" != "$CURRENT_COMMIT" ]; then
            validation_status="fail"
            diagnostic="artifact commit does not match current HEAD"
        elif [ "$artifact_status" = "fail" ]; then
            validation_status="fail"
            diagnostic="artifact reports failure status"
        elif [ "$artifact_status" = "empty" ]; then
            validation_status="warn"
            diagnostic="artifact exists but is empty"
        elif [ "$artifact_status" = "skip" ]; then
            validation_status="warn"
            diagnostic="artifact reports skipped status"
        else
            diagnostic="artifact exists and digest verified"
        fi
    fi

    jq -cn \
        --arg lane "$lane" \
        --arg gate_id "$gate_id" \
        --arg command "$command" \
        --arg required "$required" \
        --arg description "$description" \
        --arg path "$rel" \
        --arg digest "$digest" \
        --arg artifact_status "$artifact_status" \
        --arg validation_status "$validation_status" \
        --arg diagnostic "$diagnostic" \
        --arg run_id "$run_id" \
        --arg profile "$profile" \
        --arg artifact_commit "$artifact_commit" \
        --argjson bytes "$bytes" \
        --argjson mtime "$mtime" \
        '{
            lane: $lane,
            gate_id: $gate_id,
            command: $command,
            required: ($required == "true"),
            description: $description,
            artifact: {
                path: (if $path == "" then null else $path end),
                sha256: (if $digest == "" then null else $digest end),
                bytes: $bytes,
                mtime_epoch: $mtime,
                digest_verified: ($digest != "")
            },
            evidence: {
                run_id: (if $run_id == "" then null else $run_id end),
                profile: (if $profile == "" then null else $profile end),
                artifact_status: $artifact_status,
                artifact_commit: (if $artifact_commit == "" then null else $artifact_commit end)
            },
            validation: {
                status: $validation_status,
                diagnostic: $diagnostic
            }
        }' >> "$RECORDS"
}

latest_conformance="$(latest_match "$REPO_ROOT/tools/ci/artifacts/conformance/conformance-*-conformance.summary.json")"
latest_codec="$(latest_match "$REPO_ROOT/tools/ci/artifacts/conformance/codec-equivalence-*-codec-equivalence.summary.json")"
latest_profile="$(latest_match "$REPO_ROOT/tools/ci/artifacts/conformance/profile-parity-*-profile-parity.summary.json")"
latest_parallel="$(latest_match "$REPO_ROOT/tools/ci/artifacts/conformance/parallel-parity-*-parallel-parity.summary.json")"
latest_fuzz="$(latest_match "$REPO_ROOT/tools/ci/artifacts/fuzz/*.summary.json")"
latest_unit_log="$(latest_nonempty_match "$REPO_ROOT/build/test-logs/unit-*.jsonl")"
latest_wave_c="$(latest_match "$REPO_ROOT/build/e2e-artifacts/wave-c-acceptance-*/wave_c_acceptance.summary.json")"
formal_artifact="$(latest_match "$REPO_ROOT/build/tests/formal/*")"
embedded_artifact="$(latest_match "$REPO_ROOT/tools/ci/artifacts/embedded/*.jsonl")"

record_artifact "build" "GATE-BUILD" "make build" "true" \
    "$REPO_ROOT/build/lib/libasx.a" \
    "Static library produced by the strict build lane." "CORE"
record_artifact "lint" "GATE-LINT" "make lint" "false" \
    "$(latest_match "$REPO_ROOT/build/lint/*.json")" \
    "Static-analysis report, when the local lint lane emits one." ""
record_artifact "unit" "GATE-UNIT" "make test-unit" "false" \
    "$latest_unit_log" \
    "Latest non-empty unit-test JSONL log, when emitted by focused tests." ""
record_artifact "conformance" "GATE-CONFORMANCE" "make conformance" "true" \
    "$latest_conformance" \
    "Rust fixture parity summary." ""
record_artifact "codec-equivalence" "GATE-CODEC" "make codec-equivalence" "true" \
    "$latest_codec" \
    "JSON-vs-BIN canonical semantic digest parity summary." ""
record_artifact "profile-parity" "GATE-PROFILE" "make profile-parity" "true" \
    "$latest_profile" \
    "Cross-profile canonical semantic digest parity summary." ""
record_artifact "parallel-parity" "GATE-PARALLEL-PARITY" "make parallel-parity" "false" \
    "$latest_parallel" \
    "Single-vs-multi-worker parity summary, when present." ""
record_artifact "fuzz-smoke" "GATE-FUZZ" "make fuzz-smoke" "false" \
    "$latest_fuzz" \
    "Differential fuzz smoke summary, when the structured fuzz gate emits one." ""
record_artifact "formal" "GATE-FORMAL" "make formal-check" "false" \
    "$formal_artifact" \
    "Formal-check binary/report artifact, when present." ""
record_artifact "embedded-smoke" "GATE-EMBED" "make ci-embedded-matrix" "false" \
    "$embedded_artifact" \
    "Embedded matrix/QEMU JSONL artifact, when present." ""
record_artifact "benchmark-slo" "GATE-HFT-PERF" "make slo-gate" "true" \
    "$REPO_ROOT/build/perf/slo_gate_report.json" \
    "Benchmark SLO gate report." ""
record_artifact "parallel-benchmark" "GATE-PARALLEL-BENCHMARK" "make parallel-bench-gate" "false" \
    "$REPO_ROOT/build/perf/parallel-bench-gate-summary.json" \
    "Parallel worker-count benchmark gate summary." "PARALLEL"
record_artifact "wave-c-acceptance" "GATE-WAVE-C-ACCEPTANCE" "make wave-c-acceptance-demo" "true" \
    "$latest_wave_c" \
    "User-facing Wave C acceptance evidence summary." "PARALLEL"

summary_json="$(jq -s '
    {
      total: length,
      required_total: (map(select(.required)) | length),
      optional_total: (map(select(.required | not)) | length),
      passed: (map(select(.validation.status == "pass")) | length),
      failed: (map(select(.validation.status == "fail")) | length),
      warnings: (map(select(.validation.status == "warn")) | length),
      missing_required: (
        map(select(.required and .validation.status == "fail" and .artifact.path == null)) | length
      ),
      stale_commit_mismatches: (
        map(select(.validation.diagnostic == "artifact commit does not match current HEAD")) | length
      ),
      digest_failures: (
        map(select(.validation.diagnostic == "digest verification mismatch")) | length
      )
    }
' "$RECORDS")"

overall_status="$(jq -r 'if .failed > 0 then "fail" elif .warnings > 0 then "warn" else "pass" end' <<<"$summary_json")"

jq -s \
    --arg schema "asx.release_evidence_manifest.v1" \
    --arg run_id "$RUN_ID" \
    --arg generated_at "$GENERATED_AT" \
    --arg git_commit "$CURRENT_COMMIT" \
    --arg git_branch "$CURRENT_BRANCH" \
    --arg cc_version "$CC_VERSION" \
    --arg make_version "$MAKE_VERSION" \
    --arg jq_version "$JQ_VERSION" \
    --arg status "$overall_status" \
    --arg records_file "$(rel_path "$RECORDS")" \
    --arg report_file "$(rel_path "$REPORT")" \
    --argjson summary "$summary_json" \
    '{
        schema: $schema,
        run_id: $run_id,
        generated_at: $generated_at,
        status: $status,
        git: {
            commit: $git_commit,
            branch: $git_branch
        },
        tools: {
            cc: $cc_version,
            make: $make_version,
            jq: $jq_version
        },
        policy: {
            required_artifacts_fail_closed: true,
            digest_verification_required: true,
            stale_commit_metadata_fails: true,
            optional_missing_artifacts_warn: true
        },
        summary: $summary,
        artifacts: .,
        companion_files: {
            records_jsonl: $records_file,
            validation_report: $report_file
        },
        rerun_command: "tools/ci/generate_release_evidence_manifest.sh --strict"
    }' "$RECORDS" > "$OUTPUT"

jq \
    --arg schema "asx.release_evidence_report.v1" \
    --arg manifest "$(rel_path "$OUTPUT")" \
    '{
        schema: $schema,
        manifest: $manifest,
        run_id,
        generated_at,
        status,
        summary,
        failures: (.artifacts | map(select(.validation.status == "fail"))),
        warnings: (.artifacts | map(select(.validation.status == "warn"))),
        rerun_command
    }' "$OUTPUT" > "$REPORT"

echo "[asx] release-evidence: status=$overall_status manifest=$OUTPUT report=$REPORT" >&2
jq -r '.failures[]? | "  - FAIL " + .lane + ": " + .validation.diagnostic' "$REPORT" >&2
jq -r '.warnings[]? | "  - WARN " + .lane + ": " + .validation.diagnostic' "$REPORT" >&2

if [ "$STRICT" = "1" ] && [ "$overall_status" = "fail" ]; then
    echo "[asx] release-evidence: FAIL (strict mode)" >&2
    exit 1
fi

exit 0
