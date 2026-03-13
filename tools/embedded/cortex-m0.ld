/*
 * cortex-m0.ld — Linker script for Cortex-M0 bare-metal targets
 *
 * Memory map: 128KB Flash + 16KB SRAM (typical STM32F0 layout)
 * Stack size: configurable via __stack_size (default 4KB)
 *
 * SPDX-License-Identifier: MIT
 */

__stack_size = DEFINED(__stack_size) ? __stack_size : 0x1000;

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 128K
    SRAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 16K
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
        _etext = .;
    } > FLASH

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
