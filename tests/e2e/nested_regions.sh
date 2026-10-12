#!/usr/bin/env bash
# nested_regions.sh — e2e family for nested parent/child region lifecycle
#
# Exercises nested region shutdown ordering, unlinking, and Q2 quiescence
# evidence reporting, body-driven normal/exceptional close and scope retry.
# Guarded-mask worker/joiner details share this family's existing JSONL report.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

source "$SCRIPT_DIR/harness.sh"

e2e_init "nested-regions" "E2E-NESTED-REGIONS"

E2E_BIN="${E2E_ARTIFACT_DIR}/e2e_nested_regions"
NESTED_FLAGS=""
if [ -n "${ASX_E2E_RESOURCE_CLASS:-${RESOURCE_CLASS:-}}" ]; then
    case "${ASX_E2E_RESOURCE_CLASS:-${RESOURCE_CLASS}}" in
        R1|1) NESTED_FLAGS="-DASX_RESOURCE_CLASS=1" ;;
        R2|2) NESTED_FLAGS="-DASX_RESOURCE_CLASS=2" ;;
        R3|3) NESTED_FLAGS="-DASX_RESOURCE_CLASS=3" ;;
        *) echo "[e2e] invalid nested-region resource class" >&2; exit 2 ;;
    esac
fi

NESTED_BUILD_SHA="$(git -C "$E2E_PROJECT_ROOT" rev-parse HEAD)"
if [ -n "$(git -C "$E2E_PROJECT_ROOT" status --porcelain --untracked-files=no -- src include tests/e2e Makefile)" ] ||
    [ -n "$(git -C "$E2E_PROJECT_ROOT" ls-files --others --exclude-standard -- src include tests/e2e)" ]; then
    NESTED_BUILD_SHA="${NESTED_BUILD_SHA}_dirty_draft"
fi
NESTED_RUST_REV="$(python3 -I -c 'import json, sys; print(json.load(open(sys.argv[1]))["provenance"]["rust_baseline_commit"])' \
    "$E2E_PROJECT_ROOT/fixtures/rust_reference_v2/cancel-masked-nested-001.json")"
NESTED_FINGERPRINT="incomplete:build-not-verified"

# Preserve the shared envelope and add the K17 fields locally, including
# actual observations rather than another report or a readiness consumer.
_e2e_emit_record() {
    python3 -I - "$_E2E_REPORT_FILE" "$E2E_RUN_ID" "$_E2E_FAMILY_ID" "$_E2E_LANE_ID" \
        "$E2E_PROFILE" "$E2E_CODEC" "$E2E_SEED" "$NESTED_RUST_REV" "$NESTED_BUILD_SHA" \
        "$NESTED_FINGERPRINT" "$_E2E_TOTAL" "$1" "$2" "${3:-}" "${4:-}" "${5:-}" \
        "$E2E_POLICY_ID" "$_E2E_FIRST_FAILURE" <<'PY'
from datetime import datetime, timezone
import json
import sys
path, run, family, suite, profile, codec, seed, rust, sha, fingerprint, index, test, status, diagnostic, digest, detail, policy, first = sys.argv[1:]
row = {'ts': datetime.now(timezone.utc).isoformat(), 'run_id': run, 'layer': 'e2e',
       'subsystem': family, 'suite': suite, 'test': test, 'status': status,
       'event_index': int(index), 'profile': profile, 'codec': codec, 'seed': int(seed),
       'rust_rev': rust, 'c_git_sha': sha, 'config_fingerprint': fingerprint,
       'first_divergence': (first if test == '_summary' else diagnostic) if status == 'fail' else None}
if policy: row['policy_id'] = policy
if digest: row['digest'] = digest
if diagnostic: row['error'] = {'message': diagnostic}
if detail:
    observed = json.loads(detail)
    if observed.pop('test') != test: raise ValueError('detail scenario differs from envelope')
    row.update(observed)
    row['kind'] = 'mask_detail'
with open(path, 'a') as output:
    output.write(json.dumps(row, separators=(',', ':')) + '\n')
PY
}

if ! e2e_build "${SCRIPT_DIR}/e2e_nested_regions.c" "$E2E_BIN" "$NESTED_FLAGS"; then
    e2e_scenario "nested_regions.build" "compilation failed" "fail"
    e2e_finish
    exit $?
