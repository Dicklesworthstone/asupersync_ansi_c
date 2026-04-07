#!/bin/sh
# count_inventory.sh — Authoritative inventory counting script
#
# This script defines THE canonical counting method for all inventory
# metrics referenced in README.md badges and narrative text.
#
# Counting rules:
#   - Source files: .c files under src/ (recursive)
#   - Public headers: .h files under include/asx/ (recursive)
#   - Header families: subdirectories of include/asx/ containing .h files
#   - ASX_API declarations: lines matching ASX_API in public headers
#   - Test programs: .c files in each tests/ subdirectory (unit, e2e, invariant,
#     vignettes, conformance, fuzz, formal). E2E .sh scripts are harness/driver
#     scripts, not counted as "test programs" in the badge.
#   - Examples: .c files under examples/
#
# SPDX-License-Identifier: MIT

set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

src_count=$(find "$ROOT/src" -name '*.c' | wc -l | tr -d ' ')
hdr_count=$(find "$ROOT/include/asx" -name '*.h' | wc -l | tr -d ' ')
hdr_families=$(find "$ROOT/include/asx" -mindepth 1 -type d | wc -l | tr -d ' ')
api_count=$(grep -r 'ASX_API' "$ROOT/include/" --include='*.h' 2>/dev/null | wc -l | tr -d ' ')

unit=$(find "$ROOT/tests/unit" -name '*.c' | wc -l | tr -d ' ')
e2e_c=$(find "$ROOT/tests/e2e" -name '*.c' | wc -l | tr -d ' ')
invariant=$(find "$ROOT/tests/invariant" -name '*.c' | wc -l | tr -d ' ')
vignettes=$(find "$ROOT/tests/vignettes" -name '*.c' | wc -l | tr -d ' ')
conformance=$(find "$ROOT/tests/conformance" -name '*.c' | wc -l | tr -d ' ')
fuzz=$(find "$ROOT/tests/fuzz" -name '*.c' | wc -l | tr -d ' ')
formal=$(find "$ROOT/tests/formal" -name '*.c' | wc -l | tr -d ' ')
test_total=$((unit + e2e_c + invariant + vignettes + conformance + fuzz + formal))

examples=$(find "$ROOT/examples" -name '*.c' | wc -l | tr -d ' ')

printf "=== asx inventory (canonical counts) ===\n"
printf "Source files:        %s\n" "$src_count"
printf "Public headers:      %s\n" "$hdr_count"
printf "Header families:     %s\n" "$hdr_families"
printf "ASX_API declarations:%s\n" "$api_count"
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
