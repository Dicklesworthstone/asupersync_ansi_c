#!/bin/sh
# proof.sh -- link one bare-metal proof image, report its footprint and run
# it under qemu-system (make test-baremetal-qemu, bd-9kll.15.7).
#
# The library is built by the Makefile (cross-baremetal-* targets, one build
# directory per resource class); this takes the archive, the generated
# fixtures file (embed_fixtures.sh) and the harness, links them with the
# target's startup and linker script, and writes into the output directory:
#
#   image.elf   the linked image
#   size.txt    `size` of it: text, data, bss (Berkeley format) and, by
#               symbol, the largest objects in RAM and in flash (nm -S)
#   qemu.txt    what the image printed through semihosting
#   qemu.rc     qemu's exit status (the image's: failed fixtures, or 70 on a
#               fault; 124 when the time limit ended it)
#
# Usage:
#   proof.sh --arch arm-m4|riscv32 --lib LIBASX.A --fixtures FIXTURES.C
#            --out DIR --cflags "FLAGS" --qemu QEMU [--timeout SECONDS]
#
# --cflags are the library's own compile flags (profile, class, JSON
# capacity, codec, deterministic), so the harness agrees with the library on
# every structure size.
#
# SPDX-License-Identifier: MIT

set -eu

arch=
lib=
fixtures=
out=
cflags=
qemu=
limit=600
ld_script=
while [ $# -gt 0 ]; do
    case $1 in
    --arch) arch=$2 ;;
    --lib) lib=$2 ;;
    --fixtures) fixtures=$2 ;;
    --out) out=$2 ;;
    --cflags) cflags=$2 ;;
    --qemu) qemu=$2 ;;
    --timeout) limit=$2 ;;
    --ld) ld_script=$2 ;;
    *) echo "proof.sh: unknown option $1" >&2; exit 2 ;;
    esac
    shift 2
done
for v in arch lib fixtures out qemu; do
    eval "val=\${$v}"
    [ -n "$val" ] || { echo "proof.sh: --$v is required" >&2; exit 2; }
done

here=$(dirname "$0")
case $arch in
arm-m4)
    cc=arm-none-eabi-gcc
    size=arm-none-eabi-size
    nm=arm-none-eabi-nm
    target_flags="-mcpu=cortex-m4 -mthumb"
    compile_specs=
    startup="$here/bm_arm_m4.c"
    link_flags="-nostartfiles -T ${ld_script:-$here/arm_m4.ld}"
    qemu_args="-M mps2-an386 -cpu cortex-m4"
    ;;
riscv32)
    cc=riscv64-unknown-elf-gcc
    size=riscv64-unknown-elf-size
    nm=riscv64-unknown-elf-nm
    target_flags="-march=rv32imac -mabi=ilp32"
    compile_specs="--specs=picolibc.specs"
    startup="$here/bm_riscv32.c"
    # picolibc's start file and linker script with its memory map: 8 MiB of
    # "flash" (code, constants, .data's image) then 16 MiB of RAM, inside
    # the 64 MiB virt gives.
    link_flags="--specs=picolibc.specs --oslib=semihost -Wl,--defsym=__flash=0x80000000 -Wl,--defsym=__flash_size=8M -Wl,--defsym=__ram=0x80800000 -Wl,--defsym=__ram_size=16M -Wl,--defsym=__stack_size=32K"
    qemu_args="-M virt -m 64M -bios none"
    ;;
*) echo "proof.sh: --arch must be arm-m4 or riscv32" >&2; exit 2 ;;
esac

mkdir -p "$out"
inc="-I$here/../../include -I$here/../../src"
# The escaped quotes are part of the macro's value (-DBM_TARGET_NAME="arm-m4"):
# the word splitting of $common below keeps them literal on purpose.
# shellcheck disable=SC2089,SC2090
common="$target_flags $compile_specs -std=c99 -O2 -DNDEBUG -Wall -Wextra -Wpedantic -Werror -ffunction-sections -fdata-sections $inc -DBM_TARGET_NAME=\"$arch\" $cflags"

