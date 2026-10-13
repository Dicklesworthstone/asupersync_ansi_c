#!/usr/bin/env python3
"""fuzz_coverage.py - what the generated corpus leaves of the C runtime unexecuted (bd-9kll.2.10, G4).

Two subcommands, driven by tools/ci/fuzz_coverage.sh:

  collect BUILD_DIR OUT.json
      Runs gcov (JSON format) on every .gcda under BUILD_DIR and writes
      {source file: {line: execution count}} for the files under src/.

  report FIXTURES.json GENERATED.json [--top N] [--min-gap N] [--source-root DIR]
      The lines the committed Rust fixtures execute and the generated corpus
      does not, per file and as line ranges with the first source line of each,
      and the lines neither executes (counted per file). A line the fixtures
      reach and the generator never does is a part of the grammar the
      generator does not draw, or draws with the wrong shape: the work list of
      the next extension.

Exit status 0. The report is data, not a gate: a threshold gate would need a
baseline per generator version.
"""

import argparse
import json
import os
import subprocess
import sys


def collect(build_dir, out_path):
    gcdas = []
    for root, _dirs, files in os.walk(build_dir):
        for name in files:
            if name.endswith(".gcda"):
                gcdas.append(os.path.join(root, name))
    gcdas.sort()
    cov = {}
    for gcda in gcdas:
        proc = subprocess.run(
            ["gcov", "--json-format", "--stdout", os.path.basename(gcda)],
            cwd=os.path.dirname(gcda),
            capture_output=True,
            text=True,
            check=False,
        )
        if proc.returncode != 0 or not proc.stdout.strip():
            continue
        for chunk in proc.stdout.splitlines():
            chunk = chunk.strip()
            if not chunk.startswith("{"):
                continue
            try:
                doc = json.loads(chunk)
            except ValueError:
                continue
            for entry in doc.get("files", []):
                name = entry.get("file", "")
                if not name.startswith("src/"):
                    continue
                lines = cov.setdefault(name, {})
                for line in entry.get("lines", []):
                    number = str(line["line_number"])
                    lines[number] = max(lines.get(number, 0), int(line.get("count", 0)))
    with open(out_path, "w", encoding="utf-8") as handle:
        json.dump(cov, handle, sort_keys=True)
    total = sum(len(v) for v in cov.values())
    hit = sum(1 for v in cov.values() for c in v.values() if c > 0)
    print("collect: %d files, %d/%d instrumented lines executed" % (len(cov), hit, total))
    return 0


def ranges(numbers):
    out = []
    for n in sorted(numbers):
        if out and n <= out[-1][1] + 1:
            out[-1][1] = n
        else:
            out.append([n, n])
    return out


def source_line(root, name, number):
    try:
        with open(os.path.join(root, name), encoding="utf-8", errors="replace") as handle:
            for i, text in enumerate(handle, 1):
                if i == number:
                    return text.strip()[:90]
    except OSError:
        pass
    return ""


def report(fixtures_path, generated_path, top, min_gap, source_root, detail):
    with open(fixtures_path, encoding="utf-8") as handle:
        fixtures = json.load(handle)
    with open(generated_path, encoding="utf-8") as handle:
        generated = json.load(handle)
    rows = []
    totals = [0, 0, 0, 0]  # instrumented, fixtures, generated, neither
    for name in sorted(set(fixtures) | set(generated)):
        f = fixtures.get(name, {})
        g = generated.get(name, {})
        lines = set(f) | set(g)
        f_hit = {int(n) for n in lines if f.get(n, 0) > 0}
        g_hit = {int(n) for n in lines if g.get(n, 0) > 0}
        neither = len(lines) - len(f_hit | g_hit)
        only_f = f_hit - g_hit
        rows.append((len(only_f), name, len(lines), len(f_hit), len(g_hit), neither, only_f))
        totals[0] += len(lines)
        totals[1] += len(f_hit)
        totals[2] += len(g_hit)
        totals[3] += neither
    print("lines of src/ in the conformance library: %d" % totals[0])
    print("  executed by the committed fixtures: %d (%.1f%%)" % (totals[1], 100.0 * totals[1] / max(totals[0], 1)))
    print("  executed by the generated corpus:   %d (%.1f%%)" % (totals[2], 100.0 * totals[2] / max(totals[0], 1)))
    print("  executed by neither:                %d (%.1f%%)" % (totals[3], 100.0 * totals[3] / max(totals[0], 1)))
    print("  fixtures only (generator gaps):     %d" % sum(r[0] for r in rows))
    print()
    print("files by lines the fixtures execute and the generated corpus does not:")
    shown = 0
    for gap, name, n, fh, gh, neither, only_f in sorted(rows, key=lambda r: (-r[0], r[1])):
        if gap < min_gap or shown >= top:
            break
        shown += 1
        print("%5d  %-46s fixtures %5d  generated %5d  of %5d  neither %5d" % (gap, name, fh, gh, n, neither))
        for lo, hi in ranges(only_f)[:6]:
            span = "%d" % lo if lo == hi else "%d-%d" % (lo, hi)
            print("         %-11s %s" % (span, source_line(source_root, name, lo)))
        more = len(ranges(only_f)) - 6
        if more > 0:
            print("         ... %d more ranges" % more)
    print()
    never = sorted(rows, key=lambda r: -r[5])[:8]
    print("files with the most lines no corpus executes:")
    for gap, name, n, fh, gh, neither, _only in never:
        print("%5d  %-46s of %5d" % (neither, name, n))
    for wanted in detail:
        key = wanted if wanted.startswith("src/") else "src/" + wanted
        if key not in fixtures and key not in generated:
            print("\n%s: not instrumented" % key)
            continue
        f = fixtures.get(key, {})
        g = generated.get(key, {})
        lines = set(f) | set(g)
        missing = {int(n) for n in lines if f.get(n, 0) <= 0 and g.get(n, 0) <= 0}
        print("\n%s: %d of %d instrumented lines executed by neither" % (key, len(missing), len(lines)))
        for lo, hi in ranges(missing):
            span = "%d" % lo if lo == hi else "%d-%d" % (lo, hi)
            print("  %-11s %s" % (span, source_line(source_root, key, lo)))
    return 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("collect")
    c.add_argument("build_dir")
    c.add_argument("out")
    r = sub.add_parser("report")
    r.add_argument("fixtures")
    r.add_argument("generated")
    r.add_argument("--top", type=int, default=15)
    r.add_argument("--min-gap", type=int, default=5)
    r.add_argument("--source-root", default=".")
    r.add_argument(
        "--detail",
        action="append",
        default=[],
        help="a file whose lines no corpus executes are listed (repeatable)",
    )
    args = parser.parse_args(argv)
    if args.cmd == "collect":
        return collect(args.build_dir, args.out)
    return report(
        args.fixtures, args.generated, args.top, args.min_gap, args.source_root, args.detail
    )


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
