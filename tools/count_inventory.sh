#!/bin/sh
# count_inventory.sh — Authoritative inventory counting script
#
# This script defines THE canonical counting method for all inventory
# metrics referenced in README.md badges and narrative text. The README marks
# each such number <!-- fact:KEY -->N<!-- /fact --> and
# tools/ci/check_readme_facts.sh (make lint-docs) fails when one differs.
#
# Counting rules:
#   - Source files: .c files under src/ (recursive); per directory too
#   - Public headers: .h files under include/asx/ (recursive); per directory
#   - Header families: subdirectories of include/asx/ containing .h files
#   - ASX_API declarations: distinct names declared ASX_API in public headers
#     (comments and preprocessor lines excluded)
#   - Status codes: the ASX_E_* codes of asx_status.h
#   - Test programs: .c files in each tests/ subdirectory (unit, e2e, invariant,
#     vignettes, conformance, fuzz, formal). E2E .sh scripts are harness/driver
#     scripts, not counted as "test programs" in the badge.
#   - Examples: .c files under examples/
#   - CI jobs: top-level jobs of .github/workflows/ci.yml
#
# Usage: tools/count_inventory.sh [--facts]
#   --facts prints KEY=VALUE lines (the keys the README's markers use).
#
# SPDX-License-Identifier: MIT

set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

count_c() { find "$1" -name '*.c' | wc -l | tr -d ' '; }
count_h() { find "$1" -name '*.h' | wc -l | tr -d ' '; }

src_count=$(count_c "$ROOT/src")
hdr_count=$(count_h "$ROOT/include/asx")
hdr_families=$(find "$ROOT/include/asx" -mindepth 1 -type d | wc -l | tr -d ' ')
api_count=$(python3 - "$ROOT/include" <<'EOF'
import pathlib, re, sys
names = set()
for path in pathlib.Path(sys.argv[1]).rglob("*.h"):
    text = path.read_text()
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    text = re.sub(r"^[ \t]*#[^\n]*(\\\n[^\n]*)*", "", text, flags=re.M)
    for m in re.finditer(r"\bASX_API\b[^;{}]*?\b([A-Za-z_]\w*)\s*\(", text):
        names.add(m.group(1))
print(len(names))
EOF
)
status_codes=$(grep -cE '^[[:space:]]*ASX_E_[A-Z0-9_]+[[:space:]]*=' "$ROOT/include/asx/asx_status.h")

unit=$(count_c "$ROOT/tests/unit")
e2e_c=$(count_c "$ROOT/tests/e2e")
invariant=$(count_c "$ROOT/tests/invariant")
vignettes=$(count_c "$ROOT/tests/vignettes")
conformance=$(count_c "$ROOT/tests/conformance")
fuzz=$(count_c "$ROOT/tests/fuzz")
formal=$(count_c "$ROOT/tests/formal")
test_total=$((unit + e2e_c + invariant + vignettes + conformance + fuzz + formal))

examples=$(count_c "$ROOT/examples")
# Rust-captured fixtures `make conformance` executes (twin_run captures).
rust_fixtures=$(find "$ROOT/fixtures/rust_reference_v2" -maxdepth 1 -name '*.json' | wc -l | tr -d ' ')
ci_jobs=$(awk '/^jobs:/ { in_jobs = 1; next }
    in_jobs && /^[^ #]/ { in_jobs = 0 }
    in_jobs && /^  [A-Za-z0-9_-]+:[ \t]*$/ { n++ }
    END { print n + 0 }' "$ROOT/.github/workflows/ci.yml")

if [ "${1:-}" = "--facts" ]; then
    printf 'api_declarations=%s\n' "$api_count"
    printf 'header_files=%s\n' "$hdr_count"
    printf 'header_families=%s\n' "$hdr_families"
    printf 'source_files=%s\n' "$src_count"
    printf 'status_codes=%s\n' "$status_codes"
    printf 'test_programs=%s\n' "$test_total"
    printf 'test_unit=%s\n' "$unit"
    printf 'test_e2e=%s\n' "$e2e_c"
    printf 'test_invariant=%s\n' "$invariant"
    printf 'test_vignettes=%s\n' "$vignettes"
    printf 'test_conformance=%s\n' "$conformance"
    printf 'test_fuzz=%s\n' "$fuzz"
    printf 'test_formal=%s\n' "$formal"
    printf 'examples=%s\n' "$examples"
    printf 'rust_fixtures=%s\n' "$rust_fixtures"
    printf 'ci_jobs=%s\n' "$ci_jobs"
    for d in "$ROOT"/include/asx/*/; do
        printf 'headers_%s=%s\n' "$(basename "$d")" "$(count_h "$d")"
    done
    for d in "$ROOT"/src/*/; do
        printf 'sources_%s=%s\n' "$(basename "$d")" "$(count_c "$d")"
    done
    # Resource-class limits, e.g. class_r1_max_regions=4 (asx_config.h).
    sed -nE 's/^#define ASX_CLASS_(R[123])_(MAX_[A-Z_]+) ([0-9]+)u?$/class_\1_\2=\3/p' \
        "$ROOT/include/asx/asx_config.h" | tr 'A-Z' 'a-z'
    exit 0
fi

printf "=== asx inventory (canonical counts) ===\n"
printf "Source files:        %s\n" "$src_count"
printf "Public headers:      %s\n" "$hdr_count"
printf "Header families:     %s\n" "$hdr_families"
printf "ASX_API declarations:%s\n" "$api_count"
printf "Status codes:        %s\n" "$status_codes"
printf "\n"
printf "Test programs (C):   %s total\n" "$test_total"
printf "  unit:              %s\n" "$unit"
printf "  e2e (C drivers):   %s\n" "$e2e_c"
printf "  invariant:         %s\n" "$invariant"
printf "  vignettes:         %s\n" "$vignettes"
printf "  conformance:       %s\n" "$conformance"
printf "  fuzz:              %s\n" "$fuzz"
printf "  formal:            %s\n" "$formal"
printf "\n"
printf "Examples:            %s\n" "$examples"
printf "CI jobs:             %s\n" "$ci_jobs"
