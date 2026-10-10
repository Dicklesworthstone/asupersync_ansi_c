#!/usr/bin/env python3
"""
zero_drift_extractor.py — Rust-AST zero-drift extractor prototype (bd-3vt.10)

RETIRED (bd-9kll.2.13): it parses Rust fragments embedded in this file, not
asupersync, so it cannot detect upstream drift. `twin_run constants` reads
the constants from the pinned crate itself (schemas/rust_kernel_constants.json)
and `make check-rust-constants` compares C with them. Nothing calls this
file; it stays until the owner approves its deletion.

Extracts state machine transition tables and invariant schema fragments from
Rust enum definitions and transition implementations.

This prototype uses regex-based parsing on representative Rust source fragments.
A production version would use syn/tree-sitter-rust for full AST fidelity.

Usage:
    python3 tools/zero_drift_extractor.py [--compare] [--emit-c] [--emit-json]

Options:
    --compare   Compare extracted tables against manually curated C baseline
    --emit-c    Print C transition table source
    --emit-json Print JSON schema fragment
    --self-test Run self-test (built-in validation)

SPDX-License-Identifier: MIT
"""

import json
import re
import sys
from typing import NamedTuple

# ------------------------------------------------------------------ #
# Sample Rust source fragments (embedded test data)                   #
# These represent the asupersync Rust source structure.               #
# ------------------------------------------------------------------ #

RUST_REGION_ENUM = """
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum RegionState {
    Open = 0,
    Closing = 1,
    Draining = 2,
    Finalizing = 3,
    Closed = 4,
}
"""

RUST_REGION_TRANSITIONS = """
impl RegionState {
    pub fn can_transition_to(&self, target: RegionState) -> bool {
        matches!(
            (self, target),
            (RegionState::Open, RegionState::Closing)
                | (RegionState::Closing, RegionState::Draining)
                | (RegionState::Closing, RegionState::Finalizing)
                | (RegionState::Draining, RegionState::Finalizing)
                | (RegionState::Finalizing, RegionState::Closed)
        )
    }

    pub fn is_terminal(&self) -> bool {
        matches!(self, RegionState::Closed)
    }
}
"""

RUST_TASK_ENUM = """
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum TaskState {
    Created = 0,
    Running = 1,
    CancelRequested = 2,
    Cancelling = 3,
    Finalizing = 4,
    Completed = 5,
}
"""

RUST_TASK_TRANSITIONS = """
impl TaskState {
    pub fn can_transition_to(&self, target: TaskState) -> bool {
        matches!(
            (self, target),
            (TaskState::Created, TaskState::Running)
                | (TaskState::Created, TaskState::CancelRequested)
                | (TaskState::Created, TaskState::Completed)
                | (TaskState::Running, TaskState::CancelRequested)
                | (TaskState::Running, TaskState::Completed)
                | (TaskState::CancelRequested, TaskState::CancelRequested)
                | (TaskState::CancelRequested, TaskState::Cancelling)
                | (TaskState::CancelRequested, TaskState::Completed)
                | (TaskState::Cancelling, TaskState::Cancelling)
                | (TaskState::Cancelling, TaskState::Finalizing)
                | (TaskState::Cancelling, TaskState::Completed)
                | (TaskState::Finalizing, TaskState::Finalizing)
                | (TaskState::Finalizing, TaskState::Completed)
        )
    }

    pub fn is_terminal(&self) -> bool {
        matches!(self, TaskState::Completed)
    }
}
"""

RUST_OBLIGATION_ENUM = """
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum ObligationState {
    Reserved = 0,
    Committed = 1,
    Aborted = 2,
    Leaked = 3,
}
"""

RUST_OBLIGATION_TRANSITIONS = """
impl ObligationState {
    pub fn can_transition_to(&self, target: ObligationState) -> bool {
        matches!(
            (self, target),
            (ObligationState::Reserved, ObligationState::Committed)
                | (ObligationState::Reserved, ObligationState::Aborted)
                | (ObligationState::Reserved, ObligationState::Leaked)
        )
    }

    pub fn is_terminal(&self) -> bool {
        matches!(self, ObligationState::Committed | ObligationState::Aborted | ObligationState::Leaked)
    }
}
"""


