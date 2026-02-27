/*
 * asx/core/cleanup.h — deterministic cleanup stack for RAII-equivalent unwind
 *
 * C lacks RAII and destructors. The cleanup stack provides explicit
 * deterministic LIFO unwind: every acquire registers a cleanup action,
 * commit/abort pops it, finalization drains the remainder.
 *
 * Cleanup stacks are per-region (walking skeleton uses fixed-size static
 * arrays). Phase 3 will add per-task stacks and hook-backed allocation.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_CORE_CLEANUP_H
#define ASX_CORE_CLEANUP_H

#include <stdint.h>
#include <asx/asx_export.h>
#include <asx/asx_status.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum entries per cleanup stack (walking skeleton) */
#define ASX_CLEANUP_STACK_CAPACITY 32

/* Cleanup action callback: called with user context during drain.
 * Must not fail — cleanup actions are best-effort during unwind. */
typedef void (*asx_cleanup_fn)(void *user_data);

/* Opaque cleanup entry handle for pop/cancel */
typedef uint32_t asx_cleanup_handle;

/* Sentinel for invalid cleanup handle */
#define ASX_CLEANUP_INVALID ((asx_cleanup_handle)UINT32_MAX)

/* Cleanup stack (fixed-size, walking skeleton) */
typedef struct {
    asx_cleanup_fn  fns[ASX_CLEANUP_STACK_CAPACITY];
    void           *data[ASX_CLEANUP_STACK_CAPACITY];
    uint32_t        count;      /* number of active entries */
    uint32_t        drained;    /* 1 after drain has run */
} asx_cleanup_stack;

/* Initialize a cleanup stack to empty state. */
ASX_API void asx_cleanup_init(asx_cleanup_stack *stack);

/* Register a cleanup action. Returns a handle for later pop/cancel.
 * Returns ASX_E_RESOURCE_EXHAUSTED if the stack is full.
 * The cleanup_fn will be called during drain if not popped first. */
ASX_API ASX_MUST_USE asx_status asx_cleanup_push(asx_cleanup_stack *stack,
                                                 asx_cleanup_fn fn,
                                                 void *user_data,
                                                 asx_cleanup_handle *out_handle);

/* Pop (resolve) a cleanup entry by handle. The cleanup function
 * will NOT be called during drain. Returns ASX_E_NOT_FOUND if
 * the handle is invalid or already popped. */
ASX_API ASX_MUST_USE asx_status asx_cleanup_pop(asx_cleanup_stack *stack,
                                                asx_cleanup_handle handle);

/* Drain all remaining (un-popped) entries in LIFO order.
 * Each cleanup function is called exactly once. After drain,
 * the stack is empty. Calling drain on an already-drained
 * stack is a no-op. */
ASX_API void asx_cleanup_drain(asx_cleanup_stack *stack);

/* Query the number of un-popped entries remaining. */
ASX_API uint32_t asx_cleanup_pending(const asx_cleanup_stack *stack);

#ifdef __cplusplus
}
#endif

#endif /* ASX_CORE_CLEANUP_H */
