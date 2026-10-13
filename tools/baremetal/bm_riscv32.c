/*
 * bm_riscv32.c -- the RISC-V 32 proof image's one system call of its own
 * (qemu virt; make test-baremetal-qemu, bd-9kll.15.7).
 *
 * Start-up, the memory map, TLS and the stdio streams are picolibc's
 * (--specs=picolibc.specs --oslib=semihost, picolibc.ld with the memory
 * regions proof.sh defines). Only the exit is ours, so that a failed
 * fixture count reaches qemu's exit status through the same
 * SYS_EXIT_EXTENDED call as on the Cortex-M4 image.
 *
 * Not part of the library.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bm_semihost.h"

void _exit(int status);
void _exit(int status) { bm_exit(status); }