# ------------------------------------------------------------------ #
# Parser: Extract enum variants and transition pairs from Rust source #
# ------------------------------------------------------------------ #

class EnumDef(NamedTuple):
    name: str
    variants: list  # [(name, ordinal), ...]


class TransitionPair(NamedTuple):
    from_state: str
    to_state: str


def parse_enum(source: str) -> EnumDef:
    """Parse a Rust enum definition with explicit ordinal values."""
    name_match = re.search(r'pub\s+enum\s+(\w+)', source)
    if not name_match:
        return EnumDef("", [])
    name = name_match.group(1)

    # Extract variants: Name = ordinal
    variants = re.findall(r'(\w+)\s*=\s*(\d+)', source)
    return EnumDef(name, [(v, int(o)) for v, o in variants])


def parse_transitions(source: str, enum_name: str) -> list:
    """Parse transition pairs from matches!((self, target), ...) blocks."""
    # Find all (EnumName::Variant, EnumName::Variant) patterns
    pattern = rf'\({enum_name}::(\w+),\s*{enum_name}::(\w+)\)'
    pairs = re.findall(pattern, source)
    return [TransitionPair(f, t) for f, t in pairs]


def parse_terminals(source: str, enum_name: str) -> list:
    """Parse terminal states from is_terminal matches! blocks."""
    # Find is_terminal method and extract variant names
    term_match = re.search(
        r'fn\s+is_terminal.*?matches!\s*\(self,\s*(.*?)\)',
        source, re.DOTALL
    )
    if not term_match:
        return []
    body = term_match.group(1)
    return re.findall(rf'{enum_name}::(\w+)', body)


# ------------------------------------------------------------------ #
# Emitters: Generate C source and JSON schema from parsed data        #
# ------------------------------------------------------------------ #

def build_transition_matrix(enum_def: EnumDef,
                             transitions: list) -> list:
    """Build NxN boolean transition matrix from enum and pairs."""
    n = len(enum_def.variants)
    matrix = [[0] * n for _ in range(n)]
    name_to_idx = {v: i for v, i in enum_def.variants}

    for pair in transitions:
        fi = name_to_idx.get(pair.from_state)
        ti = name_to_idx.get(pair.to_state)
        if fi is not None and ti is not None:
            matrix[fi][ti] = 1

    return matrix


def emit_c_table(domain: str, enum_def: EnumDef,
                  matrix: list) -> str:
    """Emit C transition table source code."""
    n = len(enum_def.variants)
    prefix = f"ASX_{domain.upper()}"
    lines = []
    lines.append(f"static const int {domain}_transitions[{n}][{n}] = {{")

    # Column header comment
    abbrevs = [v[:5] for v, _ in enum_def.variants]
    lines.append(f"    /* To:  {'  '.join(f'{a:<5}' for a in abbrevs)} */")

    for i, (vname, _) in enumerate(enum_def.variants):
        abbrev = vname[:5]
        row = ", ".join(f"{matrix[i][j]:>4}" for j in range(n))
        lines.append(f"    /*{abbrev:<5}*/ {{{row}}},")

    lines.append("};")
    return "\n".join(lines)


