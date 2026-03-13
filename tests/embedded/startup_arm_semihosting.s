/*
 * startup_arm_semihosting.s — Minimal ARM Cortex-M startup with semihosting
 *
 * Provides _Reset entry point, vector table, BSS zeroing, and semihosting
 * exit syscall. Links with C code that provides main().
 *
 * Semihosting uses BKPT #0xAB on ARMv6-M/ARMv7-M.
 *
 * SPDX-License-Identifier: MIT
 */

    .syntax unified
    .cpu    cortex-m4
    .thumb

/* ------------------------------------------------------------------ */
/* Vector table                                                        */
/* ------------------------------------------------------------------ */
    .section .isr_vector, "a", %progbits
    .align   2
    .globl   __isr_vector
__isr_vector:
    .word    _stack_top          /* Initial stack pointer */
    .word    _Reset              /* Reset handler */
    .word    _Default_Handler    /* NMI */
    .word    _Default_Handler    /* HardFault */
    .size    __isr_vector, . - __isr_vector

/* ------------------------------------------------------------------ */
/* Reset handler                                                       */
/* ------------------------------------------------------------------ */
    .section .text, "ax", %progbits
    .align   2
    .globl   _Reset
    .type    _Reset, %function
    .thumb_func
_Reset:
    /* Zero BSS */
    ldr     r0, =_sbss
    ldr     r1, =_ebss
    movs    r2, #0
.Lzero_bss:
    cmp     r0, r1
    bge     .Lbss_done
    str     r2, [r0]
    adds    r0, r0, #4
    b       .Lzero_bss
.Lbss_done:

    /* Copy .data from flash to SRAM */
    ldr     r0, =_sdata
    ldr     r1, =_edata
    ldr     r2, =_etext
.Lcopy_data:
    cmp     r0, r1
    bge     .Ldata_done
    ldr     r3, [r2]
    str     r3, [r0]
    adds    r0, r0, #4
    adds    r2, r2, #4
    b       .Lcopy_data
.Ldata_done:

    /* Call main */
    bl      main

    /* Semihosting SYS_EXIT (0x18)
     * r0 = 0x18 (operation)
     * r1 = ADP_Stopped_ApplicationExit (0x20026) for success,
     *       or ADP_Stopped_RunTimeErrorUnknown (0x20023) for failure
     */
    cmp     r0, #0
    beq     .Lexit_ok
    ldr     r1, =0x20023        /* ADP_Stopped_RunTimeErrorUnknown */
    b       .Ldo_exit
.Lexit_ok:
    ldr     r1, =0x20026        /* ADP_Stopped_ApplicationExit */
.Ldo_exit:
    movs    r0, #0x18           /* SYS_EXIT */
    bkpt    #0xAB               /* Semihosting trap */
    b       .                   /* Should not reach here */
    .size   _Reset, . - _Reset

/* ------------------------------------------------------------------ */
/* Default handler (infinite loop)                                     */
/* ------------------------------------------------------------------ */
    .globl   _Default_Handler
    .type    _Default_Handler, %function
    .thumb_func
_Default_Handler:
    b       .
    .size   _Default_Handler, . - _Default_Handler

    .end
