#!/usr/bin/env bash
# test_release_gate.sh — negative controls for the release gate (bd-9kll.18.6)
#
# The release workflow must not publish unless the required evidence is
# green at the tagged commit and the artifacts verify. This exercises the
# decision and the verification without issuing a release or pushing a tag:
#
#   A. policy cases: tools/ci/release_gate.py over evidence built from the
#      real workflow files, one mutation per case (wrong SHA, stale green run,
#      failed / skipped / cancelled / missing lane, CORE failure hiding HFT or
#      size, missing or zero-comparison report, inconsistent aggregate,
#      profile not covered, explicit optional exclusion, ...). A refusal must
#      be exit 1 with a structured reason; an allow must have none.
#   B. workflow contract: publish is reachable only through gate and build,
#      nothing else can upload release assets, verification demands a
#      signature, and the manifest still matches the workflow files.
#   C. artifact controls on synthetic archives: a corrupted archive, checksum,
#      provenance or signature bundle is rejected at the named step, a missing
#      verifier fails (it never skips), and the source archive of a clean
#      candidate passes. Canonical files are copied before corruption.
#   D. (ASX_RELEASE_E2E=1, builds the library) a real candidate: build the
#      archives, verify them, install and run a consumer and the doctor probe,
#      then corrupt copies of the real archive and require rejection.
#
# Scratch files live under $ASX_GATE_WORK_ROOT (default build/test-gates);
# logs JSONL to gates.jsonl beside it. Nothing is deleted.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
GATE_ROOT="${ASX_GATE_WORK_ROOT:-$REPO_ROOT/build}"
RUN_ID="release-gate-$(date -u +%Y%m%dT%H%M%SZ)-$$"
WORK="$GATE_ROOT/test-gates/$RUN_ID"
LOG_DIR="$GATE_ROOT/test-logs"
LOG="$LOG_DIR/gates.jsonl"
SMOKE="$REPO_ROOT/tools/ci/run_release_install_smoke.sh"
mkdir -p "$WORK" "$LOG_DIR"

failures=0
cases=0

log_case() {
    # name expected observed status detail
    python3 - "$RUN_ID" "$1" "$2" "$3" "$4" "$5" >>"$LOG" <<'PY'
import datetime
import json
import sys

row = dict(zip(("run_id", "test", "expected", "observed", "status", "detail"), sys.argv[1:]))
row.update(ts=datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
           layer="gates", suite="release_gate")
print(json.dumps(row, separators=(",", ":")))
PY
}

# verdict <name> <expected> <observed> <detail>: PASS when they are equal.
verdict() {
    local name="$1" expected="$2" observed="$3" detail="${4:-}"
    local status="pass"
    cases=$((cases + 1))
    if [ "$expected" != "$observed" ]; then
        status="fail"
        failures=$((failures + 1))
        echo "  FAIL: $name (expected $expected, observed $observed) $detail"
    else
        echo "  PASS: $name ($observed)"
    fi
    log_case "$name" "$expected" "$observed" "$status" "$detail"
}

echo "=== test_release_gate ($RUN_ID) ==="

# ---------------------------------------------------------------------------
# A. policy cases and B. workflow contract (Python)
# ---------------------------------------------------------------------------
if ! python3 -c 'import yaml' 2>/dev/null; then
    verdict "pyyaml_available" "yes" "no" "the gate cannot read the workflow files"
else
    python3 - "$REPO_ROOT" "$WORK" "$RUN_ID" "$LOG" <<'PY'
import copy
import datetime
import json
import os
import re
import subprocess
import sys

import yaml

repo, work, run_id, log = sys.argv[1:5]
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(repo, "tools", "ci"))
import release_gate as rg  # noqa: E402

tool = os.path.join(repo, "tools", "ci", "release_gate.py")
manifest_path = os.path.join(repo, "tools", "ci", "release_required_gates.json")
manifest = rg.load_manifest(manifest_path)
expected = rg.expected_jobs(manifest, repo)
SHA = "a" * 40
OTHER = "b" * 40
TAG = "v1.2.3"
failures = 0


