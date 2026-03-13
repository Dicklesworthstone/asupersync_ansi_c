/*
 * cbmc_compat.h — CBMC/standalone compatibility macros
 *
 * When compiled with CBMC (define CBMC=1), uses __CPROVER primitives.
 * When compiled standalone, degrades to assert() for standard testing.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_CBMC_COMPAT_H
#define ASX_CBMC_COMPAT_H

#ifdef CBMC
#define VERIFY(cond) __CPROVER_assert(cond, #cond)
#define ASSUME(cond) __CPROVER_assume(cond)
#define NONDET_UINT() __CPROVER_nondet_unsigned()
#define NONDET_UINT64() __CPROVER_nondet_uint64_t()
#else
#include <assert.h>
#include <stdlib.h>

#define VERIFY(cond) assert(cond)
#define ASSUME(cond)                                                                               \
    do {                                                                                           \
        if (!(cond)) return;                                                                       \
    } while (0)
#define NONDET_UINT() 0u
#define NONDET_UINT64() 0ull
#endif

#endif /* ASX_CBMC_COMPAT_H */
