#!/bin/bash
# test_cross_determinism.sh — Verify C and Rust fuzz grammars produce identical scenarios
#
# Generates scenarios from both C and Rust for seeds 0..N-1, compares op sequences.
# Exit 0 if all match, exit 1 on first mismatch.
#
# Usage: tests/fuzz/test_cross_determinism.sh [num_seeds]
# Default: 1000 seeds
#
# Bead: bd-rkql.6

set -euo pipefail

NUM_SEEDS="${1:-1000}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
C_GEN="$SCRIPT_DIR/generate_grammar_vectors_jsonl"
RUST_GEN="$PROJECT_ROOT/tools/rust_fuzz_target/target/release/rust_fuzz_target"

TMPDIR_WORK="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_WORK"' EXIT

C_OUT="$TMPDIR_WORK/c_vectors.jsonl"
RUST_OUT="$TMPDIR_WORK/rust_vectors.jsonl"

# --- Build C generator (JSONL mode) ---
echo "[cross-det] Building C JSONL generator..."
/usr/bin/gcc -std=c99 -Wall -Wextra -Werror -pedantic \
    -o "$C_GEN" "$SCRIPT_DIR/generate_grammar_vectors_jsonl.c" 2>&1

# --- Build Rust generator ---
echo "[cross-det] Building Rust fuzz target..."
(cd "$PROJECT_ROOT/tools/rust_fuzz_target" && cargo build --release --quiet 2>&1)

# --- Generate C vectors ---
echo "[cross-det] Generating C vectors for seeds 0..$((NUM_SEEDS - 1))..."
"$C_GEN" "$NUM_SEEDS" > "$C_OUT"

# --- Generate Rust vectors ---
echo "[cross-det] Generating Rust vectors for seeds 0..$((NUM_SEEDS - 1))..."
"$RUST_GEN" --verify-grammar-batch "$NUM_SEEDS" > "$RUST_OUT"

# --- Compare ---
C_LINES=$(wc -l < "$C_OUT")
RUST_LINES=$(wc -l < "$RUST_OUT")

if [ "$C_LINES" != "$RUST_LINES" ]; then
    echo "[cross-det] FAIL: line count mismatch: C=$C_LINES Rust=$RUST_LINES"
    exit 1
fi

if diff -q "$C_OUT" "$RUST_OUT" > /dev/null 2>&1; then
    echo "[cross-det] PASS: $NUM_SEEDS seeds — all scenarios identical"
    exit 0
else
    echo "[cross-det] FAIL: differences found"
    diff --unified=0 "$C_OUT" "$RUST_OUT" | head -40
    exit 1
fi
