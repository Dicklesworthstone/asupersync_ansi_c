#!/usr/bin/env python3
"""elf_size.py -- Berkeley `size` for any ELF (32/64-bit, either endianness),
for the musl cross builds of make test-musl-qemu (bd-9kll.15.8).

The host's binutils `size` reads only its own architecture unless
binutils-multiarch is installed, and the lane runs on runners that have
neither. text is every allocated section that is not writable and holds
bytes, data every allocated writable one that holds bytes, bss every
allocated NOBITS section (thread-local .tbss takes no address space and is
left out), exactly what `size` sums.

Usage: elf_size.py FILE...   (prints "text data bss dec hex filename")

SPDX-License-Identifier: MIT
"""

import struct
import sys

SHF_WRITE = 0x1
SHF_ALLOC = 0x2
SHF_TLS = 0x400
SHT_NOBITS = 8


def sizes(path):
    with open(path, "rb") as f:
        blob = f.read()
    if blob[:4] != b"\x7fELF":
        raise SystemExit("elf_size: %s is not an ELF file" % path)
    wide = blob[4] == 2
    end = ">" if blob[5] == 2 else "<"
    if wide:
        shoff, = struct.unpack_from(end + "Q", blob, 0x28)
        shentsize, shnum = struct.unpack_from(end + "HH", blob, 0x3A)
    else:
        shoff, = struct.unpack_from(end + "I", blob, 0x20)
        shentsize, shnum = struct.unpack_from(end + "HH", blob, 0x2E)
    text = data = bss = 0
    for i in range(shnum):
        base = shoff + i * shentsize
        if wide:
            _, sh_type, flags, _, _, size = struct.unpack_from(end + "IIQQQQ", blob, base)
        else:
            _, sh_type, flags, _, _, size = struct.unpack_from(end + "IIIIII", blob, base)
        if not flags & SHF_ALLOC:
            continue
        if sh_type == SHT_NOBITS:
            if not flags & SHF_TLS:
                bss += size
        elif flags & SHF_WRITE:
            data += size
        else:
            text += size
    return text, data, bss


def main(argv):
    if len(argv) < 2:
        raise SystemExit("usage: elf_size.py FILE...")
    print("%8s %8s %8s %8s %8s filename" % ("text", "data", "bss", "dec", "hex"))
    for path in argv[1:]:
        text, data, bss = sizes(path)
        total = text + data + bss
        print("%8d %8d %8d %8d %8x %s" % (text, data, bss, total, total, path))


if __name__ == "__main__":
    main(sys.argv)