fi
NESTED_FINGERPRINT="$(python3 -I - "$E2E_BUILD_DIR" "$SCRIPT_DIR/e2e_nested_regions.c" \
    "$E2E_PROFILE" "$E2E_CODEC" "$E2E_DETERMINISTIC" "$E2E_SEED" "$NESTED_FLAGS" <<'PY'
import hashlib
from pathlib import Path
import sys
build, source, *options = sys.argv[1:]
digest = hashlib.sha256("\0".join(options).encode())
for path in [Path(build) / '.build-config', Path(build) / 'lib/libasx.a', Path(source)]:
    digest.update(path.name.encode() + b'\0')
    digest.update(path.read_bytes() if path.is_file() else b'missing')
print('sha256:' + digest.hexdigest())
PY
)"
cat "${E2E_BIN}.build.log" >&2
e2e_scenario "nested_regions.build" "" "pass"

e2e_run_binary "$E2E_BIN" "${E2E_ARTIFACT_DIR}/nested_regions.stderr" "nested_regions"
OUTPUT="$E2E_LAST_OUTPUT"
cat "${E2E_ARTIFACT_DIR}/nested_regions.stderr" >&2

DETAIL_COUNT=0
while IFS= read -r line; do
    case "$line" in
        DETAIL\ *)
            DETAIL_JSON="${line#DETAIL }"
            DETAIL_TEST="$(python3 -I -c 'import json, sys; print(json.loads(sys.argv[1])["test"])' "$DETAIL_JSON")"
            DETAIL_RESULT="$(printf '%s\n' "$OUTPUT" | awk -v test="$DETAIL_TEST" \
                '$1 == "SCENARIO" && $2 == test { result=$3 } END { print result }')"
            DETAIL_DIAGNOSTIC="$(printf '%s\n' "$OUTPUT" | awk -v test="$DETAIL_TEST" \
                '$1 == "SCENARIO" && $2 == test { for (i=4; i<=NF; i++) printf "%s%s", i==4 ? "" : " ", $i }')"
            case "$DETAIL_RESULT" in
                pass|fail) _e2e_emit_record "$DETAIL_TEST" "$DETAIL_RESULT" "$DETAIL_DIAGNOSTIC" "" "$DETAIL_JSON" ;;
                *) echo "[e2e] mask detail has no real scenario result" >&2; exit 1 ;;
            esac
            DETAIL_COUNT=$((DETAIL_COUNT + 1))
            ;;
    esac
done <<< "$OUTPUT"
if python3 -I - "$_E2E_REPORT_FILE" "$E2E_RUN_ID" "$DETAIL_COUNT" <<'PY'
import json
import sys
rows = [json.loads(line) for line in open(sys.argv[1])]
count = int(sys.argv[3])
if count <= 0: raise ValueError('current binary emitted zero mask detail records')
details = [row for row in rows if row.get('run_id') == sys.argv[2] and row.get('kind') == 'mask_detail']
details = details[-count:]
cases = {'nested.mask_guard_restores', 'nested.raw_mask_release_required', 'nested.mask_guard_depth_limit'}
required = {'run_id', 'suite', 'test', 'status', 'seed', 'profile', 'codec', 'rust_rev',
            'c_git_sha', 'config_fingerprint', 'first_divergence', 'poll', 'mask_depth',
            'cancel_phase', 'checkpoint_masked', 'checkpoint_cancelled', 'terminal_outcome'}
if len(details) != count or {row['test'] for row in details} != cases:
    raise ValueError('missing mask scenarios or zero-run detail evidence')
for test in cases:
    observed = [row for row in details if row['test'] == test]
    if {row['actor'] for row in observed} != {'worker', 'joiner'}:
        raise ValueError('missing real worker/joiner observations: ' + test)
for row in details:
    if not required.issubset(row): raise ValueError('missing K17 observation/envelope fields')
print('[e2e] mask detail records:', len(details))
PY
then
    e2e_scenario "nested_regions.mask_details" "records=${DETAIL_COUNT}" "pass"
else
    e2e_scenario "nested_regions.mask_details" "missing or malformed mask detail evidence" "fail"
fi

DIGEST="$(printf '%s\n' "$OUTPUT" | awk '$1 == "DIGEST" { digest=$2 } END { print digest }')"

if [ -n "$DIGEST" ]; then
    e2e_scenario "nested_regions.trace_digest" "" "pass" "sha256:${DIGEST}"
fi

set +e
e2e_finish
FINISH_RC=$?
set -e

exit $FINISH_RC