def emit_json_schema_fragment(domain: str, enum_def: EnumDef,
                                transitions: list,
                                terminals: list) -> dict:
    """Emit JSON invariant schema fragment for a domain."""
    c_prefix = f"ASX_{domain.upper()}_"

    states = []
    for vname, ordinal in enum_def.variants:
        states.append({
            "id": vname,
            "enum_value": c_prefix + vname.upper(),
            "ordinal": ordinal,
            "terminal": vname in terminals
        })

    legal = []
    for i, pair in enumerate(transitions):
        legal.append({
            "id": f"{domain[0].upper()}{i+1}",
            "from": pair.from_state,
            "to": pair.to_state,
        })

    # Build forbidden transitions
    name_to_idx = {v: i for v, i in enum_def.variants}
    legal_set = {(p.from_state, p.to_state) for p in transitions}
    forbidden = []
    for fv, _ in enum_def.variants:
        for tv, _ in enum_def.variants:
            if (fv, tv) not in legal_set:
                reason = "terminal" if fv in terminals else "forbidden"
                forbidden.append({
                    "from": fv,
                    "to": tv,
                    "error": "ASX_E_INVALID_TRANSITION",
                    "reason": reason
                })

    return {
        "states": states,
        "legal_transitions": legal,
        "forbidden_transitions": forbidden,
        "state_count": len(states),
        "legal_count": len(legal),
        "forbidden_count": len(forbidden),
    }


# ------------------------------------------------------------------ #
# Baseline comparison: check extracted tables against C source        #
# ------------------------------------------------------------------ #

# Manually curated C baseline (from transition_tables.c)
C_BASELINE = {
    "region": {
        "variants": [("Open", 0), ("Closing", 1), ("Draining", 2),
                      ("Finalizing", 3), ("Closed", 4)],
        "matrix": [
            [0, 1, 0, 0, 0],
            [0, 0, 1, 1, 0],
            [0, 0, 0, 1, 0],
            [0, 0, 0, 0, 1],
            [0, 0, 0, 0, 0],
        ],
        "terminals": ["Closed"],
    },
    "task": {
        "variants": [("Created", 0), ("Running", 1), ("CancelRequested", 2),
                      ("Cancelling", 3), ("Finalizing", 4), ("Completed", 5)],
        "matrix": [
            [0, 1, 1, 0, 0, 1],
            [0, 0, 1, 0, 0, 1],
            [0, 0, 1, 1, 0, 1],
            [0, 0, 0, 1, 1, 1],
            [0, 0, 0, 0, 1, 1],
            [0, 0, 0, 0, 0, 0],
        ],
        "terminals": ["Completed"],
    },
    "obligation": {
        "variants": [("Reserved", 0), ("Committed", 1),
                      ("Aborted", 2), ("Leaked", 3)],
        "matrix": [
            [0, 1, 1, 1],
            [0, 0, 0, 0],
            [0, 0, 0, 0],
            [0, 0, 0, 0],
        ],
        "terminals": ["Committed", "Aborted", "Leaked"],
    },
}


def compare_domain(domain: str, extracted_matrix: list,
                    extracted_terminals: list) -> dict:
    """Compare extracted data against C baseline for one domain."""
    baseline = C_BASELINE[domain]
    result = {
        "domain": domain,
        "matrix_match": extracted_matrix == baseline["matrix"],
        "terminal_match": sorted(extracted_terminals) == sorted(baseline["terminals"]),
        "variant_count": len(baseline["variants"]),
        "legal_transitions_baseline": sum(
            sum(row) for row in baseline["matrix"]
        ),
        "legal_transitions_extracted": sum(
            sum(row) for row in extracted_matrix
        ),
        "mismatches": [],
    }

    # Detail mismatches
    n = len(baseline["variants"])
    for i in range(n):
        for j in range(n):
            if i < len(extracted_matrix) and j < len(extracted_matrix[i]):
                if extracted_matrix[i][j] != baseline["matrix"][i][j]:
                    vnames = [v for v, _ in baseline["variants"]]
                    result["mismatches"].append({
                        "from": vnames[i],
                        "to": vnames[j],
                        "baseline": baseline["matrix"][i][j],
                        "extracted": extracted_matrix[i][j],
                    })

    return result


# ------------------------------------------------------------------ #
# Main pipeline: parse → extract → emit → compare                    #
# ------------------------------------------------------------------ #

