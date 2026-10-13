#!/usr/bin/env python3
"""release_gate.py - the release readiness decision (bd-9kll.18.5).

A release is allowed only when every required lane of every required
workflow succeeded at the exact commit the tag resolves to. This tool turns
that rule into a function over collected evidence, so the decision is
deterministic, reviewable and testable without publishing anything.

  release_gate.py expected --manifest M --workflows-dir D
      Print the job names the manifest requires, expanded from the workflow
      files (matrix included): what a release will wait for.

  release_gate.py collect --manifest M --repo OWNER/NAME --sha SHA --out E
      Fetch the workflow runs, their jobs and the report artifacts of SHA
      with the GitHub CLI (`gh`) into an evidence file.

  release_gate.py decide --manifest M --workflows-dir D --evidence E
                         --tag TAG --sha SHA [--profile P] [--output F]
      Allow or refuse. Exit 0 = allow, 1 = refuse (structured reasons in
      the decision JSON), 2 = usage or malformed input. Refusal is the
      default: anything the evidence does not positively show is a reason.

The rules, per required workflow of the manifest:
  - a completed run of the workflow exists whose head_sha is the tagged
    commit, for an allowed event; a green run of another commit is stale;
  - the latest such run succeeded (a later failed or cancelled attempt is
    not hidden by an older green one);
  - every job the workflow file defines at that commit (matrix expanded)
    ran and succeeded, unless the manifest lists it as optional with a
    rationale: a skipped, cancelled, missing or failed required job refuses;
  - a job the manifest gives required_steps must show each of them
    succeeded (a failing earlier gate that skipped a later one is visible);
  - the run's aggregate conclusion must agree with its jobs;
  - the required report artifacts exist and meet their checks (a report
    with zero comparisons or a failing count refuses).

SPDX-License-Identifier: MIT
"""

import argparse
import itertools
import json
import os
import re
import subprocess
import sys

try:
    import yaml
except ImportError:  # strict: a missing parser is a failure, never a pass
    yaml = None

DECISION_SCHEMA = "asx.release_decision.v1"
MANIFEST_SCHEMA = "asx.release_required_gates.v1"
EVIDENCE_SCHEMA = "asx.release_evidence.v1"

OPS = {
    "==": lambda a, b: a == b,
    "!=": lambda a, b: a != b,
    ">=": lambda a, b: a >= b,
    ">": lambda a, b: a > b,
    "<=": lambda a, b: a <= b,
    "<": lambda a, b: a < b,
}


class Usage(Exception):
    """Malformed input: the caller's mistake, exit status 2."""


def reason(code, detail, **fields):
    row = {"code": code, "detail": detail}
    row.update({k: v for k, v in fields.items() if v is not None})
    return row


# ---------------------------------------------------------------------------
# Manifest and workflow files
# ---------------------------------------------------------------------------

def load_json(path):
    try:
        with open(path, encoding="utf-8") as stream:
            return json.load(stream)
    except (OSError, ValueError) as exc:
        raise Usage(f"cannot read {path}: {exc}")


def load_manifest(path):
    manifest = load_json(path)
    if manifest.get("schema") != MANIFEST_SCHEMA:
        raise Usage(f"{path}: schema is not {MANIFEST_SCHEMA}")
    for wf in manifest.get("workflows", []):
        for key in ("id", "name", "path", "events"):
            if key not in wf:
                raise Usage(f"{path}: workflow entry lacks {key!r}")
        for opt in wf.get("optional_jobs", []):
            if not opt.get("rationale", "").strip():
                raise Usage(f"{path}: optional job {opt.get('job')!r} has no rationale")
    for exc in manifest.get("excluded", []):
        if not exc.get("rationale", "").strip() or not exc.get("scope", "").strip():
            raise Usage(f"{path}: excluded lane {exc.get('id')!r} needs a scope and a rationale")
    return manifest


def load_workflow(workflows_dir, wf_path):
    if yaml is None:
        raise Usage("PyYAML is required to read the workflow files")
    path = os.path.join(workflows_dir, wf_path)
    try:
        with open(path, encoding="utf-8") as stream:
            doc = yaml.safe_load(stream)
    except (OSError, yaml.YAMLError) as exc:
        raise Usage(f"cannot read workflow {path}: {exc}")
    if not isinstance(doc, dict) or not isinstance(doc.get("jobs"), dict):
        raise Usage(f"{path}: no jobs")
    return doc


