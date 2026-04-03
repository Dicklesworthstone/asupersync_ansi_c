#ifndef ASX_PLATFORM_ATOMICS_H
#define ASX_PLATFORM_ATOMICS_H

#include <stdint.h>

#ifndef ASX_LOCKFREE_SINGLE_THREAD
#define ASX_LOCKFREE_SINGLE_THREAD 1
#endif

#if !ASX_LOCKFREE_SINGLE_THREAD && defined(_MSC_VER)
#include <intrin.h>
#pragma intrinsic(_InterlockedCompareExchange)
#pragma intrinsic(_InterlockedExchange)
#pragma intrinsic(_InterlockedOr)
#endif

#if !ASX_LOCKFREE_SINGLE_THREAD && defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L &&     \
    !defined(__STDC_NO_ATOMICS__) && !defined(__clang__) && !defined(__GNUC__) &&                  \
    !defined(_MSC_VER)
#include <stdatomic.h>
#endif

/*
 * Minimal portable atomic layer for lock-free spike code.
 *
 * Backend selection order:
 *   1. ASX_LOCKFREE_SINGLE_THREAD plain loads/stores
 *   2. GCC/Clang __atomic builtins
 *   3. MSVC _Interlocked* intrinsics
 *   4. C11 _Atomic
 */

typedef struct {
#if ASX_LOCKFREE_SINGLE_THREAD
    uint32_t value;
#elif defined(__clang__) || defined(__GNUC__)
    uint32_t value;
#elif defined(_MSC_VER)
    volatile long value;
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
    _Atomic(uint32_t) value;
#else
#error "No supported atomic backend for asx/platform/atomics.h"
#endif
} asx_atomic_u32;

static inline uint32_t asx_atomic_u32_load(const asx_atomic_u32 *a) {
#if ASX_LOCKFREE_SINGLE_THREAD
    return a->value;
#elif defined(__clang__) || defined(__GNUC__)
    return __atomic_load_n(&a->value, __ATOMIC_ACQUIRE);
#elif defined(_MSC_VER)
    return (uint32_t)_InterlockedOr((volatile long *)&a->value, 0);
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
    return atomic_load_explicit(&a->value, memory_order_acquire);
#endif
}

static inline void asx_atomic_u32_store(asx_atomic_u32 *a, uint32_t v) {
#if ASX_LOCKFREE_SINGLE_THREAD
    a->value = v;
#elif defined(__clang__) || defined(__GNUC__)
    __atomic_store_n(&a->value, v, __ATOMIC_RELEASE);
#elif defined(_MSC_VER)
    (void)_InterlockedExchange((volatile long *)&a->value, (long)v);
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
    atomic_store_explicit(&a->value, v, memory_order_release);
#endif
}

static inline int asx_atomic_u32_cas(asx_atomic_u32 *a, uint32_t expected, uint32_t desired) {
#if ASX_LOCKFREE_SINGLE_THREAD
    if (a->value == expected) {
        a->value = desired;
        return 1;
    }
    return 0;
#elif defined(__clang__) || defined(__GNUC__)
    return __atomic_compare_exchange_n(&a->value, &expected, desired, 0, __ATOMIC_ACQ_REL,
                                       __ATOMIC_ACQUIRE)
               ? 1
               : 0;
#elif defined(_MSC_VER)
    return _InterlockedCompareExchange((volatile long *)&a->value, (long)desired, (long)expected) ==
                   (long)expected
               ? 1
               : 0;
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
    return atomic_compare_exchange_strong_explicit(&a->value, &expected, desired,
                                                   memory_order_acq_rel, memory_order_acquire)
               ? 1
               : 0;
#endif
}

static inline void asx_atomic_fence_acquire(void) {
#if ASX_LOCKFREE_SINGLE_THREAD
    return;
#elif defined(__clang__) || defined(__GNUC__)
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
#elif defined(_MSC_VER)
    _ReadWriteBarrier();
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
    atomic_thread_fence(memory_order_acquire);
#endif
}

static inline void asx_atomic_fence_release(void) {
#if ASX_LOCKFREE_SINGLE_THREAD
    return;
#elif defined(__clang__) || defined(__GNUC__)
    __atomic_thread_fence(__ATOMIC_RELEASE);
#elif defined(_MSC_VER)
    _ReadWriteBarrier();
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
    atomic_thread_fence(memory_order_release);
#endif
}

#endif
