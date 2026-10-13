#!/bin/bash
# build_asan.sh — build libasx.a and the fuzz harnesses (fuzz_differential,
# fuzz_minimize) with ASan+UBSan into build/asan (bd-rkql.8; the input of
# tests/fuzz/test_harness_sanity.sh).
#
# The Makefile owns the source list, the flags and the profile: this is its
# fuzz-build and minimize-build under the sanitizer flags `make test-asan`
# uses. (The script used to compile four source directories by hand and
# stopped linking as soon as the library grew past them.)
set -euo pipefail

SRCROOT="$(cd "$(dirname "$0")" && pwd)"
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"

echo "[asan] Building libasx.a and the fuzz harnesses with ASan+UBSan..."
make -C "$SRCROOT" --no-print-directory BUILD_DIR=build/asan \
    CFLAGS="$SAN" LDFLAGS="$SAN" fuzz-build minimize-build
echo "[asan] Build complete. Binaries in build/asan/fuzz/"
