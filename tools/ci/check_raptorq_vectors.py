#!/usr/bin/env python3
"""check_raptorq_vectors.py — the golden RaptorQ vectors of the C unit test
against the independent implementations that produced them (bd-9kll.16.2).

tests/unit/raptorq/test_raptorq.c carries golden tables (`g_block_golden`,
`g_k10_repair`, `g_obj_golden` and the `g_obj<N>` block tables) "produced by
the independent crates.io raptorq crate (cberner/raptorq 2.0.1)". The script
that produced them was never committed, so nothing could say that the numbers
are still the crate's. This is that script: it reads the tables from the C
source, hands their inputs to `twin_run parse-diff raptorq-vectors` (which
runs the crate, and asupersync's encoder where it has one, over the same
inputs, tools/twin_run/src/rqoracle.rs), and compares every number. A table
the script cannot find, or finds empty, is a failure, not a pass.

  check_raptorq_vectors.py --twin-run <twin_run binary> [--source <test .c>]
  check_raptorq_vectors.py --selftest

Exit 0: every vector of the C table is what the crate produces (and what
asupersync produces where it has an encoder for the row). Exit 1: a number
differs. Exit 2: a tool error (table missing, twin_run failed).
"""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

DEFAULT_SOURCE = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "..", "tests", "unit", "raptorq",
    "test_raptorq.c",
)
TIMEOUT = 600  # seconds for the twin_run run (a K=8192 block is dense work)

U = r"(\d+)u"
H64 = r"UINT64_C\(0x([0-9a-f]+)\)"


def table_body(text, name):
    """The text between `name[] = {` and its closing `};`, or None."""
    match = re.search(r"\b%s\[\d*\]\s*=\s*\{" % re.escape(name), text)
    if match is None:
        return None
    depth = 1
    i = match.end()
    while i < len(text) and depth:
        depth += {"{": 1, "}": -1}.get(text[i], 0)
        i += 1
    return text[match.end() : i - 1] if depth == 0 else None


def parse_tables(text):
    """The golden tables of the C source: {"blocks": [...], "k10": [...hex],
    "objects": [...]}; raises ValueError when one is missing or empty."""
    body = table_body(text, "g_block_golden")
    if body is None:
        raise ValueError("g_block_golden not found")
    blocks = []
    pattern = re.compile(
        r"\{\s*%s,\s*%s,\s*%s,\s*%s,\s*%s,\s*\"([0-9a-f]+)\",\s*%s\s*\}" % (U, U, U, U, H64, H64)
    )
    for m in pattern.finditer(body):
        k, t, seed, count, digest, first, far = m.groups()
        blocks.append({"k": int(k), "t": int(t), "seed": int(seed), "count": int(count),
                       "digest": digest, "first_repair": first, "far_digest": far})
    if not blocks:
        raise ValueError("g_block_golden has no rows")

    body = table_body(text, "g_k10_repair")
    if body is None:
        raise ValueError("g_k10_repair not found")
    k10 = re.findall(r'"([0-9a-f]+)"', body)
    if not k10:
        raise ValueError("g_k10_repair is empty")

    per_object = {}
    for m in re.finditer(r"static const rq_obj_block_golden (g_obj\d+)\[\]", text):
        name = m.group(1)
        body = table_body(text, name)
        rows = re.findall(r"\{\s*%s,\s*%s,\s*%s,\s*%s\s*\}" % (U, U, H64, H64), body or "")
        if not rows:
            raise ValueError("%s is empty" % name)
        per_object[name] = [
            {"sbn": int(s), "k": int(k), "source_digest": sd, "repair_digest": rd}
            for s, k, sd, rd in rows
        ]
    body = table_body(text, "g_obj_golden")
    if body is None:
        raise ValueError("g_obj_golden not found")
    objects = []
    pattern = re.compile(
        r"\{\s*%s,\s*%s,\s*%s,\s*%s,\s*%s,\s*%s,\s*\"([0-9a-f]+)\",\s*(g_obj\d+)\s*\}"
        % (U, U, U, U, U, U)
    )
    for m in pattern.finditer(body):
        f, t, z, n, al, seed, oti, name = m.groups()
        if name not in per_object:
            raise ValueError("g_obj_golden names %s, which is not a table" % name)
        objects.append({"f": int(f), "t": int(t), "z": int(z), "n": int(n), "al": int(al),
                        "seed": int(seed), "oti_hex": oti, "blocks": per_object[name]})
    if not objects:
        raise ValueError("g_obj_golden has no rows")
    return {"blocks": blocks, "k10": k10, "objects": objects}