def matrix_combos(matrix):
    """The job combinations of a strategy.matrix, GitHub's way for the forms
    this repository uses: lists cross-multiplied, include entries merged into
    a combination they do not contradict or appended as a new one, exclude
    entries removing matching combinations."""
    if not isinstance(matrix, dict):
        return [{}]
    include = matrix.get("include") or []
    exclude = matrix.get("exclude") or []
    axes = {k: v for k, v in matrix.items() if k not in ("include", "exclude")}
    for key, values in axes.items():
        if not isinstance(values, list):
            raise Usage(f"matrix axis {key!r} is not a list (expressions are not supported)")
    if axes:
        keys = list(axes)
        combos = [dict(zip(keys, vals)) for vals in itertools.product(*(axes[k] for k in keys))]
    else:
        combos = []
    combos = [c for c in combos if not any(all(c.get(k) == v for k, v in ex.items()) for ex in exclude)]
    original = set(axes)
    # An include entry extends the combinations it does not contradict;
    # when it fits none (or there are no axes at all) it is a combination
    # of its own. Entries never merge into one another.
    base = list(combos)
    for inc in include:
        merged = False
        for combo in base:
            if all(combo.get(k) == v for k, v in inc.items() if k in original):
                combo.update({k: v for k, v in inc.items() if k not in original})
                merged = True
        if not merged:
            combos.append(dict(inc))
    return combos or [{}]


def job_names(job_id, job):
    """The display names a job produces: its name template per matrix
    combination, or GitHub's default of the id plus the matrix values."""
    strategy = job.get("strategy") or {}
    matrix = strategy.get("matrix") if isinstance(strategy, dict) else None
    combos = matrix_combos(matrix) if matrix is not None else [{}]
    template = job.get("name")
    names = []
    for combo in combos:
        if template is None:
            if combo:
                names.append(f"{job_id} ({', '.join(str(v) for v in combo.values())})")
            else:
                names.append(job_id)
            continue
        text = str(template)
        for key, value in combo.items():
            text = re.sub(r"\$\{\{\s*matrix\." + re.escape(key) + r"\s*\}\}", str(value), text)
        names.append(text)
    return names


def expected_jobs(manifest, workflows_dir):
    """{workflow id: [{job_id, name, optional: rationale | None}]}"""
    out = {}
    for wf in manifest["workflows"]:
        doc = load_workflow(workflows_dir, wf["path"])
        optional = {o["job"]: o["rationale"] for o in wf.get("optional_jobs", [])}
        unknown = sorted(set(optional) - set(doc["jobs"]))
        if unknown:
            raise Usage(f"{wf['path']}: optional job(s) not in the workflow: {', '.join(unknown)}")
        rows = []
        for job_id, job in doc["jobs"].items():
            for name in job_names(job_id, job):
                rows.append({"job_id": job_id, "name": name, "optional": optional.get(job_id)})
        out[wf["id"]] = rows
    return out


# ---------------------------------------------------------------------------
# The decision
# ---------------------------------------------------------------------------

def run_sort_key(run):
    return (run.get("created_at") or "", int(run.get("run_attempt") or 1), int(run.get("id") or 0))


def get_path(obj, path):
    cur = obj
    for part in path.split("."):
        if isinstance(cur, dict) and part in cur:
            cur = cur[part]
        else:
            raise KeyError(path)
    return cur