# shellcheck disable=SC2086 # the flag strings are word lists by design
$cc $common -c -o "$out/bm_main.o" "$here/bm_main.c"
# shellcheck disable=SC2086
$cc $common -c -o "$out/bm_startup.o" "$startup"
# The generated file is data: no library flags matter, and it is not held to
# -Wpedantic's complaints about trailing commas in a C89 mode.
# shellcheck disable=SC2086
$cc $target_flags $compile_specs -std=c99 -O2 -ffunction-sections -fdata-sections -c -o "$out/bm_fixtures.o" "$fixtures"

# shellcheck disable=SC2086
$cc $target_flags $link_flags -Wl,--gc-sections -Wl,-Map="$out/image.map" \
    -o "$out/image.elf" "$out/bm_main.o" "$out/bm_startup.o" "$out/bm_fixtures.o" \
    "$lib" -Wl,--start-group -lc -lgcc -Wl,--end-group

{
    echo "# $arch: text = code + constants (flash), data = initialized RAM (its image is in flash), bss = zeroed RAM"
    echo "# (bss includes the stack and heap reservations of the linker script)"
    $size "$out/image.elf"
    echo ""
    echo "# by origin, from the linker map (bytes; flash = text + data):"
    echo "#   kernel = libasx.a without the conformance interpreter, harness = interpreter,"
    echo "#   JSON, canonical digest, embedded fixtures and start-up, libc = C library, libgcc"
    awk '
    function hex(s,   i, c, v, d) {
        v = 0; s = tolower(s); sub(/^0x/, "", s)
        for (i = 1; i <= length(s); i++) {
            c = substr(s, i, 1); d = index("0123456789abcdef", c) - 1; v = v * 16 + d
        }
        return v
    }
    /^Linker script and memory map/ { inmap = 1; next }
    !inmap { next }
    {
        line = $0; size = -1
        if (match(line, /^ \.[A-Za-z0-9_.$]+/)) {
            name = substr(line, RSTART + 1, RLENGTH - 1)
            n = split(line, f, " ")
            if (n >= 4) { size = hex(f[3]); file = f[4] } else { pending = name; next }
        } else if (pending != "" && match(line, /^ +0x[0-9a-fA-F]+ +0x[0-9a-fA-F]+ /)) {
            n = split(line, f, " "); size = hex(f[2]); file = f[3]; name = pending; pending = ""
        } else { pending = ""; next }
        if (size <= 0) next
        if (file ~ /libasx\.a\((interpreter|json|canon)\.o\)/ || file ~ /bm_(main|startup|fixtures)\.o/) who = "harness"
        else if (file ~ /libasx\.a\(/) who = "kernel"
        else if (file ~ /\.o$|\.a\(/) who = "libc"
        else next
        if (name ~ /^\.(text|rodata|srodata|sdata_rodata)/ || name ~ /^\.rodata/) kind = "text"
        else if (name ~ /^\.(data|sdata|tdata)/) kind = "data"
        else if (name ~ /^\.(bss|sbss|tbss)/ || name == "COMMON") kind = "bss"
        else next
        t[who, kind] += size
    }
    END {
        printf "%-9s %10s %10s %10s %10s\n", "origin", "text", "data", "bss", "flash"
        split("kernel harness libc", w, " ")
        for (i = 1; i <= 3; i++) {
            x = w[i]
            printf "%-9s %10d %10d %10d %10d\n", x, t[x, "text"], t[x, "data"], t[x, "bss"], t[x, "text"] + t[x, "data"]
        }
    }' "$out/image.map"
    echo ""
    echo "# largest objects in RAM (b/B/d/D), bytes"
    $nm -S --size-sort -r "$out/image.elf" | grep -i -E ' [bd] ' | head -12
    echo ""
    echo "# largest objects in flash (r/R/t/T), bytes"
    $nm -S --size-sort -r "$out/image.elf" | grep -i -E ' [rt] ' | head -8
} > "$out/size.txt"

rc=0
# shellcheck disable=SC2086
timeout "$limit" "$qemu" $qemu_args -nographic -monitor none -serial none \
    -chardev "file,id=bmout,path=$out/qemu.txt" \
    -semihosting-config "enable=on,target=native,chardev=bmout" \
    -kernel "$out/image.elf" > "$out/qemu.stdout" 2>&1 || rc=$?
echo "$rc" > "$out/qemu.rc"
exit 0
