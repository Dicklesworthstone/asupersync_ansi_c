#!/usr/bin/env bash
# =============================================================================
# validate_schemas.sh — JSON schema validation gate (bd-16r)
#
# Validates generated JSON artifacts against their corresponding schemas
# in schemas/. Uses python3 + jsonschema for Draft 2020-12 compliance.
#
# Artifact:
#   build/conformance/schema_validation_<run_id>.json
#
# Exit codes:
#   0: pass (all validated) or skip (no artifacts found and not --strict)
#   1: gate failure (validation errors)
#   2: usage/configuration error
#
# SPDX-License-Identifier: MIT
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
RUN_ID="${ASX_CI_RUN_TAG:-schema-validation-$(date -u +%Y%m%dT%H%M%SZ)}"
ARTIFACT_DIR="$REPO_ROOT/build/conformance"
ARTIFACT_FILE="$ARTIFACT_DIR/schema_validation_${RUN_ID}.json"
STRICT=0

usage() {
    cat <<'EOF'
Usage: tools/ci/validate_schemas.sh [--strict] [--run-id <id>]

Options:
  --strict       Fail if no artifacts found (default: skip)
  --run-id <id>  Override run id used in artifact filename
  --help         Show this help and exit
EOF
    exit 2
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --strict)
            STRICT=1
            shift
            ;;
        --run-id)
            [[ $# -ge 2 ]] || usage
            RUN_ID="$2"
            ARTIFACT_FILE="$ARTIFACT_DIR/schema_validation_${RUN_ID}.json"
            shift 2
            ;;
        --help|-h)
            usage
            ;;
        *)
            echo "[asx] schema-validation: unknown option: $1" >&2
            usage
            ;;
    esac
done

mkdir -p "$ARTIFACT_DIR"

# -----------------------------------------------------------------------
# Check prerequisites
# -----------------------------------------------------------------------

if ! python3 -c "import jsonschema" 2>/dev/null; then
    echo "[asx] schema-validation: SKIP (python3 jsonschema module not available)" >&2
    jq -n \
      --arg run_id "$RUN_ID" \
      --arg status "skip" \
      --arg reason "python3 jsonschema not available" \
      '{kind:"schema_validation",run_id:$run_id,status:$status,reason:$reason,pass:true,validated:0,failed:0,skipped:0,errors:[]}' \
      >"$ARTIFACT_FILE"
    exit 0
fi

# -----------------------------------------------------------------------
# Validation runner (Python)
# -----------------------------------------------------------------------

python3 - "$REPO_ROOT" "$ARTIFACT_FILE" "$STRICT" "$RUN_ID" <<'PYEOF'
import json
import os
import sys
import glob

repo_root = sys.argv[1]
artifact_file = sys.argv[2]
strict = sys.argv[3] == "1"
run_id = sys.argv[4]

try:
    from jsonschema import Draft202012Validator, ValidationError
except ImportError:
    from jsonschema import Draft7Validator as Draft202012Validator, ValidationError

schemas_dir = os.path.join(repo_root, "schemas")
fixtures_dir = os.path.join(repo_root, "fixtures")

# Schema loading
def load_schema(path):
    with open(path, "r") as f:
        return json.load(f)

# Validation targets: (glob_pattern, schema_file, description)
targets = []

# 1. Canonical fixtures
canonical_schema_path = os.path.join(schemas_dir, "canonical_fixture.schema.json")
if os.path.isfile(canonical_schema_path):
    targets.append({
        "schema_path": canonical_schema_path,
        "schema_name": "canonical_fixture",
        "glob": os.path.join(fixtures_dir, "rust_reference", "**", "*.json"),
        "description": "Canonical fixture files",
    })

# 3. Core fixture family manifests
manifest_schema_path = os.path.join(schemas_dir, "core_fixture_family_manifest.schema.json")
if os.path.isfile(manifest_schema_path):
    targets.append({
        "schema_path": manifest_schema_path,
        "schema_name": "core_fixture_family_manifest",
        "glob": os.path.join(fixtures_dir, "**", "*core*manifest*.json"),
        "description": "Core fixture family manifests",
    })

# 4. Robustness fixture family manifests
robustness_schema_path = os.path.join(schemas_dir, "robustness_fixture_family_manifest.schema.json")
if os.path.isfile(robustness_schema_path):
    targets.append({
        "schema_path": robustness_schema_path,
        "schema_name": "robustness_fixture_family_manifest",
        "glob": os.path.join(fixtures_dir, "**", "*robustness*manifest*.json"),
        "description": "Robustness fixture family manifests",
    })

