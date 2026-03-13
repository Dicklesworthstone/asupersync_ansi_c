#!/bin/bash
set -euo pipefail

# Build libasx.a and fuzz harnesses with ASan+UBSan
# Used by bd-rkql.8 for sanitizer validation

GCC=/usr/bin/gcc
OUTDIR=build_asan
SRCROOT="$(cd "$(dirname "$0")" && pwd)"

mkdir -p "$OUTDIR/lib" "$OUTDIR/fuzz"

COMMON_FLAGS="-std=c99 -Wall -Wextra -Wpedantic -Wno-unused-parameter -O0 -g -DASX_DEBUG=1"
INCLUDES="-I$SRCROOT/include -DASX_PROFILE_CORE -DASX_CODEC_JSON -DASX_DETERMINISTIC=1"
SANITIZERS="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"

echo "[asan] Building libasx.a with ASan+UBSan..."

# Compile all source files
for src in src/core/*.c src/runtime/*.c src/channel/*.c src/time/*.c; do
    obj="$OUTDIR/obj/$(echo "$src" | sed 's|^src/||;s|\.c$|.o|')"
    mkdir -p "$(dirname "$obj")"
    $GCC $COMMON_FLAGS $INCLUDES $SANITIZERS -c -o "$obj" "$src"
done

OBJ_COUNT=$(find "$OUTDIR/obj" -name '*.o' | wc -l)
echo "[asan] Compiled $OBJ_COUNT object files"

# Create archive
ar rcs "$OUTDIR/lib/libasx.a" \
    "$OUTDIR"/obj/core/*.o \
    "$OUTDIR"/obj/runtime/*.o \
    "$OUTDIR"/obj/channel/*.o \
    "$OUTDIR"/obj/time/*.o
echo "[asan] Archive created: $(wc -c < "$OUTDIR/lib/libasx.a") bytes"

# Build fuzz_differential
FUZZ_FLAGS="-Wno-unused-result -Wno-conversion -Wno-sign-conversion"
FUZZ_INCLUDES="-I$SRCROOT/tests -I$SRCROOT/src"

$GCC $COMMON_FLAGS $FUZZ_FLAGS $INCLUDES $FUZZ_INCLUDES $SANITIZERS \
    -o "$OUTDIR/fuzz/fuzz_differential" \
    tests/fuzz/fuzz_differential.c "$OUTDIR/lib/libasx.a"
echo "[asan] Built fuzz_differential"

# Build fuzz_minimize
$GCC $COMMON_FLAGS $FUZZ_FLAGS $INCLUDES $FUZZ_INCLUDES $SANITIZERS \
    -o "$OUTDIR/fuzz/fuzz_minimize" \
    tests/fuzz/fuzz_minimize.c "$OUTDIR/lib/libasx.a"
echo "[asan] Built fuzz_minimize"

echo "[asan] Build complete. All binaries in $OUTDIR/"
