#!/usr/bin/env bash
# run_release_install_smoke.sh - install and run from a packaged release artifact
#
# Verifies a release tarball from the operator point of view:
#   - artifact, checksum, and provenance exist and match
#   - lib/header payload installs from the extracted artifact into a fresh prefix
#   - README quickstart compiles and runs against only the installed prefix
#   - a machine-readable report records commands and pass/fail statuses
#
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

ARTIFACT_PATH=""
VERSION=""
TARGET=""
REPORT_DIR="${REPO_ROOT}/build/release/install-smoke"

usage() {
    cat <<'USAGE'
Usage: tools/ci/run_release_install_smoke.sh --artifact <asx-target.tar.xz> --version <x.y.z> --target <id>

Options:
  --artifact <path>     Release tar.xz artifact to verify and install
  --version <x.y.z>     Release version expected in evidence
  --target <id>         Release target expected in evidence
  --report-dir <dir>    Report/work directory root (default: build/release/install-smoke)
  -h, --help            Show this help
USAGE
}

while [ $# -gt 0 ]; do
    case "$1" in
        --artifact) ARTIFACT_PATH="$2"; shift 2 ;;
        --version) VERSION="$2"; shift 2 ;;
        --target) TARGET="$2"; shift 2 ;;
        --report-dir) REPORT_DIR="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage; exit 2 ;;
    esac
done

if [ -z "$ARTIFACT_PATH" ] || [ -z "$VERSION" ] || [ -z "$TARGET" ]; then
    echo "missing required --artifact/--version/--target arguments" >&2
    usage
    exit 2
fi

if ! command -v jq >/dev/null 2>&1; then
    echo "jq is required to emit the release install smoke report" >&2
    exit 2
fi

ARTIFACT_PATH="$(cd "$(dirname "$ARTIFACT_PATH")" && pwd)/$(basename "$ARTIFACT_PATH")"
ARTIFACT_BASE="$(basename "$ARTIFACT_PATH" .tar.xz)"
ARTIFACT_DIR="$(dirname "$ARTIFACT_PATH")"
SHA_PATH="${ARTIFACT_PATH}.sha256"
SIGSTORE_PATH="${ARTIFACT_PATH}.sigstore.json"
PROVENANCE_PATH="${ARTIFACT_DIR}/${ARTIFACT_BASE}.provenance.json"

RUN_ID="release-install-smoke-$(date -u +%Y%m%dT%H%M%SZ)-$$"
WORK_DIR="${REPORT_DIR}/${RUN_ID}"
EXTRACT_DIR="${WORK_DIR}/extract"
PREFIX_DIR="${WORK_DIR}/prefix"
BUILD_DIR="${WORK_DIR}/build"
STEPS_FILE="${WORK_DIR}/steps.jsonl"
REPORT_FILE="${WORK_DIR}/release_install_smoke_report.json"
QUICKSTART_SRC="${BUILD_DIR}/readme_quickstart.c"
QUICKSTART_BIN="${BUILD_DIR}/readme_quickstart"
CC_BIN="${CC:-gcc}"
CC_COMMAND="${CC_BIN%% *}"
SHA_TOOL=""

mkdir -p "$EXTRACT_DIR" "$PREFIX_DIR" "$BUILD_DIR"
: >"$STEPS_FILE"

json_escape_path() {
    printf '%s' "$1"
}

record_step() {
    local name="$1"
    local status="$2"
    local command="$3"
    local diagnostic="${4:-}"

    jq -cn \
        --arg name "$name" \
        --arg status "$status" \
        --arg command "$command" \
        --arg diagnostic "$diagnostic" \
        '{
          name: $name,
          status: $status,
          command: $command
        } + (if $diagnostic == "" then {} else {diagnostic: $diagnostic} end)' \
        >>"$STEPS_FILE"
}

sha256_file() {
    local file="$1"
    if [ "$SHA_TOOL" = "sha256sum" ]; then
        sha256sum "$file" | awk '{print $1}'
        return
    fi
    if [ "$SHA_TOOL" = "shasum" ]; then
        shasum -a 256 "$file" | awk '{print $1}'
        return
    fi
    echo "missing sha256 tool (need sha256sum or shasum)" >&2
    return 1
}

