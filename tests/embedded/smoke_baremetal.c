/*
 * smoke_baremetal.c — Trivial bare-metal smoke test
 *
 * Returns 0 on success. Used to verify that the semihosting startup
 * code and QEMU environment work correctly.
 *
 * SPDX-License-Identifier: MIT
 */

/* Minimal volatile write to prevent optimizer from eliminating everything */
static volatile int result;

int main(void) {
    /* Simple computation to verify CPU works */
    int a = 21;
    int b = 2;
    result = a * b;

    /* Return 0 = success */
    return (result == 42) ? 0 : 1;
}