def spec_of(tables):
    blocks = [{"k": b["k"], "t": b["t"], "seed": b["seed"], "count": b["count"]}
              for b in tables["blocks"]]
    # The K=10 repair symbols of g_k10_repair: T=16, seed=3, ESIs 10..21.
    blocks.append({"k": 10, "t": 16, "seed": 3, "count": len(tables["k10"]),
                   "list": len(tables["k10"])})
    objects = [{key: o[key] for key in ("f", "t", "z", "n", "al", "seed")}
               for o in tables["objects"]]
    return {"blocks": blocks, "objects": objects}


def compare(tables, got):
    """Lines describing every difference between the C tables and the
    twin_run output `got` (empty when they agree), and a count of the numbers
    checked."""
    diffs = []
    checked = 0

    def check(what, want, have):
        nonlocal checked
        checked += 1
        if want != have:
            diffs.append("%s: C table %s, reference %s" % (what, want, have))

    rows = got["blocks"]
    if len(rows) != len(tables["blocks"]) + 1:
        return ["twin_run returned %d block rows for %d" % (len(rows), len(tables["blocks"]) + 1)], 0
    for table, row in zip(tables["blocks"], rows):
        tag = "block K=%d T=%d seed=%d" % (table["k"], table["t"], table["seed"])
        check(tag + " repair digest", table["digest"], row["digest"])
        check(tag + " first repair symbol", table["first_repair"], row["first_repair"])
        check(tag + " far digest", table["far_digest"], row["far_digest"])
        if row.get("asupersync_digest") is not None:
            check(tag + " repair digest (asupersync)", table["digest"], row["asupersync_digest"])
        if row.get("asupersync_far_digest") is not None:
            check(tag + " far digest (asupersync)", table["far_digest"],
                  row["asupersync_far_digest"])
    check("K=10 T=16 seed=3 repair symbols ESI 10..", tables["k10"], rows[-1]["listed"])
    if len(got["objects"]) != len(tables["objects"]):
        diffs.append("twin_run returned %d object rows for %d"
                     % (len(got["objects"]), len(tables["objects"])))
        return diffs, checked
    for table, row in zip(tables["objects"], got["objects"]):
        tag = "object F=%d T=%d Z=%d N=%d Al=%d" % (
            table["f"], table["t"], table["z"], table["n"], table["al"])
        check(tag + " OTI", table["oti_hex"], row["oti_hex"])
        if len(row["blocks"]) != len(table["blocks"]):
            diffs.append("%s: C table has %d blocks, reference %d"
                         % (tag, len(table["blocks"]), len(row["blocks"])))
            continue
        for want, have in zip(table["blocks"], row["blocks"]):
            btag = "%s block %d" % (tag, want["sbn"])
            check(btag + " K", want["k"], have["k"])
            check(btag + " source digest", want["source_digest"], have["source_digest"])
            check(btag + " repair digest", want["repair_digest"], have["repair_digest"])
    return diffs, checked


def run_twin(twin_run, spec):
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "spec.json")
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(spec, fh)
        done = subprocess.run(
            [twin_run, "parse-diff", "raptorq-vectors", path],
            capture_output=True, encoding="utf-8", errors="replace", check=False,
            timeout=TIMEOUT,
        )
    if done.returncode != 0:
        raise RuntimeError("twin_run exited %d: %s" % (done.returncode, done.stderr[-400:]))
    lines = [line for line in done.stdout.split("\n") if line.startswith("{")]
    if len(lines) != 1:
        raise RuntimeError("twin_run printed %d result lines" % len(lines))
    try:
        outcome = json.loads(lines[0])["outcome"]
    except (ValueError, KeyError, TypeError) as err:
        raise RuntimeError("twin_run printed a line that is not an outcome") from err
    if outcome.get("kind") != "complete":
        raise RuntimeError("twin_run: %s" % outcome.get("detail", outcome))
    return outcome


