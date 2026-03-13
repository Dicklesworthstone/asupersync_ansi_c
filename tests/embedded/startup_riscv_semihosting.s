/*
 * startup_riscv_semihosting.s — Minimal RISC-V startup for QEMU virt
 *
 * Provides _Reset entry point, BSS zeroing, and exit via QEMU virt
 * machine's test finisher device (syscon at 0x100000).
 *
 * Write 0x5555 for PASS, 0x3333 for FAIL.
 *
 * SPDX-License-Identifier: MIT
 */

    .section .text.init, "ax", @progbits
    .align   2
    .globl   _Reset
    .globl   _start
    .type    _Reset, @function
_start:
_Reset:
    /* Set up stack pointer */
    la      sp, _stack_top

    /* Zero BSS */
    la      t0, _sbss
    la      t1, _ebss
.Lzero_bss:
    bge     t0, t1, .Lbss_done
    sw      zero, 0(t0)
    addi    t0, t0, 4
    j       .Lzero_bss
.Lbss_done:

    /* Call main */
    jal     ra, main

    /* Exit via QEMU virt test finisher at 0x100000
     * 0x5555 = FINISHER_PASS (exit code 0)
     * 0x3333 = FINISHER_FAIL (exit code 1)
     */
    li      t0, 0x100000        /* Test finisher MMIO address */
    bnez    a0, .Lexit_fail
    li      t1, 0x5555          /* FINISHER_PASS */
    j       .Ldo_exit
.Lexit_fail:
    li      t1, 0x3333          /* FINISHER_FAIL */
.Ldo_exit:
    sw      t1, 0(t0)

    /* Should not reach here */
.Lspin:
    wfi
    j       .Lspin
    .size   _Reset, . - _Reset

    .end
