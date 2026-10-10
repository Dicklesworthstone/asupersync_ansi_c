#!/usr/bin/env python3
"""check_ambient_authority.py - ambient clock and entropy stay in their providers (bd-aack).

Deterministic replay needs every clock read and every random draw to go
through the runtime hooks (asx_runtime_hooks), which deterministic builds and
the lab replace with a virtual clock and a seeded PRNG. This check reads the
ambient-authority catalog of src/security/audit.c (s_known_findings: the files
exempted as providers) and its pristine-module list, then:

  * fails if a catalog or pristine path names a file that does not exist;
  * strips comments and string literals from every C file under src/ and
    include/ and fails if an ambient clock or entropy call appears in a file
    the catalog does not exempt.

Usage: tools/ci/check_ambient_authority.py   (from any directory)
Exit 0 on success, 1 on a finding.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
AUDIT = ROOT / "src/security/audit.c"

# Calls that read a clock or draw entropy from the host (matched in code with
# comments and string literals blanked).
AMBIENT_CALL = re.compile(
    r"\b(clock_gettime|gettimeofday|getrandom|arc4random\w*|QueryPerformanceCounter"
    r"|QueryPerformanceFrequency|GetTickCount64|GetTickCount|GetSystemTimeAsFileTime"
    r"|GetSystemTimePreciseAsFileTime|BCryptGenRandom|RtlGenRandom|rand_s|srand|rand"
    r"|time|clock|timespec_get|mach_absolute_time)\s*\("
)
# Host entropy devices (matched in code with only comments blanked: the path
# is a string literal).
AMBIENT_DEVICE = re.compile(r"/dev/u?random")


def catalog_paths(text: str, array: str) -> list[str]:
    """The "src/..." / "include/..." strings in the initializer of `array`."""
    m = re.search(re.escape(array) + r"\[\]\s*=\s*\{(.*?)\};", text, re.S)
    if m is None:
        sys.exit(f"check_ambient_authority: {array} not found in {AUDIT}")
    return re.findall(r'"((?:src|include)/[^"]+)"', m.group(1))


def strip_c(text: str, keep_strings: bool) -> str:
    """Blank out comments (and string/char literals unless keep_strings),
    keeping line breaks."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(ch if ch == "\n" else " " for ch in text[i:j]))
            i = j
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            if keep_strings:
                out.append(text[i:j])
            else:
                # Keep the quotes so a string never fuses two tokens.
                out.append(c + " " * (j - i - 2) + c if j - i >= 2 else c)
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def main() -> int:
    text = AUDIT.read_text()
    providers = catalog_paths(text, "s_known_findings")
    pristine = catalog_paths(text, "s_pristine_modules")
    status = 0

    for path in sorted(set(providers) | set(pristine)):
        if not (ROOT / path).is_file():
            print(f"check_ambient_authority: FAIL: {AUDIT.relative_to(ROOT)} names missing file {path}")
            status = 1

    exempt = set(providers)
    files = sorted(
        p for d in ("src", "include") for p in (ROOT / d).rglob("*") if p.suffix in (".c", ".h")
    )
    for path in files:
        rel = path.relative_to(ROOT).as_posix()
        if rel in exempt:
            continue
        text = path.read_text(errors="replace")
        code = strip_c(text, keep_strings=False).splitlines()
        with_strings = strip_c(text, keep_strings=True).splitlines()
        for lineno, (line, raw) in enumerate(zip(code, with_strings), 1):
            m = AMBIENT_CALL.search(line) or AMBIENT_DEVICE.search(raw)
            if m:
                print(
                    f"check_ambient_authority: FAIL: {rel}:{lineno}: ambient clock/entropy "
                    f"'{m.group(0).rstrip('(').strip()}' outside the providers cataloged in "
                    f"src/security/audit.c"
                )
                status = 1

    if status == 0:
        print(
            f"check_ambient_authority: PASS ({len(files)} files; providers: {', '.join(sorted(exempt))})"
        )
    return status


if __name__ == "__main__":
    sys.exit(main())