def check_workflow(wf, expected, evidence, sha):
    reasons = []
    same_workflow = [r for r in evidence.get("runs", [])
                     if r.get("path") == wf["path"] or r.get("workflow") == wf["name"]]
    here = [r for r in same_workflow if r.get("head_sha") == sha]
    at_sha = [r for r in here if r.get("event") in wf["events"]]
    if not at_sha:
        other = sorted({r.get("head_sha", "?")[:12] for r in same_workflow})
        if here:
            reasons.append(reason(
                "event_not_allowed",
                f"{wf['name']} ran for {sha[:12]} only for events "
                f"{sorted({r.get('event') for r in here})}, the release needs {wf['events']}",
                workflow=wf["id"]))
        elif other:
            reasons.append(reason(
                "only_other_sha_runs",
                f"{wf['name']} has runs for {', '.join(other)} but none for {sha[:12]}",
                workflow=wf["id"]))
        else:
            reasons.append(reason(
                "no_run_at_sha", f"no {wf['name']} run for {sha[:12]} (events: {wf['events']})",
                workflow=wf["id"]))
        return reasons, None

    latest = max(at_sha, key=run_sort_key)
    if latest.get("status") != "completed":
        reasons.append(reason(
            "run_not_completed", f"{wf['name']} run {latest.get('id')} is {latest.get('status')}",
            workflow=wf["id"], run_id=latest.get("id")))
        return reasons, latest
    conclusion = latest.get("conclusion")
    if conclusion != "success":
        older = [r for r in at_sha if r is not latest and r.get("conclusion") == "success"]
        reasons.append(reason(
            f"run_{conclusion or 'unknown'}",
            f"the latest {wf['name']} run {latest.get('id')} at {sha[:12]} concluded "
            f"{conclusion}" + (f"; an older run {older[0].get('id')} succeeded and does not count"
                               if older else ""),
            workflow=wf["id"], run_id=latest.get("id")))

    jobs = latest.get("jobs") or []
    by_name = {}
    for job in jobs:
        by_name.setdefault(job.get("name"), []).append(job)

    for row in expected:
        matches = by_name.get(row["name"], [])
        if not matches:
            if row["optional"]:
                continue
            reasons.append(reason(
                "job_missing", f"required job {row['name']!r} did not run",
                workflow=wf["id"], job=row["name"], run_id=latest.get("id")))
            continue
        for job in matches:
            if row["optional"]:
                continue
            if job.get("status") != "completed":
                reasons.append(reason(
                    "job_not_completed", f"job {row['name']!r} is {job.get('status')}",
                    workflow=wf["id"], job=row["name"], run_id=latest.get("id")))
            elif job.get("conclusion") != "success":
                reasons.append(reason(
                    f"job_{job.get('conclusion') or 'unknown'}",
                    f"required job {row['name']!r} concluded {job.get('conclusion')}",
                    workflow=wf["id"], job=row["name"], run_id=latest.get("id")))

    # The run's aggregate must agree with what its jobs say.
    if conclusion == "success":
        red = [j for j in jobs if j.get("conclusion") in ("failure", "cancelled", "timed_out")]
        if red:
            reasons.append(reason(
                "aggregate_inconsistent",
                f"{wf['name']} run {latest.get('id')} reports success but job "
                f"{red[0].get('name')!r} is {red[0].get('conclusion')}",
                workflow=wf["id"], run_id=latest.get("id")))

    # Steps a job must have run: a failed earlier gate that skipped a later
    # one (the CORE SLO failing and hiding HFT or size) is not a pass.
    for job_id, steps in (wf.get("required_steps") or {}).items():
        names = [r["name"] for r in expected if r["job_id"] == job_id]
        for name in names:
            for job in by_name.get(name, []):
                have = {s.get("name"): s.get("conclusion") for s in job.get("steps") or []}
                for step in steps:
                    if have.get(step) != "success":
                        reasons.append(reason(
                            "step_not_success",
                            f"step {step!r} of {name!r} is {have.get(step, 'absent')}",
                            workflow=wf["id"], job=name, step=step, run_id=latest.get("id")))
    return reasons, latest


def check_profile(manifest, profile, evidence_by_wf):
    """The matrix lanes of the profile being released: at least the minimum
    number ran and succeeded."""
    reasons = []
    for spec in manifest.get("profile_jobs", []):
        run = evidence_by_wf.get(spec["workflow"])
        if run is None:
            continue  # the workflow's own refusal already says why
        pattern = re.compile(spec["pattern"].replace("{profile}", re.escape(profile)))
        hits = [j for j in run.get("jobs") or [] if pattern.search(j.get("name") or "")]
        if len(hits) < int(spec.get("min", 1)):
            reasons.append(reason(
                "profile_not_covered",
                f"profile {profile}: {len(hits)} job(s) match {pattern.pattern!r}, "
                f"{spec.get('min', 1)} required",
                workflow=spec["workflow"]))
            continue
        for job in hits:
            if job.get("conclusion") != "success":
                reasons.append(reason(
                    "profile_job_not_success",
                    f"profile {profile}: {job.get('name')!r} is {job.get('conclusion')}",
                    workflow=spec["workflow"], job=job.get("name")))
    return reasons


def check_reports(manifest, evidence):
    reasons = []
    reports = evidence.get("reports") or {}
    for spec in manifest.get("reports", []):
        report = reports.get(spec["id"])
        if report is None:
            reasons.append(reason(
                "report_missing", f"required report {spec['id']!r} is not in the evidence",
                report=spec["id"]))
            continue
        for check in spec.get("require", []):
            try:
                value = get_path(report, check["path"])
            except KeyError:
                reasons.append(reason(
                    "report_check_failed",
                    f"report {spec['id']!r} has no field {check['path']!r}", report=spec["id"]))
                continue
            op = OPS.get(check["op"])
            if op is None:
                raise Usage(f"unknown operator {check['op']!r} for report {spec['id']!r}")
            if not op(value, check["value"]):
                code = "zero_comparison" if (check["op"] in (">=", ">") and value == 0) \
                    else "report_check_failed"
                reasons.append(reason(
                    code,
                    f"report {spec['id']!r}: {check['path']} = {value!r}, "
                    f"required {check['op']} {check['value']!r}", report=spec["id"]))
    return reasons