finalize_report() {
    local status="$1"
    local diagnostic="${2:-}"
    local checksum="${3:-}"
    local git_commit

    git_commit="$(git -C "$REPO_ROOT" rev-parse HEAD 2>/dev/null || echo unknown)"

    jq -s \
        --arg schema "asx.release_install_smoke.v1" \
        --arg run_id "$RUN_ID" \
        --arg status "$status" \
        --arg diagnostic "$diagnostic" \
        --arg generated_at "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
        --arg git_commit "$git_commit" \
        --arg version "$VERSION" \
        --arg target "$TARGET" \
        --arg artifact_path "$(json_escape_path "$ARTIFACT_PATH")" \
        --arg checksum "$checksum" \
        --arg sha_path "$(json_escape_path "$SHA_PATH")" \
        --arg sigstore_path "$(json_escape_path "$SIGSTORE_PATH")" \
        --arg provenance_path "$(json_escape_path "$PROVENANCE_PATH")" \
        --arg prefix "$(json_escape_path "$PREFIX_DIR")" \
        --arg work_dir "$(json_escape_path "$WORK_DIR")" \
        '{
          schema: $schema,
          run_id: $run_id,
          status: $status,
          diagnostic: (if $diagnostic == "" then null else $diagnostic end),
          generated_at: $generated_at,
          git: {commit: $git_commit},
          release: {version: $version, target: $target},
          artifact: {
            path: $artifact_path,
            sha256: $checksum,
            sha256_file: $sha_path,
            sigstore_bundle: $sigstore_path,
            provenance: $provenance_path
          },
          install: {
            prefix: $prefix,
            work_dir: $work_dir
          },
          steps: .
        }' "$STEPS_FILE" >"$REPORT_FILE"

    echo "[asx] release-install-smoke: status=$status report=$REPORT_FILE"
}

fail_step() {
    local name="$1"
    local command="$2"
    local diagnostic="$3"
    local checksum="${4:-}"

    record_step "$name" "fail" "$command" "$diagnostic"
    finalize_report "fail" "$diagnostic" "$checksum"
    exit 1
}

pass_step() {
    record_step "$1" "pass" "$2" "${3:-}"
}

require_command() {
    local name="$1"
    local label="${2:-$name}"
    local command_text="command -v $name"

    if ! command -v "$name" >/dev/null 2>&1; then
        fail_step "command_available_${label}" "$command_text" "required command is missing"
    fi
    pass_step "command_available_${label}" "$command_text"
}

require_command "tar"
require_command "awk"
require_command "install"
require_command "cp"
require_command "git"
require_command "$CC_COMMAND" "cc"

if command -v sha256sum >/dev/null 2>&1; then
    SHA_TOOL="sha256sum"
    pass_step "command_available_sha256" "command -v sha256sum"
elif command -v shasum >/dev/null 2>&1; then
    SHA_TOOL="shasum"
    pass_step "command_available_sha256" "command -v shasum"
else
    fail_step "command_available_sha256" "command -v sha256sum || command -v shasum" "required sha256 tool is missing"
fi

if [ ! -f "$ARTIFACT_PATH" ]; then
    fail_step "artifact_exists" "test -f $ARTIFACT_PATH" "release artifact is missing"
fi
if [ ! -s "$ARTIFACT_PATH" ]; then
    fail_step "artifact_nonempty" "test -s $ARTIFACT_PATH" "release artifact is empty"
fi
if [ ! -f "$SHA_PATH" ]; then
    fail_step "checksum_file_exists" "test -f $SHA_PATH" "checksum file is missing"
fi
if [ ! -f "$SIGSTORE_PATH" ]; then
    fail_step "sigstore_file_exists" "test -f $SIGSTORE_PATH" "sigstore bundle is missing"
fi
if [ ! -f "$PROVENANCE_PATH" ]; then
    fail_step "provenance_file_exists" "test -f $PROVENANCE_PATH" "provenance file is missing"
fi
pass_step "artifact_files_exist" "test -f artifact checksum sigstore provenance"

EXPECTED_SHA="$(awk '{print $1}' "$SHA_PATH")"
ACTUAL_SHA="$(sha256_file "$ARTIFACT_PATH")"
if [ -z "$EXPECTED_SHA" ] || [ "$EXPECTED_SHA" != "$ACTUAL_SHA" ]; then
    fail_step "checksum_matches" "sha256($ARTIFACT_PATH) == $(basename "$SHA_PATH")" "checksum mismatch" "$ACTUAL_SHA"
fi
pass_step "checksum_matches" "sha256($ARTIFACT_PATH) == $(basename "$SHA_PATH")"

PROVENANCE_ARTIFACT="$(jq -r '.artifact // ""' "$PROVENANCE_PATH")"
PROVENANCE_SHA="$(jq -r '.integrity.sha256 // ""' "$PROVENANCE_PATH")"
PROVENANCE_GIT="$(jq -r '.git.sha // ""' "$PROVENANCE_PATH")"
HEAD_SHA="$(git -C "$REPO_ROOT" rev-parse HEAD 2>/dev/null || echo unknown)"
if [ "$PROVENANCE_ARTIFACT" != "$(basename "$ARTIFACT_PATH")" ]; then
    fail_step "provenance_artifact_matches" "jq .artifact $PROVENANCE_PATH" "provenance artifact does not name the tarball" "$ACTUAL_SHA"
fi
if [ "$PROVENANCE_SHA" != "$ACTUAL_SHA" ]; then
    fail_step "provenance_checksum_matches" "jq .integrity.sha256 $PROVENANCE_PATH" "provenance checksum mismatch" "$ACTUAL_SHA"
