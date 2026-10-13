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

#include <asx/asx_config.h>
#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum entries per cleanup stack (walking skeleton) */
#if (ASX_CLEANUP_STACK_CAPACITY) < 1
#error "ASX_CLEANUP_STACK_CAPACITY must be at least 1"
#endif

/* Cleanup action callback: called with user context during drain.
 * Must not fail — cleanup actions are best-effort during unwind. */
typedef void (*asx_cleanup_fn)(void *user_data);

/* Opaque cleanup entry handle for pop/cancel */
typedef uint32_t asx_cleanup_handle;

/* Sentinel for invalid cleanup handle */
#define ASX_CLEANUP_INVALID ((asx_cleanup_handle)UINT32_MAX)

/* Async cleanup action (a region's async finalizer, Rust's
 * Scope::defer_async): a task poll function. A drain never calls it inline;
 * asx_cleanup_take hands it back so the caller can run it to completion as
 * a task. */
typedef asx_status (*asx_cleanup_poll_fn)(void *user_data, asx_task_id self);

/* Cleanup stack (fixed-size, walking skeleton). An entry is sync (fns[i])
 * or async (polls[i]); a resolved or retired entry has both NULL. */
typedef struct {
    asx_cleanup_fn fns[ASX_CLEANUP_STACK_CAPACITY];
    asx_cleanup_poll_fn polls[ASX_CLEANUP_STACK_CAPACITY];
    void *data[ASX_CLEANUP_STACK_CAPACITY];
    uint16_t generations[ASX_CLEANUP_STACK_CAPACITY];
    uint32_t count;    /* stack depth / high-water slot + 1 */
    uint32_t drained;  /* 1 after drain has run */
    uint32_t draining; /* 1 while callbacks run; nested drain is a no-op */
} asx_cleanup_stack;

/* Initialize a cleanup stack to empty state. */
ASX_API void asx_cleanup_init(asx_cleanup_stack *stack);

/* Register a cleanup action. Returns a handle for later pop/cancel.
 * Returns ASX_E_RESOURCE_EXHAUSTED if the stack is full.
 * Returns ASX_E_INVALID_STATE during a drain (no callback-time pushes).
 * The cleanup_fn will be called during drain if not popped first. */
ASX_API ASX_MUST_USE asx_status asx_cleanup_push(asx_cleanup_stack *stack, asx_cleanup_fn fn,
                                                 void *user_data, asx_cleanup_handle *out_handle);

/* Register an async cleanup action (Rust's Scope::defer_async). It shares
 * the LIFO order and the handle space of the sync entries, but a drain never
 * calls it: the drain stops with the entry on top, and the owner takes it
 * with asx_cleanup_take to run it as a task.
 * Same errors as asx_cleanup_push. */
ASX_API ASX_MUST_USE asx_status asx_cleanup_push_async(asx_cleanup_stack *stack,
                                                       asx_cleanup_poll_fn poll_fn, void *user_data,
                                                       asx_cleanup_handle *out_handle);

/* Pop (resolve) a cleanup entry by handle. The cleanup function
 * will NOT be called during drain. Returns ASX_E_NOT_FOUND if
 * the handle is invalid or already popped. */
ASX_API ASX_MUST_USE asx_status asx_cleanup_pop(asx_cleanup_stack *stack,
                                                asx_cleanup_handle handle);

/* Drain remaining (un-popped) entries in LIFO order. Each sync cleanup
 * function is called exactly once. The drain stops, leaving it and every
 * entry beneath it pending, when an async entry reaches the top; the stack
 * counts as drained only once it is empty. Calling drain on an
 * already-drained stack is a no-op, as is a recursive drain from a cleanup
 * callback. A callback may pop another pending entry to cancel that action. */
ASX_API void asx_cleanup_drain(asx_cleanup_stack *stack);

/* Take the entry on top of the stack, sync or async: retire it and return
 * its action and context. Exactly one of *out_fn (a sync entry) and
 * *out_poll (an async entry) is non-NULL on success. The owner that runs
 * entries as tasks, as a region does, takes them one at a time with this
 * instead of draining. Returns ASX_E_NOT_FOUND when the stack is empty or
 * during a drain callback, ASX_E_INVALID_ARGUMENT for a NULL argument. */
ASX_API ASX_MUST_USE asx_status asx_cleanup_take(asx_cleanup_stack *stack, asx_cleanup_fn *out_fn,
                                                 asx_cleanup_poll_fn *out_poll, void **out_data);

/* Query the number of un-popped entries remaining. */
ASX_API uint32_t asx_cleanup_pending(const asx_cleanup_stack *stack);

/* Query how many of the un-popped entries are async. */
ASX_API uint32_t asx_cleanup_async_pending(const asx_cleanup_stack *stack);

#ifdef __cplusplus
}
#endif

#endif /* ASX_CORE_CLEANUP_H */
