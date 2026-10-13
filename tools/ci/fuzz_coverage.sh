#!/usr/bin/env bash
# =============================================================================
# fuzz_coverage.sh - make fuzz-coverage (bd-9kll.2.10, G4)
#
# Which lines of the C runtime the committed Rust fixtures execute and the
# generated corpus of make fuzz-differential does not: the parts of the DSL
# the generator does not draw, or draws in the wrong shape. The conformance
# runner is built with gcc --coverage into its own BUILD_DIR (build/ is not
# touched), run once over fixtures/rust_reference_v2 and once over the
# captured fixtures of every `make fuzz-differential` run under
# build/fuzz_v2/seed-*/fixtures, and the two gcov results are compared by
# tools/ci/fuzz_coverage.py.
#
# Environment:
#   FUZZ_COV_DIR     BUILD_DIR of the instrumented build (default build/fuzz_coverage)
#   FUZZ_COV_CORPUS  glob of generated fixture directories
#                    (default build/fuzz_v2/seed-*/fixtures)
#   FUZZ_COV_TOP     files shown (default 15)
#   FUZZ_COV_DETAIL  space-separated files (src/...) whose lines no corpus
#                    executes are listed with their source text
#
# Exit 0 with the report; 2 when there is no generated corpus.
#
# SPDX-License-Identifier: MIT
# =============================================================================
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/../.."
COV="${FUZZ_COV_DIR:-build/fuzz_coverage}"
CORPUS_GLOB="${FUZZ_COV_CORPUS:-build/fuzz_v2/seed-*/fixtures}"
TOP="${FUZZ_COV_TOP:-15}"
PY="${PYTHON:-python3}"

dirs=()
for d in $CORPUS_GLOB; do
    [ -d "$d" ] && dirs+=("$d")
done
if [ "${#dirs[@]}" -eq 0 ]; then
    echo "[asx] fuzz-coverage: no generated corpus under $CORPUS_GLOB; run make fuzz-differential FUZZ_V2_SEED=<n> first" >&2
    exit 2
fi

if ! command -v gcov >/dev/null 2>&1; then
    echo "[asx] fuzz-coverage: gcov not found (gcc --coverage needs it)" >&2
    exit 2
fi

echo "[asx] fuzz-coverage: instrumented runner in $COV"
make --no-print-directory BUILD_DIR="$COV" CFLAGS='-O0 -g --coverage' LDFLAGS='--coverage' \
    "$COV/bin/asx-conformance" > "$COV.build.log" 2>&1 || {
    tail -20 "$COV.build.log" >&2
    echo "[asx] fuzz-coverage: instrumented build failed" >&2
    exit 1
}
RUNNER="$COV/bin/asx-conformance"

measure() { # label dir...
    local label=$1
    shift
    find "$COV" -name '*.gcda' -delete
    local total=0
    for d in "$@"; do
        local files
        files=$(ls "$d"/*.json 2>/dev/null || true)
        [ -n "$files" ] || continue
        # shellcheck disable=SC2086
        "$RUNNER" compare $files > "$COV/$label.compare.log" 2>&1 || true
        total=$((total + $(grep -c '^PASS ' "$COV/$label.compare.log" || true)))
    done
    echo "[asx] fuzz-coverage: $label: $total scenarios passed in the instrumented run"
    "$PY" tools/ci/fuzz_coverage.py collect "$COV" "$COV/$label.json"
}

measure fixtures fixtures/rust_reference_v2
measure generated "${dirs[@]}"

detail=()
for f in ${FUZZ_COV_DETAIL:-}; do
    detail+=(--detail "$f")
done

echo
"$PY" tools/ci/fuzz_coverage.py report "$COV/fixtures.json" "$COV/generated.json" \
    --top "$TOP" --source-root . ${detail[@]+"${detail[@]}"}