# 5. Vertical continuity fixture family manifests
vertical_schema_path = os.path.join(schemas_dir, "vertical_continuity_fixture_family_manifest.schema.json")
if os.path.isfile(vertical_schema_path):
    targets.append({
        "schema_path": vertical_schema_path,
        "schema_name": "vertical_continuity_fixture_family_manifest",
        "glob": os.path.join(fixtures_dir, "**", "*vertical*manifest*.json"),
        "description": "Vertical continuity fixture family manifests",
    })

# 6. Fixture capture manifests
capture_schema_path = os.path.join(schemas_dir, "fixture_capture_manifest.schema.json")
if os.path.isfile(capture_schema_path):
    targets.append({
        "schema_path": capture_schema_path,
        "schema_name": "fixture_capture_manifest",
        "glob": os.path.join(fixtures_dir, "**", "*capture*manifest*.json"),
        "description": "Fixture capture manifests",
    })

# 7. Trace event golden examples
trace_event_schema_path = os.path.join(schemas_dir, "trace_event.schema.json")
if os.path.isfile(trace_event_schema_path):
    targets.append({
        "schema_path": trace_event_schema_path,
        "schema_name": "trace_event",
        "glob": os.path.join(fixtures_dir, "trace_events", "**", "*.json"),
        "description": "Trace event schema golden examples",
    })

# 8. Incident evidence bundle golden examples
incident_bundle_schema_path = os.path.join(schemas_dir, "incident_bundle.schema.json")
if os.path.isfile(incident_bundle_schema_path):
    targets.append({
        "schema_path": incident_bundle_schema_path,
        "schema_name": "incident_bundle",
        "glob": os.path.join(fixtures_dir, "incident_bundles", "**", "*.json"),
        "description": "Incident evidence bundle golden examples",
    })

# Deduplicate files across targets to avoid double-validation
validated_files = set()
results = []
total_pass = 0
total_fail = 0
total_skip = 0
errors = []

for target in targets:
    schema_path = target["schema_path"]
    if not os.path.isfile(schema_path):
        continue

    schema = load_schema(schema_path)
    validator = Draft202012Validator(schema)

    matched_files = sorted(glob.glob(target["glob"], recursive=True))

    for fpath in matched_files:
        if not os.path.isfile(fpath):
            continue
        rel_path = os.path.relpath(fpath, repo_root)
        if rel_path in validated_files:
            continue
        validated_files.add(rel_path)

        try:
            with open(fpath, "r") as f:
                data = json.load(f)
        except json.JSONDecodeError as e:
            total_fail += 1
            errors.append({
                "file": rel_path,
                "schema": target["schema_name"],
                "error": f"JSON parse error: {e}",
            })
            continue

        errs = list(validator.iter_errors(data))
        if errs:
            total_fail += 1
            for err in errs[:3]:  # Cap at 3 errors per file
                errors.append({
                    "file": rel_path,
                    "schema": target["schema_name"],
                    "path": list(str(p) for p in err.absolute_path),
                    "error": err.message[:200],
                })
        else:
            total_pass += 1

# Generate artifact
status = "pass"
reason = "all fixtures validate against schemas"

if total_fail > 0:
    status = "fail"
    reason = f"{total_fail} file(s) failed validation"
elif total_pass == 0:
    if strict:
        status = "fail"
        reason = "no fixture files found for validation (strict mode)"
    else:
        status = "skip"
        reason = "no fixture files found for validation"

artifact = {
    "kind": "schema_validation",
    "run_id": run_id,
    "status": status,
    "pass": status != "fail",
    "reason": reason,
    "validated": total_pass,
    "failed": total_fail,
    "skipped": total_skip,
    "total_files": len(validated_files),
    "schemas_checked": list(set(t["schema_name"] for t in targets if os.path.isfile(t["schema_path"]))),
    "errors": errors,
}

with open(artifact_file, "w") as f:
    json.dump(artifact, f, indent=2)
    f.write("\n")

# Print summary
print(f"[asx] schema-validation: {total_pass} passed, {total_fail} failed, "
      f"{len(validated_files)} files checked", file=sys.stderr)

if errors:
    for err in errors[:10]:
        path_str = " → ".join(err.get("path", []))
        loc = f" at {path_str}" if path_str else ""
        print(f"[asx] schema-validation: FAIL {err['file']}{loc}: {err['error'][:120]}", file=sys.stderr)

if status == "fail":
    print(f"[asx] schema-validation: FAIL artifact={artifact_file}", file=sys.stderr)
    sys.exit(1)
elif status == "skip":
    print(f"[asx] schema-validation: SKIP artifact={artifact_file}", file=sys.stderr)
    sys.exit(0)
else:
    print(f"[asx] schema-validation: PASS artifact={artifact_file}", file=sys.stderr)
    sys.exit(0)
PYEOF
