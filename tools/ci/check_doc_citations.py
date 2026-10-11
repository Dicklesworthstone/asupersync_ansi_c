#!/usr/bin/env python3
"""check_doc_citations.py - the evidence the parity docs cite exists (bd-9kll.1.8).

Usage: tools/ci/check_doc_citations.py [DOC.md ...]
  default: the docs the 2026-10-10 truth pass re-derived from the code.

The truth pass found docs citing fixtures and code that did not exist (66
of the 68 fixture IDs the provenance map listed as mapped). For each doc,
every backticked repository path, bare or as `path:N` / `path:N-M`, must
name a file (or, bare, a directory) that exists, and the cited lines must
lie within it. Paths under src/ that this repository lacks and that end in
.rs or have no extension are the Rust reference's files and modules, and
are skipped, as are bare file names.

A citation tied to a C symbol, "`sym` (`path:N`)" or "`path:N` `sym`",
must cite lines that mention the symbol whenever the cited file mentions
it at all (bd-r3o5): line numbers drift as code moves, and a range that
merely exists proves nothing. The error names the line that defines the
symbol, or its only occurrence, when there is one. (A backticked word the
file never mentions, such as a vocabulary term, is not a symbol here.)

docs/FEATURE_PARITY.md is also checked as the parity tracker: every
backticked fixture ID (`name-NNN`) must be a v2 fixture
(fixtures/rust_reference_v2/<id>.json) or a legacy one (fixtures/
rust_reference/**/<id>.*.json), and every matrix row whose status is
`conformance-passed` must name at least one v2 fixture (or claim every v2
fixture).

Exit 0 when every citation resolves, 1 otherwise.

SPDX-License-Identifier: MIT
"""

import glob
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
DEFAULT_DOCS = [
    "docs/FEATURE_PARITY.md",
    "docs/SOURCE_TO_FIXTURE_PROVENANCE_MAP.md",
    "docs/DEFERRED_STUBS_REGISTER.md",
    "docs/CHANNEL_TIMER_SEMANTICS.md",
    "docs/RUST_FIXTURE_CAPTURE_TOOLING.md",
    "docs/QUIESCENCE_FINALIZATION_INVARIANTS.md",
    "docs/CHANNEL_TIMER_DETERMINISM.md",
    "docs/CHANNEL_TIMER_KERNEL_SEMANTICS.md",
    "docs/EXISTING_ASUPERSYNC_STRUCTURE.md",
    "docs/LIFECYCLE_TRANSITION_TABLES.md",
]
# Checked for symbol-tied citations only: its Rust column cites the Rust
# repository's docs/ and tests/ by bare path, which check_paths would take
# for this repository's (check_refinement_map.sh checks its rule IDs).
SYMBOL_ONLY_DOCS = ["docs/C_REFINEMENT_MAP.md"]
REPO_DIRS = ("src/", "include/", "tests/", "tools/", "docs/", "schemas/", "fixtures/", ".github/")
PATH_RE = re.compile(r"`((?:[A-Za-z0-9_.-]+/)+[A-Za-z0-9_.-]+|Makefile)(?::([0-9]+)(?:-([0-9]+))?)?`")
FIXTURE_RE = re.compile(r"`([a-z0-9]+(?:-[a-z0-9]+)*-[0-9]{3})`")
V2_DIR = os.path.join(ROOT, "fixtures", "rust_reference_v2")