def decide(manifest, workflows_dir, evidence, tag, sha, profile=None):
    """The decision document for a release of `tag`, resolved to `sha`."""
    reasons = []
    if evidence.get("schema") != EVIDENCE_SCHEMA:
        reasons.append(reason("evidence_schema", f"evidence schema is not {EVIDENCE_SCHEMA}"))
    pattern = manifest.get("tag_pattern")
    if pattern and not re.match(pattern, tag or ""):
        reasons.append(reason("tag_not_release", f"tag {tag!r} does not match {pattern}"))
    if not re.fullmatch(r"[0-9a-f]{40}", sha or ""):
        reasons.append(reason("sha_malformed", f"{sha!r} is not a full commit id"))
    if evidence.get("sha") and evidence.get("sha") != sha:
        reasons.append(reason(
            "evidence_for_other_sha",
            f"evidence was collected for {evidence.get('sha')}, the tag resolves to {sha}"))

    expected = expected_jobs(manifest, workflows_dir)
    runs = {}
    for wf in manifest["workflows"]:
        wf_reasons, latest = check_workflow(wf, expected[wf["id"]], evidence, sha)
        reasons.extend(wf_reasons)
        runs[wf["id"]] = latest
    release_profile = profile or manifest.get("default_profile")
    if release_profile:
        reasons.extend(check_profile(manifest, release_profile, runs))
    reasons.extend(check_reports(manifest, evidence))

    required = sum(1 for rows in expected.values() for r in rows if not r["optional"])
    optional = [{"workflow": wf_id, "job": r["name"], "rationale": r["optional"]}
                for wf_id, rows in expected.items() for r in rows if r["optional"]]
    return {
        "schema": DECISION_SCHEMA,
        "decision": "refuse" if reasons else "allow",
        "tag": tag,
        "sha": sha,
        "profile": release_profile,
        "required_jobs": required,
        "optional_jobs": optional,
        "excluded_lanes": manifest.get("excluded", []),
        "runs": {k: (v.get("id") if v else None) for k, v in runs.items()},
        "reasons": reasons,
    }


# ---------------------------------------------------------------------------
# Evidence collection (GitHub CLI)
# ---------------------------------------------------------------------------