RUST_SOURCES = {
    "region": (RUST_REGION_ENUM, RUST_REGION_TRANSITIONS, "RegionState"),
    "task": (RUST_TASK_ENUM, RUST_TASK_TRANSITIONS, "TaskState"),
    "obligation": (RUST_OBLIGATION_ENUM, RUST_OBLIGATION_TRANSITIONS, "ObligationState"),
}


def run_extraction():
    """Run full extraction pipeline on all domains."""
    results = {}
    for domain, (enum_src, trans_src, enum_name) in RUST_SOURCES.items():
        enum_def = parse_enum(enum_src)
        transitions = parse_transitions(trans_src, enum_name)
        terminals = parse_terminals(trans_src, enum_name)
        matrix = build_transition_matrix(enum_def, transitions)

        results[domain] = {
            "enum_def": enum_def,
            "transitions": transitions,
            "terminals": terminals,
            "matrix": matrix,
        }
    return results


def run_comparison(results: dict) -> dict:
    """Compare all domains against baseline."""
    report = {"domains": {}, "all_match": True}
    for domain, data in results.items():
        cmp = compare_domain(domain, data["matrix"], data["terminals"])
        report["domains"][domain] = cmp
        if not cmp["matrix_match"] or not cmp["terminal_match"]:
            report["all_match"] = False
    return report


