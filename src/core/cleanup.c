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
        stack->data[i] = NULL;
        stack->generations[i] = 0u;
    }
    stack->count = 0;
    stack->drained = 0;
}

asx_status asx_cleanup_push(asx_cleanup_stack *stack, asx_cleanup_fn fn, void *user_data,
                            asx_cleanup_handle *out_handle) {
    uint32_t idx;

    if (stack == NULL || fn == NULL || out_handle == NULL) return ASX_E_INVALID_ARGUMENT;

    if (stack->drained && stack->count == 0) {
        /* Allow deterministic reuse after a prior full drain. */
        stack->drained = 0;
    }

    if (stack->count >= ASX_CLEANUP_STACK_CAPACITY) return ASX_E_RESOURCE_EXHAUSTED;

    idx = stack->count++;
    stack->generations[idx] = cleanup_next_generation(stack->generations[idx]);
    stack->fns[idx] = fn;
    stack->data[idx] = user_data;
    *out_handle = cleanup_make_handle(idx, stack->generations[idx]);
    return ASX_OK;
}

asx_status asx_cleanup_pop(asx_cleanup_stack *stack, asx_cleanup_handle handle) {
    uint32_t slot;
    uint16_t generation;

    if (stack == NULL) return ASX_E_INVALID_ARGUMENT;
    slot = cleanup_handle_slot(handle);
    generation = cleanup_handle_generation(handle);

    if (slot >= stack->count) return ASX_E_NOT_FOUND;
    if (stack->fns[slot] == NULL) return ASX_E_NOT_FOUND;
    if (stack->generations[slot] != generation) return ASX_E_NOT_FOUND;

    /* Mark as resolved — will be skipped during drain */
    stack->fns[slot] = NULL;
    stack->data[slot] = NULL;

    /* Keep depth tight for normal LIFO pop sequences. */
    while (stack->count > 0u && stack->fns[stack->count - 1u] == NULL) {
        ASX_CHECKPOINT_WAIVER("bounded: stack->count decrements to zero");
        stack->count--;
    }

    return ASX_OK;
}

void asx_cleanup_drain(asx_cleanup_stack *stack) {
    uint32_t i;
    if (stack == NULL) return;
    if (stack->drained) return;

    /* Drain in LIFO order: highest index first */
    i = stack->count;
    while (i > 0) {
        /* ASX_CHECKPOINT_WAIVER: bounded by count <= ASX_CLEANUP_STACK_CAPACITY */
        i--;
        if (stack->fns[i] != NULL) {
            stack->fns[i](stack->data[i]);
            stack->fns[i] = NULL;
            stack->data[i] = NULL;
        }
    }

    stack->count = 0;
    stack->drained = 1;
}

uint32_t asx_cleanup_pending(const asx_cleanup_stack *stack) {
    uint32_t i, pending;
    if (stack == NULL) return 0;
    pending = 0;
    for (i = 0; i < stack->count; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: count <= ASX_CLEANUP_STACK_CAPACITY");
        if (stack->fns[i] != NULL) { pending++; }
    }
    return pending;
}