def gh_json(args):
    """One JSON value, or with --paginate the pages merged: `gh api
    --paginate` prints one JSON document per page, back to back."""
    proc = subprocess.run(["gh", "api", *args], capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        raise Usage(f"gh api {' '.join(args)} failed: {proc.stderr.strip()[:300]}")
    decoder = json.JSONDecoder()
    text = proc.stdout.strip()
    docs = []
    pos = 0
    try:
        while pos < len(text):
            doc, end = decoder.raw_decode(text, pos)
            docs.append(doc)
            pos = end
            while pos < len(text) and text[pos].isspace():
                pos += 1
    except ValueError as exc:
        raise Usage(f"gh api {' '.join(args)}: not JSON: {exc}")
    if len(docs) == 1:
        return docs[0]
    merged = {}
    for doc in docs:
        for key, value in doc.items():
            if isinstance(value, list):
                merged.setdefault(key, []).extend(value)
            else:
                merged.setdefault(key, value)
    return merged


def run_row(raw, jobs):
    return {
        "id": raw.get("id"),
        "workflow": raw.get("name"),
        # The API may report the file as "<path>@<ref>".
        "path": (raw.get("path") or "").split("@")[0],
        "head_sha": raw.get("head_sha"),
        "event": raw.get("event"),
        "status": raw.get("status"),
        "conclusion": raw.get("conclusion"),
        "run_attempt": raw.get("run_attempt"),
        "created_at": raw.get("created_at"),
        "jobs": [{
            "name": j.get("name"),
            "status": j.get("status"),
            "conclusion": j.get("conclusion"),
            "steps": [{"name": s.get("name"), "status": s.get("status"),
                       "conclusion": s.get("conclusion")} for s in j.get("steps") or []],
        } for j in jobs],
    }


def collect(manifest, repo, sha, out_path, report_dir):
    runs = []
    listing = gh_json([f"repos/{repo}/actions/runs?head_sha={sha}&per_page=100"])
    wanted = {wf["path"]: wf for wf in manifest["workflows"]}
    chosen = {}
    for raw in listing.get("workflow_runs", []):
        path = (raw.get("path") or "").split("@")[0]
        if path not in wanted:
            continue
        jobs = gh_json(["--paginate", f"repos/{repo}/actions/runs/{raw['id']}/jobs?per_page=100"])
        job_list = jobs.get("jobs", []) if isinstance(jobs, dict) else jobs
        row = run_row(raw, job_list)
        runs.append(row)
        wf = wanted[path]
        if row["event"] in wf["events"]:
            if wf["id"] not in chosen or run_sort_key(row) > run_sort_key(chosen[wf["id"]]):
                chosen[wf["id"]] = row

    reports = {}
    for spec in manifest.get("reports", []):
        run = chosen.get(spec["workflow"])
        if run is None:
            continue
        arts = gh_json([f"repos/{repo}/actions/runs/{run['id']}/artifacts?per_page=100"])
        names = [a["name"] for a in arts.get("artifacts", [])
                 if a.get("name", "").startswith(spec["artifact_prefix"]) and not a.get("expired")]
        if not names:
            continue
        dest = os.path.join(report_dir, spec["id"])
        os.makedirs(dest, exist_ok=True)
        proc = subprocess.run(["gh", "run", "download", str(run["id"]), "-R", repo,
                               "-n", names[0], "-D", dest],
                              capture_output=True, text=True, check=False)
        if proc.returncode != 0:
            continue
        found = []
        for root, _dirs, files in os.walk(dest):
            for fname in sorted(files):
                if re.fullmatch(spec["glob"].replace(".", r"\.").replace("*", ".*"), fname):
                    found.append(os.path.join(root, fname))
        for path in sorted(found):
            try:
                reports[spec["id"]] = load_json(path)
                break
            except Usage:
                continue

    evidence = {"schema": EVIDENCE_SCHEMA, "repository": repo, "sha": sha,
                "runs": runs, "reports": reports}
    with open(out_path, "w", encoding="utf-8") as stream:
        json.dump(evidence, stream, indent=2, sort_keys=True)
        stream.write("\n")
    return evidence


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def render(decision):
    lines = [f"release gate: {decision['decision'].upper()}  tag={decision['tag']} "
             f"sha={decision['sha']} profile={decision['profile']}",
             f"  required jobs: {decision['required_jobs']}  "
             f"runs: {json.dumps(decision['runs'], sort_keys=True)}"]
    for exc in decision["excluded_lanes"]:
        lines.append(f"  excluded: {exc['id']} ({exc['scope']}): {exc['rationale']}")
    for opt in decision["optional_jobs"]:
        lines.append(f"  optional: {opt['workflow']}/{opt['job']}: {opt['rationale']}")
    for row in decision["reasons"]:
        lines.append(f"  REFUSE {row['code']}: {row['detail']}")
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description="release readiness decision")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_exp = sub.add_parser("expected")
    p_exp.add_argument("--manifest", required=True)
    p_exp.add_argument("--workflows-dir", default=".")

    p_col = sub.add_parser("collect")
    p_col.add_argument("--manifest", required=True)
    p_col.add_argument("--repo", required=True)
    p_col.add_argument("--sha", required=True)
    p_col.add_argument("--out", required=True)
    p_col.add_argument("--report-dir", default="release-evidence-reports")

    p_dec = sub.add_parser("decide")
    p_dec.add_argument("--manifest", required=True)
    p_dec.add_argument("--workflows-dir", default=".")
    p_dec.add_argument("--evidence", required=True)
    p_dec.add_argument("--tag", required=True)
    p_dec.add_argument("--sha", required=True)
    p_dec.add_argument("--profile")
    p_dec.add_argument("--output")

    args = parser.parse_args(argv)
    try:
        manifest = load_manifest(args.manifest)
        if args.cmd == "expected":
            for wf_id, rows in expected_jobs(manifest, args.workflows_dir).items():
                for row in rows:
                    flag = f"  (optional: {row['optional']})" if row["optional"] else ""
                    print(f"{wf_id}\t{row['job_id']}\t{row['name']}{flag}")
            return 0
        if args.cmd == "collect":
            collect(manifest, args.repo, args.sha, args.out, args.report_dir)
            print(f"release gate: evidence for {args.sha} written to {args.out}")
            return 0
        evidence = load_json(args.evidence)
        decision = decide(manifest, args.workflows_dir, evidence, args.tag, args.sha, args.profile)
        if args.output:
            with open(args.output, "w", encoding="utf-8") as stream:
                json.dump(decision, stream, indent=2, sort_keys=True)
                stream.write("\n")
        print(render(decision))
        return 0 if decision["decision"] == "allow" else 1
    except Usage as exc:
        print(f"release_gate: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
