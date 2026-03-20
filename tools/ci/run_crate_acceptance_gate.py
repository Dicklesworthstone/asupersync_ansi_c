#!/usr/bin/env python3
"""
run_crate_acceptance_gate.py — crate-level parity acceptance gate

Aggregates crate-level parity evidence into one auditable decision artifact.
The gate is intentionally strict: it fails when reopened families do not have
named documentation anchors, unit-test surfaces, end-to-end smoke surfaces, or
required retained artifacts from the shared conformance/profile/e2e lanes.

Outputs:
  - build/acceptance/crate_acceptance_<run_id>.json
  - tools/ci/artifacts/acceptance/crate_acceptance_<run_id>.summary.json

Exit codes:
  0: gate passes
  1: gate fails
  2: usage/config error

SPDX-License-Identifier: MIT
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


@dataclass(frozen=True)
class FamilySpec:
    family_id: str
    title: str
    unit_globs: tuple[str, ...]
    e2e_globs: tuple[str, ...]
    doc_requirements: tuple[tuple[str, str], ...]


def utc_now() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def latest_matching(root: Path, pattern: str) -> Path | None:
    matches = sorted(root.glob(pattern), key=lambda p: p.stat().st_mtime)
    return matches[-1] if matches else None


def load_json(path: Path) -> Any:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def text_contains(path: Path, needle: str) -> bool:
    return needle in path.read_text(encoding="utf-8")


def first_matches(root: Path, patterns: tuple[str, ...]) -> list[str]:
    matches: list[str] = []
    for pattern in patterns:
        for path in sorted(root.glob(pattern)):
            rel = path.relative_to(root).as_posix()
            if rel not in matches:
                matches.append(rel)
    return matches


def gate_status(ok: bool) -> str:
    return "pass" if ok else "fail"


FAMILY_SPECS: tuple[FamilySpec, ...] = (
    FamilySpec(
        family_id="bd-yx9r.4-6",
        title="Public/core/runtime/capability completion",
        unit_globs=(
            "tests/unit/cx/test_*.c",
            "tests/unit/runtime/test_*.c",
            "tests/unit/security/test_*.c",
            "tests/unit/app/test_*.c",
            "tests/unit/evidence/test_*.c",
        ),
        e2e_globs=(
            "tests/e2e/foundational_contracts.sh",
            "tests/e2e/core_lifecycle.sh",
            "tests/e2e/native_host.sh",
            "tests/e2e/server_shutdown.sh",
        ),
        doc_requirements=(
            ("docs/SUBSYSTEM_EVIDENCE_MATRIX.md", "bd-yx9r.4*"),
            ("docs/SUBSYSTEM_EVIDENCE_MATRIX.md", "bd-yx9r.5*"),
            ("docs/SUBSYSTEM_EVIDENCE_MATRIX.md", "bd-yx9r.6*"),
            ("docs/E2E_SCENARIO_PACKS_AND_REPORT_CONTRACT.md", "bd-yx9r.4"),
            ("docs/E2E_SCENARIO_PACKS_AND_REPORT_CONTRACT.md", "bd-yx9r.5"),
            ("docs/E2E_SCENARIO_PACKS_AND_REPORT_CONTRACT.md", "bd-yx9r.6"),
            ("docs/DEFERRED_SURFACE_REGISTER.md", "Runtime object/config/bootstrap beyond kernel loop"),
            ("docs/DEFERRED_SURFACE_REGISTER.md", "Capability / structured-concurrency surface"),
            ("docs/DEFERRED_SURFACE_REGISTER.md", "Trace / replay / lab / diagnostics"),
        ),
    ),
    FamilySpec(
        family_id="bd-yx9r.7",
        title="Combinator/service/transport/remote/spork",
        unit_globs=(
            "tests/unit/service/test_*.c",
            "tests/unit/transport/test_*.c",
            "tests/unit/remote/test_*.c",
            "tests/unit/spork/test_*.c",
            "tests/unit/stream/test_*.c",
        ),
        e2e_globs=(
            "tests/e2e/network_surface.sh",
            "tests/e2e/server_shutdown.sh",
        ),
        doc_requirements=(
            ("docs/SUBSYSTEM_EVIDENCE_MATRIX.md", "bd-yx9r.7*"),
            ("docs/E2E_SCENARIO_PACKS_AND_REPORT_CONTRACT.md", "bd-yx9r.7"),
            ("docs/USER_WORKFLOW_ACCEPTANCE_INVENTORY.md", "bd-yx9r.[4-12].*"),
        ),
    ),
    FamilySpec(
        family_id="bd-yx9r.8-9",
        title="Networking core and advanced transport",
        unit_globs=(
            "tests/unit/net/test_net.c",
            "tests/unit/net/test_tls.c",
            "tests/unit/net/test_server.c",
            "tests/unit/net/test_quic.c",
            "tests/unit/net/test_websocket.c",
            "tests/unit/transport/test_*.c",
        ),
        e2e_globs=(
            "tests/e2e/network_surface.sh",
            "tests/e2e/continuity.sh",
            "tests/e2e/router_storm.sh",
        ),
        doc_requirements=(
            ("docs/SUBSYSTEM_EVIDENCE_MATRIX.md", "bd-yx9r.8*"),
            ("docs/SUBSYSTEM_EVIDENCE_MATRIX.md", "bd-yx9r.9*"),
            ("docs/E2E_SCENARIO_PACKS_AND_REPORT_CONTRACT.md", "bd-yx9r.8"),
            ("docs/E2E_SCENARIO_PACKS_AND_REPORT_CONTRACT.md", "bd-yx9r.9"),
            ("docs/DEFERRED_SURFACE_REGISTER.md", "Networking core (`net`)"),
        ),
    ),
    FamilySpec(
        family_id="bd-yx9r.10",
        title="HTTP/web/gRPC/application surfaces",
        unit_globs=(
            "tests/unit/net/test_http.c",
            "tests/unit/net/test_web.c",
            "tests/unit/net/test_grpc.c",
            "tests/unit/net/test_db.c",
            "tests/unit/app/test_*.c",
        ),
        e2e_globs=(
            "tests/e2e/network_surface.sh",
            "tests/e2e/browser_smoke.sh",
            "tests/e2e/server_shutdown.sh",
        ),
        doc_requirements=(
            ("docs/SUBSYSTEM_EVIDENCE_MATRIX.md", "bd-yx9r.10*"),
            ("docs/E2E_SCENARIO_PACKS_AND_REPORT_CONTRACT.md", "bd-yx9r.10"),
            ("docs/USER_WORKFLOW_ACCEPTANCE_INVENTORY.md", "HTTP/gRPC/server/web/process/signal/CLI surfaces"),
            ("docs/DEFERRED_SURFACE_REGISTER.md", "HTTP / web / gRPC / transport / service / remote / distributed / messaging / server / TLS / database"),
        ),
    ),
    FamilySpec(
        family_id="bd-yx9r.11",
        title="Data/messaging/distributed systems",
        unit_globs=(
            "tests/unit/net/test_messaging.c",
            "tests/unit/net/test_distributed.c",
            "tests/unit/migration/test_*.c",
            "tests/unit/raptorq/test_*.c",
            "tests/unit/remote/test_*.c",
        ),
        e2e_globs=(
            "tests/e2e/network_surface.sh",
            "tests/e2e/continuity_restart.sh",
        ),
        doc_requirements=(
            ("docs/SUBSYSTEM_EVIDENCE_MATRIX.md", "bd-yx9r.11*"),
            ("docs/E2E_SCENARIO_PACKS_AND_REPORT_CONTRACT.md", "bd-yx9r.11"),
            ("docs/USER_WORKFLOW_ACCEPTANCE_INVENTORY.md", "Messaging/remote/distributed"),
        ),
    ),
    FamilySpec(
        family_id="bd-yx9r.12",
        title="Developer/operator/acceptance surfaces",
        unit_globs=(
            "tests/unit/console/test_*.c",
            "tests/unit/tracing_compat/test_*.c",
            "tests/unit/app/test_report.c",
            "tests/unit/runtime/test_browser_diagnostic.c",
        ),
        e2e_globs=(
            "tests/e2e/examples_smoke.sh",
            "tests/e2e/browser_smoke.sh",
            "tests/e2e/native_host.sh",
            "tests/e2e/server_shutdown.sh",
        ),
        doc_requirements=(
            ("docs/SUBSYSTEM_EVIDENCE_MATRIX.md", "bd-yx9r.12*"),
            ("docs/E2E_SCENARIO_PACKS_AND_REPORT_CONTRACT.md", "bd-yx9r.12"),
        ),
    ),
    FamilySpec(
        family_id="bd-yx9r.19",
        title="Actor/gen_server/supervision parity",
        unit_globs=(
            "tests/unit/actor/test_*.c",
        ),
        e2e_globs=(
            "tests/e2e/foundational_contracts.sh",
        ),
        doc_requirements=(
            ("docs/SUBSYSTEM_EVIDENCE_MATRIX.md", "actor / gen_server / supervision"),
            ("docs/E2E_SCENARIO_PACKS_AND_REPORT_CONTRACT.md", "bd-yx9r.19"),
            ("docs/DEFERRED_SURFACE_REGISTER.md", "Actor / supervision / gen_server"),
        ),
    ),
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run crate-level acceptance gate")
    parser.add_argument("--repo-root", default=None, help="Repository root")
    parser.add_argument("--run-id", default=None, help="Override run id")
    parser.add_argument("--build-dir", default="build", help="Build directory")
    parser.add_argument(
        "--artifact-dir",
        default="tools/ci/artifacts/acceptance",
        help="Summary artifact directory relative to repo root",
    )
    return parser.parse_args()


def latest_summary_for_mode(conformance_dir: Path, mode: str) -> Path | None:
    candidate: Path | None = None
    for path in sorted(conformance_dir.glob("*.summary.json"), key=lambda p: p.stat().st_mtime):
        try:
            data = load_json(path)
        except Exception:
            continue
        if data.get("mode") == mode:
            candidate = path
    return candidate


def main() -> int:
    args = parse_args()
    repo_root = Path(args.repo_root).resolve() if args.repo_root else Path(__file__).resolve().parents[2]
    build_dir = (repo_root / args.build_dir).resolve()
    summary_dir = (repo_root / args.artifact_dir).resolve()
    run_id = args.run_id or f"crate-acceptance-{datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')}"

    acceptance_dir = build_dir / "acceptance"
    acceptance_dir.mkdir(parents=True, exist_ok=True)
    summary_dir.mkdir(parents=True, exist_ok=True)

    report_path = acceptance_dir / f"crate_acceptance_{run_id}.json"
    summary_path = summary_dir / f"crate_acceptance_{run_id}.summary.json"

    required_docs = (
        "docs/FEATURE_PARITY.md",
        "docs/SUBSYSTEM_EVIDENCE_MATRIX.md",
        "docs/USER_WORKFLOW_ACCEPTANCE_INVENTORY.md",
        "docs/E2E_SCENARIO_PACKS_AND_REPORT_CONTRACT.md",
        "docs/DEFERRED_SURFACE_REGISTER.md",
        "docs/WAVE_GATING_PROTOCOL.md",
    )

    doc_results: list[dict[str, Any]] = []
    docs_ok = True
    for rel in required_docs:
        path = repo_root / rel
        exists = path.is_file()
        docs_ok = docs_ok and exists
        doc_results.append({"path": rel, "status": gate_status(exists)})

    conformance_dir = repo_root / "tools/ci/artifacts/conformance"
    latest_conformance = latest_summary_for_mode(conformance_dir, "conformance")
    latest_codec = latest_summary_for_mode(conformance_dir, "codec-equivalence")
    latest_profile = latest_summary_for_mode(conformance_dir, "profile-parity")
    latest_e2e = latest_matching(build_dir, "e2e-artifacts/*/run_manifest.json")

    artifact_results: list[dict[str, Any]] = []
    artifacts_ok = True

    def check_summary(name: str, path: Path | None, pass_predicate: Any) -> dict[str, Any]:
        nonlocal artifacts_ok
        if path is None:
            artifacts_ok = False
            return {"name": name, "status": "fail", "reason": "missing"}
        data = load_json(path)
        ok = bool(pass_predicate(data))
        artifacts_ok = artifacts_ok and ok
        return {
            "name": name,
            "status": gate_status(ok),
            "path": path.relative_to(repo_root).as_posix(),
            "mode": data.get("mode"),
        }

    artifact_results.append(
        check_summary(
            "conformance",
            latest_conformance,
            lambda data: int(data.get("fail", 1)) == 0 and bool(data.get("semantic_delta_pass", False)),
        )
    )
    artifact_results.append(
        check_summary(
            "codec-equivalence",
            latest_codec,
            lambda data: int(data.get("fail", 1)) == 0,
        )
    )
    artifact_results.append(
        check_summary(
            "profile-parity",
            latest_profile,
            lambda data: int(data.get("fail", 1)) == 0 and bool(data.get("semantic_delta_pass", False)),
        )
    )

    if latest_e2e is None:
        artifacts_ok = False
        artifact_results.append({"name": "e2e-manifest", "status": "fail", "reason": "missing"})
        e2e_manifest = None
    else:
        e2e_manifest = load_json(latest_e2e)
        e2e_ok = e2e_manifest.get("status") == "pass" and int(e2e_manifest.get("failed_families", 1)) == 0
        artifacts_ok = artifacts_ok and e2e_ok
        artifact_results.append(
            {
                "name": "e2e-manifest",
                "status": gate_status(e2e_ok),
                "path": latest_e2e.relative_to(repo_root).as_posix(),
            }
        )

    family_results: list[dict[str, Any]] = []
    families_ok = True
    for spec in FAMILY_SPECS:
        unit_matches = first_matches(repo_root, spec.unit_globs)
        e2e_matches = first_matches(repo_root, spec.e2e_globs)
        doc_checks = []
        docs_present = True
        for rel, marker in spec.doc_requirements:
            path = repo_root / rel
            present = path.is_file() and text_contains(path, marker)
            docs_present = docs_present and present
            doc_checks.append({"path": rel, "marker": marker, "status": gate_status(present)})
        unit_ok = len(unit_matches) > 0
        e2e_ok = len(e2e_matches) > 0
        family_ok = docs_present and unit_ok and e2e_ok
        families_ok = families_ok and family_ok
        family_results.append(
            {
                "family_id": spec.family_id,
                "title": spec.title,
                "status": gate_status(family_ok),
                "docs_status": gate_status(docs_present),
                "unit_status": gate_status(unit_ok),
                "e2e_status": gate_status(e2e_ok),
                "doc_checks": doc_checks,
                "unit_matches": unit_matches,
                "e2e_matches": e2e_matches,
            }
        )

    verdict = docs_ok and artifacts_ok and families_ok

    report = {
        "kind": "crate_acceptance_gate",
        "run_id": run_id,
        "generated_at": utc_now(),
        "repo_root": repo_root.as_posix(),
        "verdict": gate_status(verdict),
        "requirements": {
            "docs_ok": docs_ok,
            "artifacts_ok": artifacts_ok,
            "families_ok": families_ok,
        },
        "required_docs": doc_results,
        "artifact_checks": artifact_results,
        "family_checks": family_results,
        "sources": {
            "latest_conformance_summary": None if latest_conformance is None else latest_conformance.relative_to(repo_root).as_posix(),
            "latest_codec_summary": None if latest_codec is None else latest_codec.relative_to(repo_root).as_posix(),
            "latest_profile_summary": None if latest_profile is None else latest_profile.relative_to(repo_root).as_posix(),
            "latest_e2e_manifest": None if latest_e2e is None else latest_e2e.relative_to(repo_root).as_posix(),
        },
    }

    summary = {
        "kind": "crate_acceptance_gate_summary",
        "run_id": run_id,
        "generated_at": report["generated_at"],
        "status": report["verdict"],
        "required_doc_failures": [entry["path"] for entry in doc_results if entry["status"] != "pass"],
        "artifact_failures": [entry["name"] for entry in artifact_results if entry["status"] != "pass"],
        "family_failures": [entry["family_id"] for entry in family_results if entry["status"] != "pass"],
        "report_file": report_path.relative_to(repo_root).as_posix(),
        "summary_file": summary_path.relative_to(repo_root).as_posix(),
    }

    report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    summary_path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    json.dump(summary, sys.stdout)
    sys.stdout.write("\n")
    return 0 if verdict else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except BrokenPipeError:
        raise SystemExit(2)