SAMPLE = """
static const rq_block_golden g_block_golden[] = {
    {1u, 16u, 1u, 8u, UINT64_C(0x6d056193a18e0880), "3c5e81b40c5ec68e04a3406c97d63cfb",
     UINT64_C(0x0840b659f53ebb4f)},
};
static const char *const g_k10_repair[12] = {
    "e39a283a99cbcb50941f1328a558abf5", "e4cd704c6af2d9ee82ffee4dd91014aa"};
static const rq_obj_block_golden g_obj0[] = {
    {0u, 16u, UINT64_C(0xdf631dd2cee865db), UINT64_C(0xcacf30302c0f2d22)},
    {1u, 15u, UINT64_C(0xc51809c8f17bce73), UINT64_C(0x016816256a6815da)}};
static const rq_obj_golden g_obj_golden[] = {
    {1000u, 16u, 4u, 1u, 1u, 11u, "00000003e800001004000101", g_obj0},
};
"""


def selftest():
    failures = []
    tables = parse_tables(SAMPLE)
    if tables["blocks"][0]["digest"] != "6d056193a18e0880":
        failures.append("block row parsed wrong: %r" % tables["blocks"])
    if tables["k10"] != ["e39a283a99cbcb50941f1328a558abf5", "e4cd704c6af2d9ee82ffee4dd91014aa"]:
        failures.append("k10 parsed wrong: %r" % tables["k10"])
    if [b["k"] for b in tables["objects"][0]["blocks"]] != [16, 15]:
        failures.append("object blocks parsed wrong: %r" % tables["objects"])
    spec = spec_of(tables)
    if spec["blocks"][-1] != {"k": 10, "t": 16, "seed": 3, "count": 2, "list": 2}:
        failures.append("spec wrong: %r" % spec)
    # The numbers of a faithful reference agree; one wrong number is named.
    got = {
        "blocks": [
            {"digest": "6d056193a18e0880", "first_repair": "3c5e81b40c5ec68e04a3406c97d63cfb",
             "far_digest": "0840b659f53ebb4f", "asupersync_digest": None,
             "asupersync_far_digest": None},
            {"listed": tables["k10"]},
        ],
        "objects": [{"oti_hex": "00000003e800001004000101",
                     "blocks": [
                         {"sbn": 0, "k": 16, "source_digest": "df631dd2cee865db",
                          "repair_digest": "cacf30302c0f2d22"},
                         {"sbn": 1, "k": 15, "source_digest": "c51809c8f17bce73",
                          "repair_digest": "016816256a6815da"}]}],
    }
    diffs, checked = compare(tables, got)
    if diffs or checked != 11:
        failures.append("a faithful reference differs: %r (%d checked)" % (diffs, checked))
    got["objects"][0]["blocks"][1]["repair_digest"] = "1"
    diffs, _ = compare(tables, got)
    if len(diffs) != 1 or "block 1 repair digest" not in diffs[0]:
        failures.append("one wrong digest is not named: %r" % diffs)
    for broken in ("", "static const rq_block_golden g_block_golden[] = {};"):
        try:
            parse_tables(broken)
            failures.append("a missing or empty table must be an error")
        except ValueError:
            pass
    if failures:
        for what in failures:
            sys.stderr.write("selftest FAIL: %s\n" % what)
        return 1
    sys.stdout.write("check_raptorq_vectors selftest: PASS\n")
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--twin-run", help="the twin_run binary")
    ap.add_argument("--source", default=DEFAULT_SOURCE, help="tests/unit/raptorq/test_raptorq.c")
    args = ap.parse_args(argv)
    if args.selftest:
        return selftest()
    if not args.twin_run:
        sys.stderr.write("check_raptorq_vectors: --twin-run is required\n")
        return 2
    try:
        with open(args.source, encoding="utf-8") as fh:
            tables = parse_tables(fh.read())
        got = run_twin(args.twin_run, spec_of(tables))
    except (OSError, ValueError, RuntimeError, KeyError, subprocess.TimeoutExpired) as err:
        sys.stderr.write("check_raptorq_vectors: %s\n" % err)
        return 2
    diffs, checked = compare(tables, got)
    sys.stdout.write("check_raptorq_vectors: %d numbers of %d block rows, %d object rows checked\n"
                     % (checked, len(tables["blocks"]) + 1, len(tables["objects"])))
    skipped = [b for b in got["blocks"][:-1] if b.get("asupersync_far_digest") is None]
    if skipped:
        sys.stdout.write("  asupersync gave no far digest for %d block row(s)\n" % len(skipped))
    for line in diffs:
        sys.stdout.write("  DIFFERS: %s\n" % line)
    if diffs:
        sys.stdout.write("check_raptorq_vectors: FAIL, %d difference(s)\n" % len(diffs))
        return 1
    sys.stdout.write("check_raptorq_vectors: PASS\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
