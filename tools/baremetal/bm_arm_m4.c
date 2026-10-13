/*
 * bm_arm_m4.c -- vector table, reset handler and the C library's few system
 * calls for the Cortex-M4 proof image (mps2-an386 under qemu-system-arm;
 * make test-baremetal-qemu, bd-9kll.15.7).
 *
 * No newlib start files: the reset handler copies .data, clears .bss and
 * calls main. A fault ends the run with status 70 (so a bad image fails the
 * gate instead of hanging it). The C library asks for memory with _sbrk;
 * it gets a static heap whose size is the linker script's.
 *
 * Not part of the library.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bm_semihost.h"

#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

extern uint32_t _estack;
extern uint32_t _sidata;
extern uint32_t _sdata;
extern uint32_t _edata;
extern uint32_t _sbss;
extern uint32_t _ebss;
extern uint32_t _heap_start;
extern uint32_t _heap_end;

int main(void);
void Reset_Handler(void);

void Reset_Handler(void) {
    const uint32_t *src = &_sidata;
    uint32_t *dst;
    for (dst = &_sdata; dst < &_edata;) *dst++ = *src++;
    for (dst = &_sbss; dst < &_ebss;) *dst++ = 0u;
    bm_exit(main());
}

/* Every fault and unexpected interrupt: report and stop. */
static void bm_fault(void) {
    bm_puts("bm: FAULT\n");
    bm_exit(70);
}

typedef void (*bm_vector)(void);

/* Initial stack pointer, reset, NMI, HardFault, MemManage, BusFault,
 * UsageFault, four reserved, SVCall, DebugMon, reserved, PendSV, SysTick. */
__attribute__((section(".isr_vector"), used)) const bm_vector g_bm_vectors[16] = {
    (bm_vector)(uintptr_t)&_estack,
    Reset_Handler,
    bm_fault,
    bm_fault,
    bm_fault,
    bm_fault,
    bm_fault,
    0,
    0,
    0,
    0,
    bm_fault,
    bm_fault,
    0,
    bm_fault,
    bm_fault,
};

void *_sbrk(ptrdiff_t incr);
void *_sbrk(ptrdiff_t incr) {
    static uint8_t *brk;
    uint8_t *prev;
    if (brk == 0) brk = (uint8_t *)&_heap_start;
    prev = brk;
    if (incr < 0 || (uintptr_t)(&_heap_end) - (uintptr_t)brk < (uintptr_t)incr) {
        return (void *)-1;
    }
    brk += incr;
    return prev;
}

/* The C library's system calls. The image has no files and no processes:
 * the ones stdio touches answer "not a file"/"a terminal" and the rest
 * fail; _write goes to the semihosting console, _exit to the host. */
void _exit(int status);
void _exit(int status) { bm_exit(status); }

int _close(int fd);
int _close(int fd) {
    (void)fd;
    return -1;
}

int _fstat(int fd, struct stat *st);
int _fstat(int fd, struct stat *st) {
    (void)fd;
    st->st_mode = S_IFCHR;
    return 0;
}

int _isatty(int fd);
int _isatty(int fd) {
    (void)fd;
    return 1;
}

off_t _lseek(int fd, off_t offset, int whence);
off_t _lseek(int fd, off_t offset, int whence) {
    (void)fd;
    (void)offset;
    (void)whence;
    return -1;
}

int _read(int fd, char *buf, int len);
int _read(int fd, char *buf, int len) {
    (void)fd;
    (void)buf;
    (void)len;
    return -1;
}

int _getpid(void);
int _getpid(void) { return 1; }

int _kill(int pid, int sig);
int _kill(int pid, int sig) {
    (void)pid;
    (void)sig;
    return -1;
}

int _write(int fd, const char *buf, int len);
int _write(int fd, const char *buf, int len) {
    char chunk[64];
    int done = 0;
    (void)fd;
    while (done < len) {
        int n = len - done < (int)sizeof(chunk) - 1 ? len - done : (int)sizeof(chunk) - 1;
        int k;
        for (k = 0; k < n; k++) chunk[k] = buf[done + k];
        chunk[n] = '\0';
        bm_puts(chunk);
        done += n;
    }
    return len;
}