def self_test():
    """Run built-in validation tests."""
    results = run_extraction()
    report = run_comparison(results)

    tests_run = 0
    tests_passed = 0

    # Test 1: All domains extracted
    tests_run += 1
    if len(results) == 3:
        tests_passed += 1
        print("  PASS: all 3 domains extracted")
    else:
        print(f"  FAIL: expected 3 domains, got {len(results)}")

    # Test 2: Region enum has 5 variants
    tests_run += 1
    if len(results["region"]["enum_def"].variants) == 5:
        tests_passed += 1
        print("  PASS: region has 5 variants")
    else:
        print(f"  FAIL: region variants: {len(results['region']['enum_def'].variants)}")

    # Test 3: Task enum has 6 variants
    tests_run += 1
    if len(results["task"]["enum_def"].variants) == 6:
        tests_passed += 1
        print("  PASS: task has 6 variants")
    else:
        print(f"  FAIL: task variants: {len(results['task']['enum_def'].variants)}")

    # Test 4: Obligation enum has 4 variants
    tests_run += 1
    if len(results["obligation"]["enum_def"].variants) == 4:
        tests_passed += 1
        print("  PASS: obligation has 4 variants")
    else:
        print(f"  FAIL: obligation variants: {len(results['obligation']['enum_def'].variants)}")

    # Test 5: Region transition count
    tests_run += 1
    region_legal = sum(sum(r) for r in results["region"]["matrix"])
    if region_legal == 5:
        tests_passed += 1
        print("  PASS: region has 5 legal transitions")
    else:
        print(f"  FAIL: region legal transitions: {region_legal}")

    # Test 6: Task transition count
    tests_run += 1
    task_legal = sum(sum(r) for r in results["task"]["matrix"])
    if task_legal == 13:
        tests_passed += 1
        print("  PASS: task has 13 legal transitions")
    else:
        print(f"  FAIL: task legal transitions: {task_legal}")

    # Test 7: Obligation transition count
    tests_run += 1
    obl_legal = sum(sum(r) for r in results["obligation"]["matrix"])
    if obl_legal == 3:
        tests_passed += 1
        print("  PASS: obligation has 3 legal transitions")
    else:
        print(f"  FAIL: obligation legal transitions: {obl_legal}")

    # Test 8: Region matrix matches C baseline
    tests_run += 1
    if report["domains"]["region"]["matrix_match"]:
        tests_passed += 1
        print("  PASS: region matrix matches C baseline")
    else:
        print(f"  FAIL: region mismatches: {report['domains']['region']['mismatches']}")

    # Test 9: Task matrix matches C baseline
    tests_run += 1
    if report["domains"]["task"]["matrix_match"]:
        tests_passed += 1
        print("  PASS: task matrix matches C baseline")
    else:
        print(f"  FAIL: task mismatches: {report['domains']['task']['mismatches']}")

    # Test 10: Obligation matrix matches C baseline
    tests_run += 1
    if report["domains"]["obligation"]["matrix_match"]:
        tests_passed += 1
        print("  PASS: obligation matrix matches C baseline")
    else:
        print(f"  FAIL: obligation mismatches: {report['domains']['obligation']['mismatches']}")

    # Test 11: Terminal states match
    tests_run += 1
    all_terminals = all(
        report["domains"][d]["terminal_match"] for d in report["domains"]
    )
    if all_terminals:
        tests_passed += 1
        print("  PASS: all terminal states match C baseline")
    else:
        print("  FAIL: terminal state mismatch")

    # Test 12: C emission is valid
    tests_run += 1
    c_output = emit_c_table("region", results["region"]["enum_def"],
                             results["region"]["matrix"])
    if "static const int region_transitions[5][5]" in c_output:
        tests_passed += 1
        print("  PASS: C table emission contains correct declaration")
    else:
        print(f"  FAIL: unexpected C output: {c_output[:80]}")

    # Test 13: JSON emission is valid
    tests_run += 1
    schema = emit_json_schema_fragment("region", results["region"]["enum_def"],
                                        results["region"]["transitions"],
                                        results["region"]["terminals"])
    if schema["state_count"] == 5 and schema["legal_count"] == 5:
        tests_passed += 1
        print("  PASS: JSON schema fragment has correct counts")
    else:
        print(f"  FAIL: schema counts: states={schema['state_count']}, legal={schema['legal_count']}")

    # Test 14: Forbidden transition count is correct
    tests_run += 1
    # Region: 5x5=25 total, 5 legal, 20 forbidden
    if schema["forbidden_count"] == 20:
        tests_passed += 1
        print("  PASS: region has 20 forbidden transitions")
    else:
        print(f"  FAIL: forbidden count: {schema['forbidden_count']}")

    # Test 15: Enum names parsed correctly
    tests_run += 1
    names_ok = (results["region"]["enum_def"].name == "RegionState"
                and results["task"]["enum_def"].name == "TaskState"
                and results["obligation"]["enum_def"].name == "ObligationState")
    if names_ok:
        tests_passed += 1
        print("  PASS: enum names parsed correctly")
    else:
        print("  FAIL: enum names incorrect")

    # Test 16: Self-transitions detected (task CancelRequested->CancelRequested)
    tests_run += 1
    task_self = any(
        t.from_state == "CancelRequested" and t.to_state == "CancelRequested"
        for t in results["task"]["transitions"]
    )
    if task_self:
        tests_passed += 1
        print("  PASS: task self-transition (strengthen) detected")
    else:
        print("  FAIL: task self-transition not found")

    # Test 17: Overall parity
    tests_run += 1
    if report["all_match"]:
        tests_passed += 1
        print("  PASS: OVERALL — all extracted tables match C baseline")
    else:
        print("  FAIL: OVERALL — parity mismatch detected")

    print(f"\n{tests_passed}/{tests_run} tests passed")
    return tests_passed == tests_run


def main():
    args = sys.argv[1:]

    if "--self-test" in args or not args:
        print("=== zero-drift extractor self-test (bd-3vt.10) ===")
        ok = self_test()
        sys.exit(0 if ok else 1)

    results = run_extraction()

    if "--compare" in args:
        report = run_comparison(results)
        print(json.dumps(report, indent=2))

    if "--emit-c" in args:
        for domain, data in results.items():
            print(f"\n/* {domain} transitions (auto-generated) */")
            print(emit_c_table(domain, data["enum_def"], data["matrix"]))

    if "--emit-json" in args:
        schemas = {}
        for domain, data in results.items():
            schemas[domain] = emit_json_schema_fragment(
                domain, data["enum_def"],
                data["transitions"], data["terminals"]
            )
        print(json.dumps({"domains": schemas}, indent=2))


if __name__ == "__main__":
    main()