# Symbol-tied citations of C files: "`sym` (`path:N[-M]`" and "`path:N[-M]` `sym`".
_SYM = r"([A-Za-z_][A-Za-z0-9_]{2,})"
_CITE = r"((?:[A-Za-z0-9_.-]+/)+[A-Za-z0-9_.-]+\.[ch]):([0-9]+)(?:-([0-9]+))?"
SYM_BEFORE_RE = re.compile(r"`" + _SYM + r"`,? \(`" + _CITE + r"`")
SYM_AFTER_RE = re.compile(r"`" + _CITE + r"` `" + _SYM + r"`")
TOKEN_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
# Each captures the identifier a line defines.
DEFINES_RE = (
    re.compile(r"^[A-Za-z_][^;=(]*?\b([A-Za-z_][A-Za-z0-9_]*)\s*\("),  # function
    re.compile(r"^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)"),  # macro
    re.compile(r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*(?:=[^=]|,|$)"),  # enum constant
    re.compile(r"^\s*}\s*([A-Za-z_][A-Za-z0-9_]*)\s*;"),  # typedef name
)


def line_count(path):
    with open(path, "rb") as f:
        return sum(1 for _ in f)


def source_lines(path, cache):
    """The file's lines and, per line, the identifiers it mentions."""
    if path not in cache:
        with open(path, encoding="utf-8", errors="replace") as f:
            text = f.read().splitlines()
        cache[path] = ([set(TOKEN_RE.findall(line)) for line in text], text)
    return cache[path]


def defines(line, sym):
    for pattern in DEFINES_RE:
        m = pattern.search(line)
        if m and m.group(1) == sym:
            return True
    return False


def symbol_home(tokens, text, sym):
    """The line that defines `sym`, else its only occurrence, else None."""
    hits = [i + 1 for i, toks in enumerate(tokens) if sym in toks]
    defs = [h for h in hits if defines(text[h - 1], sym)]
    if len(defs) == 1:
        return defs[0]
    return hits[0] if len(hits) == 1 else None


def check_symbols(doc, text, errors):
    cache = {}
    for lineno, line in enumerate(text.splitlines(), 1):
        tied = [(m.group(1), m.group(2), m.group(3), m.group(4)) for m in SYM_BEFORE_RE.finditer(line)]
        tied += [(m.group(4), m.group(1), m.group(2), m.group(3)) for m in SYM_AFTER_RE.finditer(line)]
        for sym, rel, first, last in tied:
            path = os.path.join(ROOT, rel)
            if not os.path.isfile(path):
                continue  # reported by check_paths, or a Rust file
            tokens, src = source_lines(path, cache)
            if not any(sym in toks for toks in tokens):
                continue  # not a symbol of that file
            lo, hi = int(first), int(last) if last is not None else int(first)
            if any(sym in tokens[i - 1] for i in range(max(lo, 1), min(hi, len(tokens)) + 1)):
                continue
            home = symbol_home(tokens, src, sym)
            hint = f"; it is at {rel}:{home}" if home is not None else ""
            errors.append(f"{doc}:{lineno}: cites {rel}:{first}{'-' + last if last else ''} for "
                          f"`{sym}`, which those lines do not mention{hint}")


def check_paths(doc, text, errors):
    counts = {}
    for lineno, line in enumerate(text.splitlines(), 1):
        for m in PATH_RE.finditer(line):
            rel, first, last = m.group(1), m.group(2), m.group(3)
            if rel != "Makefile" and not rel.startswith(REPO_DIRS):
                continue
            path = os.path.join(ROOT, rel)
            if rel.startswith("src/") and not os.path.exists(path):
                base = os.path.basename(rel)
                if rel.endswith(".rs") or "." not in base:
                    continue  # a Rust reference file or module directory
            if first is None and os.path.isdir(path):
                continue
            if not os.path.isfile(path):
                errors.append(f"{doc}:{lineno}: cites {rel}, which does not exist")
                continue
            if first is None:
                continue
            if path not in counts:
                counts[path] = line_count(path)
            hi = int(last) if last is not None else int(first)
            if int(first) < 1 or hi < int(first) or hi > counts[path]:
                errors.append(
                    f"{doc}:{lineno}: cites {rel}:{first}{'-' + last if last else ''},"
                    f" but the file has {counts[path]} lines"
                )


def fixture_exists(fid):
    if os.path.isfile(os.path.join(V2_DIR, fid + ".json")):
        return "v2"
    legacy = os.path.join(ROOT, "fixtures", "rust_reference", "**", fid + ".*.json")
    if glob.glob(legacy, recursive=True):
        return "legacy"
    return None


def check_parity_tracker(doc, text, errors):
    for lineno, line in enumerate(text.splitlines(), 1):
        kinds = []
        for m in FIXTURE_RE.finditer(line):
            kind = fixture_exists(m.group(1))
            if kind is None:
                errors.append(f"{doc}:{lineno}: cites fixture {m.group(1)}, which does not exist")
            kinds.append(kind)
        cells = [c.strip() for c in line.split("|")]
        if line.startswith("| `U-") and len(cells) > 6 and cells[6].startswith("`conformance-passed`"):
            if "v2" not in kinds and "every v2 fixture" not in cells[6]:
                errors.append(f"{doc}:{lineno}: a conformance-passed row names no v2 fixture")


def main(argv):
    docs = argv[1:] or DEFAULT_DOCS + SYMBOL_ONLY_DOCS
    errors = []
    for doc in docs:
        with open(os.path.join(ROOT, doc), encoding="utf-8") as f:
            text = f.read()
        if doc not in SYMBOL_ONLY_DOCS:
            check_paths(doc, text, errors)
        check_symbols(doc, text, errors)
        if os.path.basename(doc) == "FEATURE_PARITY.md":
            check_parity_tracker(doc, text, errors)
    for e in errors:
        print(f"check_doc_citations: {e}", file=sys.stderr)
    if errors:
        print(f"check_doc_citations: FAIL ({len(errors)} unresolved citation(s))", file=sys.stderr)
        return 1
    print(f"check_doc_citations: {len(docs)} doc(s), every cited path, line range, symbol and "
          f"fixture resolves")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