fi
if [ "$PROVENANCE_GIT" != "$HEAD_SHA" ]; then
    fail_step "provenance_commit_matches" "jq .git.sha $PROVENANCE_PATH" "provenance commit does not match HEAD" "$ACTUAL_SHA"
fi
pass_step "provenance_matches_artifact" "jq .artifact/.integrity.sha256/.git.sha $PROVENANCE_PATH"

if ! tar -xJf "$ARTIFACT_PATH" -C "$EXTRACT_DIR"; then
    fail_step "extract_archive" "tar -xJf $ARTIFACT_PATH -C $EXTRACT_DIR" "artifact extraction failed" "$ACTUAL_SHA"
fi
pass_step "extract_archive" "tar -xJf $ARTIFACT_PATH -C $EXTRACT_DIR"

PACKAGE_TOP="$(tar -tf "$ARTIFACT_PATH" | awk -F/ 'NR == 1 {print $1}')"
if [ -z "$PACKAGE_TOP" ] || [ "$PACKAGE_TOP" = "." ] || [ "$PACKAGE_TOP" = ".." ]; then
    fail_step "package_root_detected" "tar -tf $ARTIFACT_PATH" "could not detect package root" "$ACTUAL_SHA"
fi
PACKAGE_DIR="${EXTRACT_DIR}/${PACKAGE_TOP}"

if [ ! -s "${PACKAGE_DIR}/lib/libasx.a" ]; then
    fail_step "package_library_present" "test -s ${PACKAGE_DIR}/lib/libasx.a" "packaged libasx.a missing or empty" "$ACTUAL_SHA"
fi
if [ ! -f "${PACKAGE_DIR}/include/asx/asx.h" ]; then
    fail_step "package_umbrella_header_present" "test -f ${PACKAGE_DIR}/include/asx/asx.h" "packaged umbrella header missing" "$ACTUAL_SHA"
fi
pass_step "package_payload_present" "test packaged libasx.a and include/asx/asx.h"

install -d "${PREFIX_DIR}/lib" "${PREFIX_DIR}/include"
install -m 644 "${PACKAGE_DIR}/lib/libasx.a" "${PREFIX_DIR}/lib/libasx.a"
cp -R "${PACKAGE_DIR}/include/asx" "${PREFIX_DIR}/include/"
if [ ! -s "${PREFIX_DIR}/lib/libasx.a" ] || [ ! -f "${PREFIX_DIR}/include/asx/asx.h" ]; then
    fail_step "install_from_artifact" "install lib/include from extracted artifact to $PREFIX_DIR" "installed payload missing" "$ACTUAL_SHA"
fi
pass_step "install_from_artifact" "install lib/include from extracted artifact to $PREFIX_DIR"

cat >"$QUICKSTART_SRC" <<'C_EOF'
#include <asx/asx.h>

static asx_status noop_poll(void *user_data, asx_task_id self) {
    (void)user_data;
    (void)self;
    return ASX_OK;
}

int main(void) {
    asx_runtime rt;
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    asx_status st;

    asx_runtime_config_init(&cfg);
    st = asx_runtime_hooks_init(&hooks);
    if (st != ASX_OK) return 1;

    st = asx_runtime_init(&rt, &cfg, &hooks);
    if (st != ASX_OK) return 1;

    st = asx_region_open(&rid);
    if (st != ASX_OK) {
        asx_runtime_shutdown(&rt);
        return 1;
    }

    st = asx_task_spawn(rid, noop_poll, NULL, &tid);
    if (st != ASX_OK) {
        asx_runtime_shutdown(&rt);
        return 1;
    }

    budget = asx_budget_from_polls(64u);
    st = asx_scheduler_run(rid, &budget);
    asx_runtime_shutdown(&rt);
    return st == ASX_OK ? 0 : 2;
}
C_EOF

COMPILE_CMD="$CC_BIN -std=c99 -Wall -Wextra -Wpedantic -Werror -I${PREFIX_DIR}/include -o ${QUICKSTART_BIN} ${QUICKSTART_SRC} ${PREFIX_DIR}/lib/libasx.a"
if ! $CC_BIN -std=c99 -Wall -Wextra -Wpedantic -Werror \
    -I"${PREFIX_DIR}/include" \
    -o "$QUICKSTART_BIN" \
    "$QUICKSTART_SRC" \
    "${PREFIX_DIR}/lib/libasx.a"; then
    fail_step "quickstart_compile" "$COMPILE_CMD" "README quickstart failed to compile against installed prefix" "$ACTUAL_SHA"
fi
pass_step "quickstart_compile" "$COMPILE_CMD"

if ! "$QUICKSTART_BIN"; then
    fail_step "quickstart_run" "$QUICKSTART_BIN" "README quickstart binary failed" "$ACTUAL_SHA"
fi
pass_step "quickstart_run" "$QUICKSTART_BIN"

finalize_report "pass" "" "$ACTUAL_SHA"
