/*
 * bm_semihost.h -- ARM and RISC-V semihosting calls of the bare-metal
 * fixture proof (make test-baremetal-qemu, bd-9kll.15.7).
 *
 * The images have no file system and no UART driver: the only host service
 * they use is semihosting, which qemu answers (-semihosting-config
 * enable=on). Two operations are enough: SYS_WRITE0 to print one NUL
 * terminated string and SYS_EXIT_EXTENDED to end with a status.
 *
 * Not part of the library.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_BM_SEMIHOST_H
#define ASX_BM_SEMIHOST_H

#include <stdint.h>

#define BM_SYS_WRITE0 0x04
#define BM_SYS_EXIT_EXTENDED 0x20
#define BM_ADP_STOPPED_APPLICATION_EXIT 0x20026u

/* One semihosting call: `op` in the first argument register, the parameter
 * (a pointer or a word) in the second, the result back in the first. */
static inline long bm_semihost(long op, const void *arg) {
#if defined(__arm__)
    register long r0 __asm__("r0") = op;
    register const void *r1 __asm__("r1") = arg;
    __asm__ volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
#elif defined(__riscv)
    /* The three-instruction trap sequence qemu recognizes: uncompressed, in
     * one 16-byte aligned block, so no interrupt or compressed fetch can
     * split it. */
    register long a0 __asm__("a0") = op;
    register const void *a1 __asm__("a1") = arg;
    __asm__ volatile(".option push\n"
                     ".option norvc\n"
                     ".balign 16\n"
                     "slli x0, x0, 0x1f\n"
                     "ebreak\n"
                     "srai x0, x0, 7\n"
                     ".option pop\n"
                     : "+r"(a0)
                     : "r"(a1)
                     : "memory");
    return a0;
#else
#error "bm_semihost.h: ARM or RISC-V only"
#endif
}

/* Print `s` (NUL terminated) on the semihosting console. */
static inline void bm_puts(const char *s) { (void)bm_semihost(BM_SYS_WRITE0, s); }

/* End the run with `status`; qemu exits with it. Does not return. */
static inline void bm_exit(int status) {
    uintptr_t block[2];
    block[0] = (uintptr_t)BM_ADP_STOPPED_APPLICATION_EXIT;
    block[1] = (uintptr_t)(unsigned int)status;
    (void)bm_semihost(BM_SYS_EXIT_EXTENDED, block);
    for (;;) {
    }
}

#endif /* ASX_BM_SEMIHOST_H */
