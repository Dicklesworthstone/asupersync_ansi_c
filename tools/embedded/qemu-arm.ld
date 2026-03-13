/*
 * qemu-arm.ld — Linker script for QEMU ARM Cortex-M semihosting tests
 *
 * Targets the mps2-an385 machine (ZBT SSRAM at 0x00000000 and 0x20000000, 4MB each).
 * Stack size: configurable via __stack_size (default 4KB)
 *
 * SPDX-License-Identifier: MIT
 */

__stack_size = DEFINED(__stack_size) ? __stack_size : 0x1000;

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 4M
    SRAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 4M
}

ENTRY(_Reset)

SECTIONS
{
    .text :
    {
        KEEP(*(.isr_vector))
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH

    /* ARM exception index (unwind tables) */
    .ARM.exidx :
    {
        *(.ARM.exidx*)
        *(.ARM.extab*)
    } > FLASH

    . = ALIGN(4);
    _etext = .;

    .data : AT(_etext)
    {
        _sdata = .;
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > SRAM

    .bss (NOLOAD) :
    {
        _sbss = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > SRAM

    .stack (NOLOAD) :
    {
        . = ALIGN(8);
        _stack_bottom = .;
        . = . + __stack_size;
        . = ALIGN(8);
        _stack_top = .;
    } > SRAM

    _end = .;

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
    }
}
