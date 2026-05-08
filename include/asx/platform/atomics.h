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
#pragma intrinsic(_InterlockedExchangeAdd)
#pragma intrinsic(_InterlockedOr)
#endif

#if !ASX_LOCKFREE_SINGLE_THREAD && defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L &&     \
    !defined(__STDC_NO_ATOMICS__) && !defined(__clang__) && !defined(__GNUC__) &&                  \
    !defined(_MSC_VER)
#include <stdatomic.h>
#endif

/*
 * Portable atomic layer for lock-free runtime foundations.
 *
 * Backend selection order:
 *   1. ASX_LOCKFREE_SINGLE_THREAD plain loads/stores
 *   2. GCC/Clang __atomic builtins
 *   3. MSVC _Interlocked* intrinsics
 *   4. C11 _Atomic
 *
 * Memory-order policy:
 *   - init: relaxed/object initialization
 *   - load: acquire
 *   - store: release
 *   - compare-exchange success: acq_rel
 *   - compare-exchange failure: acquire
 *   - exchange/fetch_add: acq_rel
 *   - fences: acquire or release as named
 *
 * The policy is intentionally stronger than some consumers need. It gives
 * channel publication, task metadata, cancellation flags, and future trace
 * sequence commits one shared contract across backends.
 */

typedef enum {
    ASX_ATOMIC_BACKEND_SINGLE_THREAD = 1,
    ASX_ATOMIC_BACKEND_GNU = 2,
    ASX_ATOMIC_BACKEND_MSVC = 3,
    ASX_ATOMIC_BACKEND_C11 = 4
} asx_atomic_backend;

#if ASX_LOCKFREE_SINGLE_THREAD
#define ASX_ATOMIC_U32_BACKEND ASX_ATOMIC_BACKEND_SINGLE_THREAD
#elif defined(__clang__) || defined(__GNUC__)
#define ASX_ATOMIC_U32_BACKEND ASX_ATOMIC_BACKEND_GNU
#elif defined(_MSC_VER)
#define ASX_ATOMIC_U32_BACKEND ASX_ATOMIC_BACKEND_MSVC
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
#define ASX_ATOMIC_U32_BACKEND ASX_ATOMIC_BACKEND_C11
#else
#define ASX_ATOMIC_U32_BACKEND 0
#endif

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

static inline asx_atomic_backend asx_atomic_u32_backend(void) {
    return (asx_atomic_backend)ASX_ATOMIC_U32_BACKEND;
}

static inline int asx_atomic_u32_is_single_threaded(void) {
    return ASX_ATOMIC_U32_BACKEND == ASX_ATOMIC_BACKEND_SINGLE_THREAD ? 1 : 0;
}

static inline void asx_atomic_u32_init(asx_atomic_u32 *a, uint32_t v) {
#if ASX_LOCKFREE_SINGLE_THREAD
    a->value = v;
#elif defined(__clang__) || defined(__GNUC__)
    __atomic_store_n(&a->value, v, __ATOMIC_RELAXED);
#elif defined(_MSC_VER)
    a->value = (long)v;
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
    atomic_init(&a->value, v);
#endif
}

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

static inline int asx_atomic_u32_compare_exchange(asx_atomic_u32 *a, uint32_t *expected,
                                                  uint32_t desired) {
    if (expected == NULL) { return 0; }
#if ASX_LOCKFREE_SINGLE_THREAD
    if (a->value == *expected) {
        a->value = desired;
        return 1;
    }
    *expected = a->value;
    return 0;
#elif defined(__clang__) || defined(__GNUC__)
    return __atomic_compare_exchange_n(&a->value, expected, desired, 0, __ATOMIC_ACQ_REL,
                                       __ATOMIC_ACQUIRE)
               ? 1
               : 0;
#elif defined(_MSC_VER)
    long observed =
        _InterlockedCompareExchange((volatile long *)&a->value, (long)desired, (long)*expected);
    if (observed == (long)*expected) { return 1; }
    *expected = (uint32_t)observed;
    return 0;
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
    return atomic_compare_exchange_strong_explicit(&a->value, expected, desired,
                                                   memory_order_acq_rel, memory_order_acquire)
               ? 1
               : 0;
#endif
}

static inline int asx_atomic_u32_cas(asx_atomic_u32 *a, uint32_t expected, uint32_t desired) {
    return asx_atomic_u32_compare_exchange(a, &expected, desired);
}

static inline uint32_t asx_atomic_u32_exchange(asx_atomic_u32 *a, uint32_t v) {
#if ASX_LOCKFREE_SINGLE_THREAD
    uint32_t old = a->value;
    a->value = v;
    return old;
#elif defined(__clang__) || defined(__GNUC__)
    return __atomic_exchange_n(&a->value, v, __ATOMIC_ACQ_REL);
#elif defined(_MSC_VER)
    return (uint32_t)_InterlockedExchange((volatile long *)&a->value, (long)v);
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
    return atomic_exchange_explicit(&a->value, v, memory_order_acq_rel);
#endif
}

static inline uint32_t asx_atomic_u32_fetch_add(asx_atomic_u32 *a, uint32_t delta) {
#if ASX_LOCKFREE_SINGLE_THREAD
    uint32_t old = a->value;
    a->value = old + delta;
    return old;
#elif defined(__clang__) || defined(__GNUC__)
    return __atomic_fetch_add(&a->value, delta, __ATOMIC_ACQ_REL);
#elif defined(_MSC_VER)
    return (uint32_t)_InterlockedExchangeAdd((volatile long *)&a->value, (long)delta);
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
    return atomic_fetch_add_explicit(&a->value, delta, memory_order_acq_rel);
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
