/*
 * cleanup.c — deterministic cleanup stack implementation
 *
 * LIFO stack of cleanup actions for RAII-equivalent unwind in C.
 * Walking skeleton uses fixed-size arrays (ASX_CLEANUP_STACK_CAPACITY).
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/core/cleanup.h>
#include <stddef.h>

void asx_cleanup_init(asx_cleanup_stack *stack)
{
    uint32_t i;
    if (stack == NULL) return;
    for (i = 0; i < ASX_CLEANUP_STACK_CAPACITY; i++) {
        stack->fns[i]  = NULL;
        stack->data[i] = NULL;
    }
    stack->count   = 0;
    stack->drained = 0;
}

asx_status asx_cleanup_push(asx_cleanup_stack *stack,
                             asx_cleanup_fn fn,
                             void *user_data,
                             asx_cleanup_handle *out_handle)
{
    uint32_t idx;

    if (stack == NULL || fn == NULL || out_handle == NULL)
        return ASX_E_INVALID_ARGUMENT;
    if (stack->count >= ASX_CLEANUP_STACK_CAPACITY)
        return ASX_E_RESOURCE_EXHAUSTED;

    idx = stack->count++;
    stack->fns[idx]  = fn;
    stack->data[idx] = user_data;
    *out_handle = idx;
    return ASX_OK;
}

asx_status asx_cleanup_pop(asx_cleanup_stack *stack,
                            asx_cleanup_handle handle)
{
    if (stack == NULL) return ASX_E_INVALID_ARGUMENT;
    if (handle >= stack->count) return ASX_E_NOT_FOUND;
    if (stack->fns[handle] == NULL) return ASX_E_NOT_FOUND;

    /* Mark as resolved — will be skipped during drain */
    stack->fns[handle]  = NULL;
    stack->data[handle] = NULL;
    return ASX_OK;
}

void asx_cleanup_drain(asx_cleanup_stack *stack)
{
    uint32_t i;
    if (stack == NULL) return;
    if (stack->drained) return;

    /* Drain in LIFO order: highest index first */
    i = stack->count;
    while (i > 0) {
        i--;
        if (stack->fns[i] != NULL) {
            stack->fns[i](stack->data[i]);
            stack->fns[i]  = NULL;
            stack->data[i] = NULL;
        }
    }

    stack->count   = 0;
    stack->drained = 1;
}

uint32_t asx_cleanup_pending(const asx_cleanup_stack *stack)
{
    uint32_t i, pending;
    if (stack == NULL) return 0;
    pending = 0;
    for (i = 0; i < stack->count; i++) {
        if (stack->fns[i] != NULL) {
            pending++;
        }
    }
    return pending;
}
