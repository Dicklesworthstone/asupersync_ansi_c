/*
 * arm_m4.ld -- memory map of the Cortex-M4 proof image (qemu mps2-an386;
 * make test-baremetal-qemu, bd-9kll.15.7).
 *
 * mps2-an386 has 4 MiB of RAM-backed "flash" at 0x0, 4 MiB of SSRAM at
 * 0x20000000 (mirrored at 0x20400000, so it is not 8 MiB) and 16 MiB of
 * PSRAM at 0x21000000. Code and constants go to the first, .data's load
 * image too. RAM is the PSRAM: the conformance interpreter's static
 * document, event and dispatch buffers are about 3.5 MiB, which with the
 * kernel arenas does not fit the SSRAM. .data, .bss, the heap and the stack
 * live there. The program fails to link when they do not fit, which is the
 * capacity check.
 *
 * SPDX-License-Identifier: MIT
 */

ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 4M
    RAM   (rwx) : ORIGIN = 0x21000000, LENGTH = 16M
}

_stack_size = 32K;
_heap_size = 64K;

SECTIONS
{
    .text :
    {
        KEEP(*(.isr_vector))
        *(.text .text.*)
        *(.rodata .rodata.*)
        . = ALIGN(4);
    } > FLASH

    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.eh_frame*) *(.init) *(.fini) }

    _sidata = LOADADDR(.data);

    .data :
    {
        _sdata = .;
        *(.data .data.*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH

    .bss (NOLOAD) :
    {
        _sbss = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(8);
        _ebss = .;
    } > RAM

    .heap (NOLOAD) :
    {
        . = ALIGN(8);
        _heap_start = .;
        . += _heap_size;
        _heap_end = .;
    } > RAM

    .stack (NOLOAD) :
    {
        . = ALIGN(8);
        . += _stack_size;
        _estack = .;
    } > RAM
}
