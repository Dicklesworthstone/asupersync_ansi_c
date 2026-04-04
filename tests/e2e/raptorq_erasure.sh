#!/usr/bin/env bash
# raptorq_erasure.sh — E2E: RaptorQ erasure encode/decode recovery
# GATE-E2E-RAPTORQ
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD="${REPO_ROOT}/build"
BIN="${BUILD}/e2e/e2e_raptorq_erasure"

echo "[GATE-E2E-RAPTORQ] building..."
mkdir -p "${BUILD}/e2e"
gcc -std=c99 -Wall -Wextra -Wpedantic -Werror -Wno-unused-parameter \
    -O0 -g -DASX_DEBUG=1 \
    -I"${REPO_ROOT}/include" -DASX_PROFILE_CORE -DASX_CODEC_JSON -DASX_DETERMINISTIC=1 \
    -o "$BIN" \
    "${SCRIPT_DIR}/e2e_raptorq_erasure.c" \
    "${BUILD}/lib/libasx.a"

echo "[GATE-E2E-RAPTORQ] running..."
if "$BIN" 2>&1; then
    echo "[GATE-E2E-RAPTORQ] PASS"
    exit 0
else
    echo "[GATE-E2E-RAPTORQ] FAIL"
    exit 1
fi