def record(name, want, got, status, detail=""):
    row = {"run_id": run_id, "test": name, "expected": want, "observed": got, "status": status,
           "detail": detail, "layer": "gates", "suite": "release_gate",
           "ts": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")}
    with open(log, "a", encoding="utf-8") as stream:
        stream.write(json.dumps(row, separators=(",", ":")) + "\n")


def verdict(name, ok, want, got, detail=""):
    global failures
    status = "pass" if ok else "fail"
    if not ok:
        failures += 1
        print(f"  FAIL: {name} (expected {want}, observed {got}) {detail}")
    else:
        print(f"  PASS: {name} ({got})")
    record(name, want, got, status, detail)


def baseline():
    runs = []
    for wf in manifest["workflows"]:
        jobs = []
        for row in expected[wf["id"]]:
            steps = [{"name": s, "status": "completed", "conclusion": "success"}
                     for s in (wf.get("required_steps") or {}).get(row["job_id"], [])]
            jobs.append({"name": row["name"], "status": "completed",
                         "conclusion": "success", "steps": steps})
        runs.append({"id": 100 + len(runs), "workflow": wf["name"], "path": wf["path"],
                     "head_sha": SHA, "event": wf["events"][0], "status": "completed",
                     "conclusion": "success", "run_attempt": 1,
                     "created_at": "2026-10-13T00:00:00Z", "jobs": jobs})
    return {"schema": rg.EVIDENCE_SCHEMA, "repository": "owner/repo", "sha": SHA, "runs": runs,
            "reports": {"conformance_summary": {"total": 141, "fail": 0,
                                                "semantic_delta_pass": True}}}


def run_of(ev, wf_id):
    path = next(w["path"] for w in manifest["workflows"] if w["id"] == wf_id)
    return next(r for r in ev["runs"] if r["path"] == path)


def job_of(run, name):
    return next(j for j in run["jobs"] if j["name"] == name)


def decide(case, ev, manifest_file=manifest_path, tag=TAG, sha=SHA, profile=None):
    case_dir = os.path.join(work, "policy-" + case)
    os.makedirs(case_dir, exist_ok=True)
    ev_path = os.path.join(case_dir, "evidence.json")
    out_path = os.path.join(case_dir, "decision.json")
    with open(ev_path, "w", encoding="utf-8") as stream:
        json.dump(ev, stream, indent=2, sort_keys=True)
    cmd = [sys.executable, tool, "decide", "--manifest", manifest_file, "--workflows-dir", repo,
           "--evidence", ev_path, "--tag", tag, "--sha", sha, "--output", out_path]
    if profile:
        cmd += ["--profile", profile]
    proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
    decision = None
    if os.path.exists(out_path):
        with open(out_path, encoding="utf-8") as stream:
            decision = json.load(stream)
    return proc.returncode, decision, proc.stderr


def refuses(case, ev, codes, **kw):
    rc, decision, err = decide(case, ev, **kw)
    got = sorted({r["code"] for r in decision["reasons"]}) if decision else [f"exit{rc}:{err.strip()}"]
    ok = (rc == 1 and decision is not None and decision["decision"] == "refuse"
          and all(c in got for c in codes))
    verdict(case, ok, f"exit 1 with {sorted(codes)}", f"exit {rc} with {got}")


# --- A. policy ---------------------------------------------------------------
ev = baseline()
rc, decision, err = decide("all_required_evidence_allows", ev)
verdict("all_required_evidence_allows",
        rc == 0 and decision["decision"] == "allow" and not decision["reasons"]
        and decision["required_jobs"] >= 30,
        "exit 0, no reasons, >= 30 required jobs",
        f"exit {rc}, {len(decision['reasons']) if decision else '?'} reasons, "
        f"{decision['required_jobs'] if decision else '?'} jobs")
verdict("unavailable_hardware_is_a_scoped_exclusion_not_a_pass",
        decision is not None
        and any(e["id"] == "device-smoke" and e["rationale"] for e in decision["excluded_lanes"]),
        "device-smoke excluded with a rationale",
        [e["id"] for e in (decision or {}).get("excluded_lanes", [])])

ev = baseline()
for r in ev["runs"]:
    r["head_sha"] = OTHER
refuses("green_result_at_a_different_sha", ev, ["only_other_sha_runs"])

ev = baseline()
ev["runs"] = []
refuses("no_run_at_the_tagged_sha", ev, ["no_run_at_sha"])

ev = baseline()
ev["sha"] = OTHER
refuses("evidence_collected_for_another_sha", ev, ["evidence_for_other_sha"])

ev = baseline()
old = copy.deepcopy(run_of(ev, "ci"))
new = run_of(ev, "ci")
new["conclusion"] = "failure"
new["id"] = 777
new["run_attempt"] = 2
new["created_at"] = "2026-10-13T01:00:00Z"
old["id"] = 555
old["created_at"] = "2026-10-12T23:00:00Z"
ev["runs"].append(old)
refuses("only_an_old_successful_run_behind_a_failed_rerun", ev, ["run_failure"])

ev = baseline()
run_of(ev, "ci")["event"] = "pull_request"
refuses("run_from_a_disallowed_event", ev, ["event_not_allowed"])

ev = baseline()
run_of(ev, "ci")["status"] = "in_progress"
run_of(ev, "ci")["conclusion"] = None
refuses("run_still_in_progress", ev, ["run_not_completed"])

ev = baseline()
job_of(run_of(ev, "ci"), "e2e scenario suite")["conclusion"] = "failure"
refuses("required_job_failed", ev, ["job_failure", "aggregate_inconsistent"])

ev = baseline()
job_of(run_of(ev, "ci"), "profile parity")["conclusion"] = "skipped"
refuses("required_job_unexpectedly_skipped", ev, ["job_skipped"])

ev = baseline()
job_of(run_of(ev, "ci"), "embedded matrix")["conclusion"] = "cancelled"
refuses("required_job_cancelled", ev, ["job_cancelled"])

ev = baseline()
run_of(ev, "ci")["jobs"] = [j for j in run_of(ev, "ci")["jobs"] if j["name"] != "MSVC (WIN32)"]
refuses("required_matrix_job_missing", ev, ["job_missing"])

# The CORE SLO fails; HFT and size are skipped behind it.
ev = baseline()
perf = run_of(ev, "perf")
perf["conclusion"] = "failure"
pjob = perf["jobs"][0]
pjob["conclusion"] = "failure"
for step in pjob["steps"]:
    step["conclusion"] = {"Evaluate SLO gates": "failure"}.get(step["name"], "skipped")
refuses("core_failure_hiding_hft_and_size", ev,
        ["run_failure", "job_failure", "step_not_success"])

# A green dashboard cannot stand in for a red SLO step.
ev = baseline()
for step in run_of(ev, "perf")["jobs"][0]["steps"]:
    if step["name"] == "Evaluate HFT SLO gates":
        step["conclusion"] = "failure"
refuses("green_aggregate_cannot_override_a_red_slo_step", ev, ["step_not_success"])

ev = baseline()
del ev["reports"]["conformance_summary"]
refuses("missing_conformance_report", ev, ["report_missing"])

ev = baseline()
ev["reports"]["conformance_summary"]["total"] = 0
refuses("zero_comparison_report", ev, ["zero_comparison"])

ev = baseline()
ev["reports"]["conformance_summary"]["fail"] = 3
refuses("report_with_failures", ev, ["report_check_failed"])

ev = baseline()
refuses("profile_without_matrix_coverage", ev, ["profile_not_covered"], profile="HFT")

ev = baseline()
ev["schema"] = "asx.something_else.v1"
refuses("evidence_of_another_schema", ev, ["evidence_schema"])

ev = baseline()
refuses("tag_that_is_not_a_release", ev, ["tag_not_release"], tag="nightly-2026-10-13")

# An explicit, scoped optional exclusion is allowed, and visible.
ev = baseline()
custom = copy.deepcopy(manifest)
for wf in custom["workflows"]:
    if wf["id"] == "ci":
        wf["optional_jobs"] = [{"job": "msvc", "rationale": "control case: lane excluded on purpose"}]
ci_run = run_of(ev, "ci")
ci_run["jobs"] = [j for j in ci_run["jobs"] if not j["name"].startswith("MSVC")]
custom_path = os.path.join(work, "manifest-optional.json")
with open(custom_path, "w", encoding="utf-8") as stream:
    json.dump(custom, stream)
rc, decision, err = decide("explicit_optional_exclusion_is_allowed_and_listed", ev,
                           manifest_file=custom_path)
verdict("explicit_optional_exclusion_is_allowed_and_listed",
        rc == 0 and decision is not None
        and any(o["job"] == "MSVC (CORE)" and "control case" in o["rationale"]
                for o in decision["optional_jobs"]),
        "exit 0 and the optional jobs listed with their rationale",
        f"exit {rc}, optional {[o['job'] for o in (decision or {}).get('optional_jobs', [])]}")

# An optional lane without a rationale is not accepted at all.
bad = copy.deepcopy(manifest)
bad["workflows"][0]["optional_jobs"] = [{"job": "msvc", "rationale": " "}]
bad_path = os.path.join(work, "manifest-no-rationale.json")
with open(bad_path, "w", encoding="utf-8") as stream:
    json.dump(bad, stream)
rc, decision, err = decide("optional_exclusion_without_a_rationale_is_rejected", baseline(),
                           manifest_file=bad_path)
verdict("optional_exclusion_without_a_rationale_is_rejected",
        rc == 2 and decision is None and "rationale" in err,
        "exit 2 naming the missing rationale", f"exit {rc}: {err.strip()[:80]}")

# An optional lane the workflow does not have is a manifest error, not a pass.
bad = copy.deepcopy(manifest)
bad["workflows"][0]["optional_jobs"] = [{"job": "no-such-job", "rationale": "typo"}]
bad_path = os.path.join(work, "manifest-unknown-job.json")
with open(bad_path, "w", encoding="utf-8") as stream:
    json.dump(bad, stream)
rc, decision, err = decide("optional_exclusion_of_an_unknown_job_is_rejected", baseline(),
                           manifest_file=bad_path)
verdict("optional_exclusion_of_an_unknown_job_is_rejected",
        rc == 2 and decision is None, "exit 2", f"exit {rc}: {err.strip()[:80]}")

# --- B. workflow contract ----------------------------------------------------
with open(os.path.join(repo, ".github", "workflows", "release.yml"), encoding="utf-8") as stream:
    release = yaml.safe_load(stream)
jobs = release["jobs"]
publish = jobs["publish"]
verdict("publish_needs_the_gate_and_the_build",
        set(publish.get("needs", [])) >= {"gate", "build"},
        "needs gate and build", publish.get("needs"))
cond = str(publish.get("if", ""))
verdict("publish_requires_an_allow_decision_and_no_dry_run",
        "needs.gate.outputs.decision == 'allow'" in cond
        and "needs.gate.outputs.dry_run != 'true'" in cond and "success()" in cond
        and "always()" not in cond,
        "if: success() && decision == allow && not dry run", cond)
uploaders = [name for name, job in jobs.items()
             if any("action-gh-release" in str(s.get("uses", "")) for s in job.get("steps", []))]
verdict("only_publish_uploads_release_assets", uploaders == ["publish"],
        "['publish']", uploaders)
writers = [name for name, job in jobs.items()
           if (job.get("permissions") or {}).get("contents") == "write"]
verdict("only_publish_can_write_contents", writers == ["publish"], "['publish']", writers)
publish_steps_always = [s.get("name") for s in publish.get("steps", [])
                        if "always()" in str(s.get("if", ""))]
verdict("no_publish_step_runs_regardless_of_failure", not publish_steps_always,
        "no always() steps", publish_steps_always)
build_text = json.dumps(jobs["build"])
verdict("build_requires_a_verified_signature",
        "--require-signature" in build_text and "run_release_install_smoke.sh" in build_text,
        "verify step with --require-signature", "present" if "--require-signature" in build_text
        else "absent")
decide_step = next(s for s in jobs["gate"]["steps"] if s.get("id") == "decide")
verdict("a_refusal_fails_the_gate_on_a_tag",
        str(decide_step.get("continue-on-error")).replace(" ", "")
        == "${{github.event_name=='workflow_dispatch'}}",
        "continue-on-error only for workflow_dispatch", decide_step.get("continue-on-error"))
verdict("release_runs_on_tags_and_dry_runs_on_dispatch",
        set(release[True]) == {"push", "workflow_dispatch"} if True in release
        else set(release["on"]) == {"push", "workflow_dispatch"},
        "push (tags) and workflow_dispatch", sorted((release.get(True) or release.get("on")).keys()))

# The manifest still describes the workflow files.
for wf in manifest["workflows"]:
    with open(os.path.join(repo, wf["path"]), encoding="utf-8") as stream:
        doc = yaml.safe_load(stream)
    for job_id, steps in (wf.get("required_steps") or {}).items():
        have = [s.get("name") for s in doc["jobs"].get(job_id, {}).get("steps", [])]
        missing = [s for s in steps if s not in have]
        verdict(f"manifest_steps_exist_in_{wf['id']}_{job_id}", not missing,
                "every required step exists", missing or "all present")
default_profile = manifest.get("default_profile")
for spec in manifest.get("profile_jobs", []):
    pattern = re.compile(spec["pattern"].replace("{profile}", re.escape(default_profile)))
    hits = [r["name"] for r in expected[spec["workflow"]] if pattern.search(r["name"])]
    verdict("manifest_profile_pattern_matches_the_matrix", len(hits) >= int(spec.get("min", 1)),
            f">= {spec.get('min', 1)} jobs for {default_profile}", len(hits))

sys.exit(1 if failures else 0)
PY
    rc=$?
    if [ "$rc" -ne 0 ]; then
        failures=$((failures + 1))
    fi
fi

# ---------------------------------------------------------------------------
# C. artifact controls on synthetic archives
# ---------------------------------------------------------------------------
if ! command -v jq >/dev/null 2>&1 || ! command -v xz >/dev/null 2>&1 \
        || ! command -v sha256sum >/dev/null 2>&1; then
    verdict "artifact_control_tools_available" "jq xz sha256sum" "missing" \
        "the artifact controls need jq, xz and sha256sum"
else
    FAKE_SHA="1111111111111111111111111111111111111111"
    WRONG_SHA="2222222222222222222222222222222222222222"
    CANON="$WORK/canonical"
    mkdir -p "$CANON"

    # make_archive <target> <kind: binary|source>: a package tree, archived
    # with its checksum, provenance and the unsigned stub bundle.
    make_archive() {
        local target="$1" kind="$2" top="asx-$1" tree
        tree="$WORK/tree-$target"
        mkdir -p "$tree/$top/include/asx" "$tree/$top/src"
        printf '/* header */\n' >"$tree/$top/include/asx/asx.h"
        printf 'readme\n' >"$tree/$top/README.md"
        if [ "$kind" = "binary" ]; then
            mkdir -p "$tree/$top/lib"
            printf 'not a real archive\n' >"$tree/$top/lib/libasx.a"
        else
            printf 'all:\n' >"$tree/$top/Makefile"
            printf 'int x;\n' >"$tree/$top/src/x.c"
        fi
        tar -C "$tree" -cJf "$CANON/$top.tar.xz" "$top"
        (cd "$CANON" && sha256sum "$top.tar.xz" >"$top.tar.xz.sha256")
        local sum
        sum="$(awk '{print $1}' "$CANON/$top.tar.xz.sha256")"
        jq -n --arg artifact "$top.tar.xz" --arg target "$target" --arg kind "$kind" \
            --arg sha "$sum" --arg git "$FAKE_SHA" '{
              schema: "asx.release.artifact.v1", artifact: $artifact, artifact_kind: $kind,
              target: $target, version: "1.2.3", release_tag: "v1.2.3",
              git: {sha: $git, ref: "main"},
              integrity: {sha256: $sha, sha256_file: ($artifact + ".sha256"),
                          sigstore_bundle: ($artifact + ".sigstore.json"),
                          sigstore_requested: false}}' >"$CANON/$top.provenance.json"
        printf '{"schema":"asx.sigstore.bundle.stub.v1","status":"skipped"}\n' \
            >"$CANON/$top.tar.xz.sigstore.json"
    }

    make_archive source source
    make_archive linux-x86_64 binary

    # case_copy <name>: an isolated copy of the canonical files to corrupt.
    case_copy() {
        mkdir -p "$WORK/case-$1"
        cp "$CANON"/* "$WORK/case-$1/"
        printf '%s' "$WORK/case-$1"
    }

    # smoke <case dir> <target> <kind> [extra args]: run the verifier; print
    # the name of the first failing step ("pass" when the run passed).
    smoke() {
        local dir="$1" target="$2" kind="$3"
        shift 3
        local report_dir="$dir/staging" out rc step
        out="$(bash "$SMOKE" --artifact "$dir/asx-$target.tar.xz" --version 1.2.3 \
            --target "$target" --kind "$kind" --expect-sha "$FAKE_SHA" \
            --report-dir "$report_dir" "$@" 2>&1)"
        rc=$?
        if [ "$rc" -eq 0 ]; then
            printf 'pass'
            return
        fi
        step="$(jq -rs '[.[]?|select(.status=="fail")|.name][0] // "unknown"' \
            "$report_dir"/*/steps.jsonl 2>/dev/null || true)"
        printf '%s' "${step:-unknown}"
    }

    SIG_ARGS=(--require-signature --cosign-identity-regexp '^https://github.com/o/r/.*$')

    d="$(case_copy clean-source)"
    verdict "clean_source_archive_passes" "pass" "$(smoke "$d" source source)"

    d="$(case_copy source-signature-required-but-stub)"
    verdict "unsigned_stub_bundle_is_rejected_when_a_signature_is_required" \
        "signature_is_not_stub" "$(smoke "$d" source source "${SIG_ARGS[@]}")"

    d="$(case_copy corrupted-archive)"
    printf 'tampered' >>"$d/asx-source.tar.xz"
    verdict "corrupted_archive_is_rejected" "checksum_matches" "$(smoke "$d" source source)"

    d="$(case_copy corrupted-checksum-file)"
    printf '%s  %s\n' "0000000000000000000000000000000000000000000000000000000000000000" \
        "asx-source.tar.xz" >"$d/asx-source.tar.xz.sha256"
    verdict "corrupted_checksum_file_is_rejected" "checksum_matches" "$(smoke "$d" source source)"

    d="$(case_copy provenance-checksum)"
    jq '.integrity.sha256 = "deadbeef"' "$CANON/asx-source.provenance.json" \
        >"$d/asx-source.provenance.json"
    verdict "provenance_with_another_checksum_is_rejected" \
        "provenance_checksum_matches" "$(smoke "$d" source source)"

    d="$(case_copy provenance-commit)"
    jq --arg sha "$WRONG_SHA" '.git.sha = $sha' "$CANON/asx-source.provenance.json" \
        >"$d/asx-source.provenance.json"
    verdict "provenance_naming_another_commit_is_rejected" \
        "provenance_commit_matches" "$(smoke "$d" source source)"

    d="$(case_copy provenance-version)"
    jq '.version = "9.9.9"' "$CANON/asx-source.provenance.json" >"$d/asx-source.provenance.json"
    verdict "provenance_of_another_version_is_rejected" \
        "provenance_release_matches" "$(smoke "$d" source source)"

    d="$(case_copy missing-provenance)"
    rm_target="$d/asx-source.provenance.json"
    mv "$rm_target" "$rm_target.moved"
    verdict "missing_provenance_is_rejected" "provenance_file_exists" "$(smoke "$d" source source)"

    d="$(case_copy missing-bundle)"
    mv "$d/asx-source.tar.xz.sigstore.json" "$d/asx-source.tar.xz.sigstore.json.moved"
    verdict "missing_signature_bundle_is_rejected" "sigstore_file_exists" \
        "$(smoke "$d" source source "${SIG_ARGS[@]}")"

    # A bundle that is not the stub, with no verifier on PATH: a failure,
    # never a skip. PATH holds only what the script needs (no cosign).
    d="$(case_copy missing-verifier)"
    printf '{"mediaType":"application/vnd.dev.sigstore.bundle+json"}\n' \
        >"$d/asx-source.tar.xz.sigstore.json"
    NOCOSIGN="$WORK/path-without-cosign"
    mkdir -p "$NOCOSIGN"
    for tool in bash awk tar xz jq git gcc cc install cp sha256sum cat date mkdir dirname \
            basename head tr sort printf env sed grep tee mv ls id; do
        src="$(command -v "$tool" 2>/dev/null || true)"
        if [ -n "$src" ] && [ ! -e "$NOCOSIGN/$tool" ]; then ln -s "$src" "$NOCOSIGN/$tool"; fi
    done
    step="$(PATH="$NOCOSIGN" smoke "$d" source source "${SIG_ARGS[@]}")"
    verdict "missing_cosign_fails_when_a_signature_is_required" \
        "command_available_cosign" "$step"

    d="$(case_copy binary-corrupted)"
    printf 'tampered' >>"$d/asx-linux-x86_64.tar.xz"
    verdict "corrupted_binary_archive_is_rejected" "checksum_matches" \
        "$(smoke "$d" linux-x86_64 binary)"

    d="$(case_copy binary-wrong-kind-payload)"
    verdict "source_archive_verified_as_binary_has_no_library" "package_library_present" \
        "$(smoke "$d" source binary)"

    # The canonical files are untouched by every control above.
    (cd "$CANON" && sha256sum -c asx-source.tar.xz.sha256 asx-linux-x86_64.tar.xz.sha256 \
        >/dev/null 2>&1)
    verdict "canonical_artifacts_were_preserved" "0" "$?"
