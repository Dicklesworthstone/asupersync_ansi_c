#!/usr/bin/env bash
# test_e2e_suite_registry.sh — keep canonical e2e suite wiring aligned
#
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

PASS=0
FAIL=0

assert_eq() {
    local label="$1" expected="$2" actual="$3"
    if [ "$expected" = "$actual" ]; then
        echo "  PASS: $label"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $label (expected='$expected' actual='$actual')"
        FAIL=$((FAIL + 1))
    fi
}

assert_contains() {
    local label="$1" haystack="$2" needle="$3"
    if echo "$haystack" | command grep -qF "$needle"; then
        echo "  PASS: $label"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $label (missing '$needle')"
        FAIL=$((FAIL + 1))
    fi
}

collect_make_scripts() {
    python3 - <<'PY' "$PROJECT_ROOT/Makefile"
import sys
from pathlib import Path

lines = Path(sys.argv[1]).read_text(encoding="utf-8").splitlines()
body = []
collect = False
for line in lines:
    if line.startswith("E2E_ALL_SCRIPTS :="):
        collect = True
        continue
    if collect and not line.startswith("\t"):
        break
    if collect:
        body.append(line)

if not body:
    raise SystemExit("missing E2E_ALL_SCRIPTS block")

scripts = []
for line in body:
    line = line.strip().rstrip("\\").strip()
    if not line:
        continue
    scripts.append(Path(line).name)
for name in scripts:
    print(name)
PY
}

collect_suite_scripts() {
    python3 - <<'PY' "$PROJECT_ROOT/tests/e2e/run_all.sh"
import re
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text(encoding="utf-8")
match = re.search(r"E2E_FAMILIES=\(\n(?P<body>.*?)\n\)", text, re.S)
if not match:
    raise SystemExit("missing E2E_FAMILIES block")
scripts = re.findall(r':[A-Za-z0-9_./-]+\.sh"', match.group("body"))
for item in scripts:
    print(Path(item[1:-1]).name)
PY
}

collect_doc_scripts() {
    python3 - <<'PY' "$PROJECT_ROOT/tests/TEST.md"
import re
import sys
from pathlib import Path

lines = Path(sys.argv[1]).read_text(encoding="utf-8").splitlines()
collect = False
for line in lines:
    if line.startswith("| Family | Gate | Script |"):
        collect = True
        continue
    if collect and not line.startswith("|"):
        break
    if collect and line.startswith("|"):
        cols = [c.strip() for c in line.split("|")[1:-1]]
        if len(cols) >= 3 and cols[2].startswith("`") and cols[2].endswith("`"):
            print(cols[2].strip("`"))
PY
}

echo "=== test_e2e_suite_registry ==="

make_scripts="$(collect_make_scripts)"
suite_scripts="$(collect_suite_scripts)"
doc_scripts="$(collect_doc_scripts)"
make_test_e2e="$(make -n test-e2e)"

assert_eq "suite script count matches Makefile" \
    "$(echo "$make_scripts" | wc -l | tr -d ' ')" \
    "$(echo "$suite_scripts" | wc -l | tr -d ' ')"

missing_from_suite="$(comm -23 <(printf '%s\n' "$make_scripts" | sort) <(printf '%s\n' "$suite_scripts" | sort) || true)"
assert_eq "no Makefile e2e scripts are missing from canonical suite" "" "$missing_from_suite"

missing_from_docs="$(comm -23 <(printf '%s\n' "$doc_scripts" | sort) <(printf '%s\n' "$suite_scripts" | sort) || true)"
assert_eq "documented e2e scripts are present in canonical suite" "" "$missing_from_docs"

assert_contains "test-e2e delegates through run_all.sh" "$make_test_e2e" "tests/e2e/run_all.sh"

echo ""
echo "$((PASS + FAIL))/$((PASS + FAIL)) tests: ${PASS} passed, ${FAIL} failed"

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
