/*
 * runtime_internal.h — shared internal state for walking skeleton runtime
 *
 * NOT part of the public API. Used only by runtime .c translation units.
 * Phase 3 will replace static arenas with hook-backed dynamic allocation.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_RUNTIME_INTERNAL_H
#define ASX_RUNTIME_INTERNAL_H

#include <asx/asx_ids.h>
#include <asx/asx_status.h>
#include <asx/core/outcome.h>
#include <asx/core/cleanup.h>
#include <asx/core/cancel.h>
#include <asx/runtime/runtime.h>

/* -------------------------------------------------------------------
 * Arena slot types (walking skeleton: fixed-size)
 * ------------------------------------------------------------------- */

typedef struct {
    asx_region_state   state;
    uint32_t           task_count;     /* live (non-completed) tasks */
    uint32_t           task_total;     /* total spawned tasks */
    uint16_t           generation;     /* increments on slot reclaim */
    int                alive;          /* 1 if slot in use */
    int                poisoned;       /* 1 if region has been poisoned (containment) */
    asx_cleanup_stack  cleanup;        /* LIFO cleanup for finalization */
    uint8_t            capture_arena[ASX_REGION_CAPTURE_ARENA_BYTES];
    uint32_t           capture_used;
} asx_region_slot;

typedef struct {
    asx_task_state   state;
    asx_region_id    region;
    asx_task_poll_fn poll_fn;
    void            *user_data;
    asx_outcome      outcome;
    uint16_t         generation;     /* increments on slot reclaim */
    int              alive;
    void            *captured_state;
    uint32_t         captured_size;
    asx_task_state_dtor_fn captured_dtor;
    /* Cancellation tracking (bd-2cw.3) */
    asx_cancel_phase   cancel_phase;
    asx_cancel_reason  cancel_reason;
    uint32_t           cancel_epoch;
    uint32_t           cleanup_polls_remaining;
    int                cancel_pending;  /* 1 if cancel signal delivered */
} asx_task_slot;

typedef struct {
    asx_obligation_state state;
    asx_region_id        region;
    uint16_t             generation;
    int                  alive;
} asx_obligation_slot;

/* -------------------------------------------------------------------
 * Global arenas (defined in lifecycle.c)
 * ------------------------------------------------------------------- */

extern asx_region_slot      g_regions[ASX_MAX_REGIONS];
extern uint32_t             g_region_count;

extern asx_task_slot        g_tasks[ASX_MAX_TASKS];
extern uint32_t             g_task_count;

extern asx_obligation_slot  g_obligations[ASX_MAX_OBLIGATIONS];
extern uint32_t             g_obligation_count;

/* -------------------------------------------------------------------
 * Shared lookup functions (generation-safe, used across TUs)
 * ------------------------------------------------------------------- */

ASX_MUST_USE asx_status asx_region_slot_lookup(asx_region_id id, asx_region_slot **out);
ASX_MUST_USE asx_status asx_task_slot_lookup(asx_task_id id, asx_task_slot **out);
ASX_MUST_USE asx_status asx_obligation_slot_lookup(asx_obligation_id id,
                                                   asx_obligation_slot **out);

#endif /* ASX_RUNTIME_INTERNAL_H */
