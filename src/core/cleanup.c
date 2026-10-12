/*
 * cleanup.c — deterministic cleanup stack implementation
 *
 * LIFO stack of cleanup actions for RAII-equivalent unwind in C.
 * Walking skeleton uses fixed-size arrays (ASX_CLEANUP_STACK_CAPACITY).
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx_config.h>
#include <asx/core/cleanup.h>
#include <stddef.h>

#define ASX_CLEANUP_HANDLE_SLOT_MASK 0xFFFFu

static uint32_t cleanup_handle_slot(asx_cleanup_handle handle) {
    return (uint32_t)(handle & ASX_CLEANUP_HANDLE_SLOT_MASK);
}

static uint16_t cleanup_handle_generation(asx_cleanup_handle handle) {
    return (uint16_t)(handle >> 16);
}

static asx_cleanup_handle cleanup_make_handle(uint32_t slot, uint16_t generation) {
    return ((asx_cleanup_handle)generation << 16) | (asx_cleanup_handle)slot;
}

static uint16_t cleanup_next_generation(uint16_t current) {
    current++;
    if (current == 0u) { current = 1u; }
    return current;
}

void asx_cleanup_init(asx_cleanup_stack *stack) {
    uint32_t i;
    if (stack == NULL) return;
    for (i = 0; i < ASX_CLEANUP_STACK_CAPACITY; i++) {
        stack->fns[i] = NULL;
        stack->polls[i] = NULL;
        stack->data[i] = NULL;
        stack->generations[i] = 0u;
    }
    stack->count = 0;
    stack->drained = 0;
    stack->draining = 0;
}

/* An entry is live while it holds a sync or an async action. */
static int cleanup_entry_live(const asx_cleanup_stack *stack, uint32_t slot) {
    return stack->fns[slot] != NULL || stack->polls[slot] != NULL;
}

/* Keep depth tight after a pop: drop resolved entries from the top. */
static void cleanup_tighten(asx_cleanup_stack *stack) {
    while (stack->count > 0u && !cleanup_entry_live(stack, stack->count - 1u)) {
        ASX_CHECKPOINT_WAIVER("bounded: stack->count decrements to zero");
        stack->count--;
    }
}

/* Shared push: exactly one of fn / poll_fn is non-NULL. */
static asx_status cleanup_push_entry(asx_cleanup_stack *stack, asx_cleanup_fn fn,
                                     asx_cleanup_poll_fn poll_fn, void *user_data,
                                     asx_cleanup_handle *out_handle) {
    uint32_t idx;

    if (stack == NULL || out_handle == NULL) return ASX_E_INVALID_ARGUMENT;
    if (stack->draining) return ASX_E_INVALID_STATE;

    if (stack->drained && stack->count == 0) {
        /* Allow deterministic reuse after a prior full drain. */
        stack->drained = 0;
    }

    if (stack->count >= ASX_CLEANUP_STACK_CAPACITY) return ASX_E_RESOURCE_EXHAUSTED;

    idx = stack->count++;
    stack->generations[idx] = cleanup_next_generation(stack->generations[idx]);
    stack->fns[idx] = fn;
    stack->polls[idx] = poll_fn;
    stack->data[idx] = user_data;
    *out_handle = cleanup_make_handle(idx, stack->generations[idx]);
    return ASX_OK;
}

asx_status asx_cleanup_push(asx_cleanup_stack *stack, asx_cleanup_fn fn, void *user_data,
                            asx_cleanup_handle *out_handle) {
    if (fn == NULL) return ASX_E_INVALID_ARGUMENT;
    return cleanup_push_entry(stack, fn, NULL, user_data, out_handle);
}

asx_status asx_cleanup_push_async(asx_cleanup_stack *stack, asx_cleanup_poll_fn poll_fn,
                                  void *user_data, asx_cleanup_handle *out_handle) {
    if (poll_fn == NULL) return ASX_E_INVALID_ARGUMENT;
    return cleanup_push_entry(stack, NULL, poll_fn, user_data, out_handle);
}

asx_status asx_cleanup_pop(asx_cleanup_stack *stack, asx_cleanup_handle handle) {
    uint32_t slot;
    uint16_t generation;

    if (stack == NULL) return ASX_E_INVALID_ARGUMENT;
    slot = cleanup_handle_slot(handle);
    generation = cleanup_handle_generation(handle);

    if (slot >= stack->count) return ASX_E_NOT_FOUND;
    if (!cleanup_entry_live(stack, slot)) return ASX_E_NOT_FOUND;
    if (stack->generations[slot] != generation) return ASX_E_NOT_FOUND;

    /* Mark as resolved — will be skipped during drain */
    stack->fns[slot] = NULL;
    stack->polls[slot] = NULL;
    stack->data[slot] = NULL;

    /* Keep depth tight for normal LIFO pop sequences. */
    cleanup_tighten(stack);

    return ASX_OK;
}

void asx_cleanup_drain(asx_cleanup_stack *stack) {
    if (stack == NULL) return;
    if (stack->drained || stack->draining) return;
    stack->draining = 1;

    /* Retire each entry before calling it: recursive drain must not call
     * it again, and callback-time pops may tighten the remaining depth. An
     * async entry on top ends the drain: it runs as a task, not inline. */
    while (stack->count > 0u) {
        uint32_t i = stack->count - 1u;
        asx_cleanup_fn fn;
        void *data;
        /* ASX_CHECKPOINT_WAIVER: bounded by count <= ASX_CLEANUP_STACK_CAPACITY */
        if (stack->polls[i] != NULL) break;
        stack->count--;
        fn = stack->fns[i];
        data = stack->data[i];
        stack->fns[i] = NULL;
        stack->data[i] = NULL;
        if (fn != NULL) fn(data);
    }

    stack->draining = 0;
    if (stack->count == 0u) stack->drained = 1;
}

asx_status asx_cleanup_take_async(asx_cleanup_stack *stack, asx_cleanup_poll_fn *out_poll,
                                  void **out_data) {
    uint32_t top;

    if (stack == NULL || out_poll == NULL || out_data == NULL) return ASX_E_INVALID_ARGUMENT;
    if (stack->draining || stack->count == 0u) return ASX_E_NOT_FOUND;
    top = stack->count - 1u;
    if (stack->polls[top] == NULL) return ASX_E_NOT_FOUND;

    *out_poll = stack->polls[top];
    *out_data = stack->data[top];
    stack->polls[top] = NULL;
    stack->data[top] = NULL;
    stack->count--;
    cleanup_tighten(stack);
    if (stack->count == 0u) stack->drained = 1;
    return ASX_OK;
}

uint32_t asx_cleanup_pending(const asx_cleanup_stack *stack) {
    uint32_t i, pending;
    if (stack == NULL) return 0;
    pending = 0;
    for (i = 0; i < stack->count; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: count <= ASX_CLEANUP_STACK_CAPACITY");
        if (cleanup_entry_live(stack, i)) { pending++; }
    }
    return pending;
}

uint32_t asx_cleanup_async_pending(const asx_cleanup_stack *stack) {
    uint32_t i, pending;
    if (stack == NULL) return 0;
    pending = 0;
    for (i = 0; i < stack->count; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: count <= ASX_CLEANUP_STACK_CAPACITY");
        if (stack->polls[i] != NULL) { pending++; }
    }
    return pending;
}