fi

# ---------------------------------------------------------------------------
# D. a real candidate (builds the library; off by default)
# ---------------------------------------------------------------------------
if [ "${ASX_RELEASE_E2E:-0}" = "1" ]; then
    E2E="$WORK/e2e"
    mkdir -p "$E2E/dist" "$E2E/manifests"
    HEAD_SHA="$(git -C "$REPO_ROOT" rev-parse HEAD)"
    if (cd "$REPO_ROOT" && ASX_USE_RCH="${ASX_USE_RCH:-0}" bash tools/ci/run_release_artifacts.sh \
            --version 0.0.0 --target linux-x86_64 --kind binary --profile CORE --codec BIN \
            --deterministic 1 --out-dir "$E2E/dist" --manifest-dir "$E2E/manifests" \
            >"$E2E/build.log" 2>&1); then
        verdict "real_candidate_builds" "0" "0"
        REAL="$E2E/dist"
        out="$(bash "$SMOKE" --artifact "$REAL/asx-linux-x86_64.tar.xz" --version 0.0.0 \
            --target linux-x86_64 --expect-sha "$HEAD_SHA" --report-dir "$E2E/staging" 2>&1)"
        verdict "real_candidate_installs_and_runs_consumer_and_doctor" "0" "$?" "$out"
        jq -e '[.steps[].name] | (index("quickstart_run") != null) and (index("doctor_run") != null)' \
            "$E2E"/staging/*/release_install_smoke_report.json >/dev/null 2>&1
        verdict "real_candidate_report_records_consumer_and_doctor_steps" "0" "$?"

        for variant in archive checksum provenance; do
            mkdir -p "$E2E/corrupt-$variant"
            cp "$REAL"/* "$E2E/corrupt-$variant/"
            case "$variant" in
                archive) printf 'tampered' >>"$E2E/corrupt-$variant/asx-linux-x86_64.tar.xz" ;;
                checksum) printf '%s  %s\n' \
                    "0000000000000000000000000000000000000000000000000000000000000000" \
                    "asx-linux-x86_64.tar.xz" \
                    >"$E2E/corrupt-$variant/asx-linux-x86_64.tar.xz.sha256" ;;
                provenance) jq '.integrity.sha256 = "deadbeef"' \
                    "$REAL/asx-linux-x86_64.provenance.json" \
                    >"$E2E/corrupt-$variant/asx-linux-x86_64.provenance.json" ;;
            esac
            bash "$SMOKE" --artifact "$E2E/corrupt-$variant/asx-linux-x86_64.tar.xz" \
                --version 0.0.0 --target linux-x86_64 --expect-sha "$HEAD_SHA" \
                --report-dir "$E2E/corrupt-$variant/staging" >/dev/null 2>&1
            rc=$?
            if [ "$rc" -ne 0 ]; then rc=1; fi
            verdict "real_candidate_corrupted_${variant}_is_rejected" "1" "$rc"
        done
    else
        verdict "real_candidate_builds" "0" "1" "see $E2E/build.log"
    fi
fi

echo ""
if [ "$failures" -eq 0 ]; then
    echo "test_release_gate: PASS"
    exit 0
fi
echo "test_release_gate: FAIL ($failures case(s)); work $WORK"
exit 1
