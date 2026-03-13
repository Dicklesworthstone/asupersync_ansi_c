/*
 * riscv32.ld — Linker script for RV32IMAC bare-metal targets
 *
 * Generic SRAM-only layout suitable for QEMU virt machine.
 * All sections placed in a single 4MB SRAM region (virt machine default).
 * Stack size: configurable via __stack_size (default 4KB)
 *
 * SPDX-License-Identifier: MIT
 */

__stack_size = DEFINED(__stack_size) ? __stack_size : 0x1000;

MEMORY
{
    SRAM (rwx) : ORIGIN = 0x80000000, LENGTH = 4M
}

ENTRY(_Reset)

SECTIONS
{
    .text :
    {
        *(.text.init)
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
        _etext = .;
    } > SRAM

    .data :
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

    .htif ALIGN(64) :
    {
        *(.htif)
    } > SRAM

    .stack (NOLOAD) :
    {
        . = ALIGN(16);
        _stack_bottom = .;
        . = . + __stack_size;
        . = ALIGN(16);
        _stack_top = .;
    } > SRAM

    _end = .;

    /* Heap symbols for picolibc sbrk */
    __heap_start = _end;
    __heap_end = ORIGIN(SRAM) + LENGTH(SRAM);

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
    }
}
