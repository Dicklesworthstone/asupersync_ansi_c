/*
 * lifecycle.c — region/task/obligation lifecycle engine (walking skeleton)
 *
 * Provides generation-safe handle validation, cleanup-stack primitives,
 * and region/task lifecycle operations.
 *
 * Uses fixed-size static arenas. Phase 3 will replace with
 * hook-backed dynamic allocation (bd-hwb.3, bd-2cw.1).
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <asx/runtime/runtime.h>
#include <asx/core/transition.h>
#include <asx/core/ghost.h>
#include <string.h>
#include "runtime_internal.h"

/* -------------------------------------------------------------------
 * Global arenas (walking skeleton: fixed-size, no dynamic allocation)
 * ------------------------------------------------------------------- */

asx_region_slot g_regions[ASX_MAX_REGIONS];
uint32_t        g_region_count;

asx_task_slot   g_tasks[ASX_MAX_TASKS];
uint32_t        g_task_count;

asx_obligation_slot g_obligations[ASX_MAX_OBLIGATIONS];
uint32_t            g_obligation_count;

/* -------------------------------------------------------------------
 * Reset (test support)
 * ------------------------------------------------------------------- */

void asx_runtime_reset(void)
{
    uint32_t i;
    for (i = 0; i < ASX_MAX_REGIONS; i++) {
        g_regions[i].state      = ASX_REGION_OPEN;
        g_regions[i].task_count = 0;
        g_regions[i].task_total = 0;
        g_regions[i].generation = 0;
        g_regions[i].alive      = 0;
        asx_cleanup_init(&g_regions[i].cleanup);
        g_regions[i].capture_used = 0;
    }
    g_region_count = 0;
    for (i = 0; i < ASX_MAX_TASKS; i++) {
        g_tasks[i].state      = ASX_TASK_CREATED;
        g_tasks[i].region     = ASX_INVALID_ID;
        g_tasks[i].poll_fn    = NULL;
        g_tasks[i].user_data  = NULL;
        g_tasks[i].outcome    = asx_outcome_make(ASX_OUTCOME_OK);
        g_tasks[i].generation = 0;
        g_tasks[i].alive      = 0;
        g_tasks[i].captured_state = NULL;
        g_tasks[i].captured_size = 0;
        g_tasks[i].captured_dtor = NULL;
    }
    g_task_count = 0;
    for (i = 0; i < ASX_MAX_OBLIGATIONS; i++) {
        g_obligations[i].state      = ASX_OBLIGATION_RESERVED;
        g_obligations[i].region     = ASX_INVALID_ID;
        g_obligations[i].generation = 0;
        g_obligations[i].alive      = 0;
    }
    g_obligation_count = 0;

    /* Reset ghost safety monitors */
    asx_ghost_reset();
}

/* -------------------------------------------------------------------
 * Generation-safe lookup helpers (shared across TUs)
 *
 * Returns ASX_OK on success, ASX_E_STALE_HANDLE when the slot is
 * alive but the handle's generation doesn't match, or ASX_E_NOT_FOUND
 * for all other failures (invalid handle, wrong type tag, dead slot).
 * ------------------------------------------------------------------- */

asx_status asx_region_slot_lookup(asx_region_id id, asx_region_slot **out)
{
    uint16_t tag, slot_idx, handle_gen;

    *out = NULL;
    if (!asx_handle_is_valid(id)) return ASX_E_NOT_FOUND;

    tag = asx_handle_type_tag(id);
    if (tag != ASX_TYPE_REGION) return ASX_E_NOT_FOUND;

    slot_idx = asx_handle_slot(id);
    if (slot_idx >= ASX_MAX_REGIONS) return ASX_E_NOT_FOUND;
    if (!g_regions[slot_idx].alive) return ASX_E_NOT_FOUND;

    handle_gen = asx_handle_generation(id);
    if (handle_gen != g_regions[slot_idx].generation) return ASX_E_STALE_HANDLE;

    *out = &g_regions[slot_idx];
    return ASX_OK;
}

asx_status asx_task_slot_lookup(asx_task_id id, asx_task_slot **out)
{
    uint16_t tag, slot_idx, handle_gen;

    *out = NULL;
    if (!asx_handle_is_valid(id)) return ASX_E_NOT_FOUND;

    tag = asx_handle_type_tag(id);
    if (tag != ASX_TYPE_TASK) return ASX_E_NOT_FOUND;

    slot_idx = asx_handle_slot(id);
    if (slot_idx >= ASX_MAX_TASKS) return ASX_E_NOT_FOUND;
    if (!g_tasks[slot_idx].alive) return ASX_E_NOT_FOUND;

    handle_gen = asx_handle_generation(id);
    if (handle_gen != g_tasks[slot_idx].generation) return ASX_E_STALE_HANDLE;

    *out = &g_tasks[slot_idx];
    return ASX_OK;
}

static uint32_t asx_align_up_u32(uint32_t value, uint32_t align)
{
    uint32_t rem = value % align;
    if (rem == 0u) return value;
    return value + (align - rem);
}

static void *asx_region_capture_alloc(asx_region_slot *region, uint32_t size, uint32_t *old_used_out)
{
    uint32_t start;
    uint32_t aligned_size;
    uint32_t end;

    if (size == 0u || old_used_out == NULL) return NULL;

    *old_used_out = region->capture_used;
    start = asx_align_up_u32(region->capture_used, 8u);
    aligned_size = asx_align_up_u32(size, 8u);

    if (start > ASX_REGION_CAPTURE_ARENA_BYTES) return NULL;
    if (aligned_size > (ASX_REGION_CAPTURE_ARENA_BYTES - start)) return NULL;

    end = start + aligned_size;
    if (end > ASX_REGION_CAPTURE_ARENA_BYTES) return NULL;

    region->capture_used = end;
    return (void *)&region->capture_arena[start];
}

/* -------------------------------------------------------------------
 * Region lifecycle
 * ------------------------------------------------------------------- */

asx_status asx_region_open(asx_region_id *out_id)
{
    uint32_t idx;
    int reclaim;

    if (out_id == NULL) return ASX_E_INVALID_ARGUMENT;

    /* Scan for a recyclable slot: unused (alive=0) or CLOSED with no tasks.
     * When ASX_DEBUG_QUARANTINE is defined, CLOSED slots are never recycled
     * so that any stale-handle dereference surfaces as RESOURCE_EXHAUSTED
     * instead of silently aliasing a new region. Zero-cost when disabled. */
    reclaim = 0;
    for (idx = 0; idx < ASX_MAX_REGIONS; idx++) {
        if (!g_regions[idx].alive) break;
#ifndef ASX_DEBUG_QUARANTINE
        if (g_regions[idx].state == ASX_REGION_CLOSED
            && g_regions[idx].task_count == 0) {
            reclaim = 1;
            break;
        }
#endif
    }
    if (idx >= ASX_MAX_REGIONS) return ASX_E_RESOURCE_EXHAUSTED;

    /* Increment generation on slot reclaim to invalidate stale handles */
    if (reclaim) {
        g_regions[idx].generation++;
    }

    g_regions[idx].state      = ASX_REGION_OPEN;
    g_regions[idx].task_count = 0;
    g_regions[idx].task_total = 0;
    g_regions[idx].alive      = 1;
    g_regions[idx].poisoned   = 0;
    asx_cleanup_init(&g_regions[idx].cleanup);
    g_regions[idx].capture_used = 0;

    if (idx >= g_region_count) {
        g_region_count = idx + 1;
    }

    *out_id = asx_handle_pack(ASX_TYPE_REGION,
                              (uint16_t)(1u << (unsigned)ASX_REGION_OPEN),
                              asx_handle_pack_index(
                                  g_regions[idx].generation,
                                  (uint16_t)idx));
    return ASX_OK;
}

asx_status asx_region_close(asx_region_id id)
{
    asx_region_slot *r;
    asx_status st;

    st = asx_region_slot_lookup(id, &r);
    if (st != ASX_OK) return st;
    if (r->poisoned) return ASX_E_REGION_POISONED;

    /* Ghost protocol monitor: record transition for diagnostics */
    asx_ghost_check_region_transition(id, r->state, ASX_REGION_CLOSING);

    /* Transition Open -> Closing */
    st = asx_region_transition_check(r->state, ASX_REGION_CLOSING);
    if (st != ASX_OK) return st;

    r->state = ASX_REGION_CLOSING;
    return ASX_OK;
}

asx_status asx_region_get_state(asx_region_id id,
                                asx_region_state *out_state)
{
    asx_region_slot *r;
    asx_status st;

    if (out_state == NULL) return ASX_E_INVALID_ARGUMENT;

    st = asx_region_slot_lookup(id, &r);
    if (st != ASX_OK) return st;

    *out_state = r->state;
    return ASX_OK;
}

asx_status asx_region_poison(asx_region_id id)
{
    asx_region_slot *r;
    asx_status st;

    st = asx_region_slot_lookup(id, &r);
    if (st != ASX_OK) return st;

    r->poisoned = 1;
    return ASX_OK;
}

asx_status asx_region_is_poisoned(asx_region_id id, int *out)
{
    asx_region_slot *r;
    asx_status st;

    if (out == NULL) return ASX_E_INVALID_ARGUMENT;

    st = asx_region_slot_lookup(id, &r);
    if (st != ASX_OK) return st;

    *out = r->poisoned;
    return ASX_OK;
}

/* -------------------------------------------------------------------
 * Task lifecycle
 * ------------------------------------------------------------------- */

asx_status asx_task_spawn(asx_region_id region,
                          asx_task_poll_fn poll_fn,
                          void *user_data,
                          asx_task_id *out_id)
{
    asx_region_slot *r;
    asx_status st;
    uint32_t idx;

    if (out_id == NULL) return ASX_E_INVALID_ARGUMENT;
    if (poll_fn == NULL) return ASX_E_INVALID_ARGUMENT;

    st = asx_region_slot_lookup(region, &r);
    if (st != ASX_OK) return st;
    if (r->poisoned) return ASX_E_REGION_POISONED;

    /* Only open regions can spawn tasks */
    if (!asx_region_can_spawn(r->state)) return ASX_E_REGION_NOT_OPEN;

    if (g_task_count >= ASX_MAX_TASKS) return ASX_E_RESOURCE_EXHAUSTED;

    idx = g_task_count++;
    g_tasks[idx].state      = ASX_TASK_CREATED;
    g_tasks[idx].region     = region;
    g_tasks[idx].poll_fn    = poll_fn;
    g_tasks[idx].user_data  = user_data;
    g_tasks[idx].outcome    = asx_outcome_make(ASX_OUTCOME_OK);
    g_tasks[idx].generation = 0;
    g_tasks[idx].alive      = 1;
    g_tasks[idx].captured_state = NULL;
    g_tasks[idx].captured_size = 0;
    g_tasks[idx].captured_dtor = NULL;

    r->task_count++;
    r->task_total++;

    *out_id = asx_handle_pack(ASX_TYPE_TASK,
                              (uint16_t)(1u << (unsigned)ASX_TASK_CREATED),
                              asx_handle_pack_index(
                                  g_tasks[idx].generation,
                                  (uint16_t)idx));
    return ASX_OK;
}

asx_status asx_task_spawn_captured(asx_region_id region,
                                   asx_task_poll_fn poll_fn,
                                   uint32_t state_size,
                                   asx_task_state_dtor_fn state_dtor,
                                   asx_task_id *out_id,
                                   void **out_state)
{
    asx_region_slot *r;
    asx_task_slot *t;
    asx_status st;
    void *captured;
    uint32_t old_capture_used;

    if (out_id == NULL || out_state == NULL) return ASX_E_INVALID_ARGUMENT;
    if (poll_fn == NULL || state_size == 0u) return ASX_E_INVALID_ARGUMENT;

    st = asx_region_slot_lookup(region, &r);
    if (st != ASX_OK) return st;
    if (!asx_region_can_spawn(r->state)) return ASX_E_REGION_NOT_OPEN;

    captured = asx_region_capture_alloc(r, state_size, &old_capture_used);
    if (captured == NULL) return ASX_E_RESOURCE_EXHAUSTED;
    memset(captured, 0, state_size);

    st = asx_task_spawn(region, poll_fn, captured, out_id);
    if (st != ASX_OK) {
        r->capture_used = old_capture_used;
        return st;
    }

    st = asx_task_slot_lookup(*out_id, &t);
    if (st != ASX_OK) {
        r->capture_used = old_capture_used;
        return st;
    }

    t->captured_state = captured;
    t->captured_size = state_size;
    t->captured_dtor = state_dtor;
    *out_state = captured;
    return ASX_OK;
}

asx_status asx_task_get_state(asx_task_id id,
                              asx_task_state *out_state)
{
    asx_task_slot *t;
    asx_status st;

    if (out_state == NULL) return ASX_E_INVALID_ARGUMENT;

    st = asx_task_slot_lookup(id, &t);
    if (st != ASX_OK) return st;

    *out_state = t->state;
    return ASX_OK;
}

asx_status asx_task_get_outcome(asx_task_id id,
                                asx_outcome *out_outcome)
{
    asx_task_slot *t;
    asx_status st;

    if (out_outcome == NULL) return ASX_E_INVALID_ARGUMENT;

    st = asx_task_slot_lookup(id, &t);
    if (st != ASX_OK) return st;
    if (!asx_task_is_terminal(t->state)) return ASX_E_TASK_NOT_COMPLETED;

    *out_outcome = t->outcome;
    return ASX_OK;
}

/* -------------------------------------------------------------------
 * Obligation lifecycle
 * ------------------------------------------------------------------- */

asx_status asx_obligation_slot_lookup(asx_obligation_id id,
                                       asx_obligation_slot **out)
{
    uint16_t tag, slot_idx, handle_gen;

    *out = NULL;
    if (!asx_handle_is_valid(id)) return ASX_E_NOT_FOUND;

    tag = asx_handle_type_tag(id);
    if (tag != ASX_TYPE_OBLIGATION) return ASX_E_NOT_FOUND;

    slot_idx = asx_handle_slot(id);
    if (slot_idx >= ASX_MAX_OBLIGATIONS) return ASX_E_NOT_FOUND;
    if (!g_obligations[slot_idx].alive) return ASX_E_NOT_FOUND;

    handle_gen = asx_handle_generation(id);
    if (handle_gen != g_obligations[slot_idx].generation)
        return ASX_E_STALE_HANDLE;

    *out = &g_obligations[slot_idx];
    return ASX_OK;
}

asx_status asx_obligation_reserve(asx_region_id region,
                                   asx_obligation_id *out_id)
{
    asx_region_slot *r;
    asx_status st;
    uint32_t idx;

    if (out_id == NULL) return ASX_E_INVALID_ARGUMENT;

    st = asx_region_slot_lookup(region, &r);
    if (st != ASX_OK) return st;
    if (r->poisoned) return ASX_E_REGION_POISONED;

    /* Only open regions can reserve obligations */
    if (!asx_region_can_spawn(r->state)) return ASX_E_REGION_NOT_OPEN;

    if (g_obligation_count >= ASX_MAX_OBLIGATIONS)
        return ASX_E_RESOURCE_EXHAUSTED;

    idx = g_obligation_count++;
    g_obligations[idx].state      = ASX_OBLIGATION_RESERVED;
    g_obligations[idx].region     = region;
    g_obligations[idx].generation = 0;
    g_obligations[idx].alive      = 1;

    *out_id = asx_handle_pack(ASX_TYPE_OBLIGATION,
                               (uint16_t)(1u << (unsigned)ASX_OBLIGATION_RESERVED),
                               asx_handle_pack_index(
                                   g_obligations[idx].generation,
                                   (uint16_t)idx));

    /* Ghost linearity monitor: track obligation reservation */
    asx_ghost_obligation_reserved(*out_id);

    return ASX_OK;
}

asx_status asx_obligation_commit(asx_obligation_id id)
{
    asx_obligation_slot *o;
    asx_status st;

    st = asx_obligation_slot_lookup(id, &o);
    if (st != ASX_OK) return st;

    /* Ghost protocol monitor: validate obligation transition */
    (void)asx_ghost_check_obligation_transition(id, o->state, ASX_OBLIGATION_COMMITTED);

    st = asx_obligation_transition_check(o->state, ASX_OBLIGATION_COMMITTED);
    if (st != ASX_OK) return st;

    o->state = ASX_OBLIGATION_COMMITTED;

    /* Ghost linearity monitor: track obligation resolution */
    asx_ghost_obligation_resolved(id);

    return ASX_OK;
}

asx_status asx_obligation_abort(asx_obligation_id id)
{
    asx_obligation_slot *o;
    asx_status st;

    st = asx_obligation_slot_lookup(id, &o);
    if (st != ASX_OK) return st;

    /* Ghost protocol monitor: validate obligation transition */
    (void)asx_ghost_check_obligation_transition(id, o->state, ASX_OBLIGATION_ABORTED);

    st = asx_obligation_transition_check(o->state, ASX_OBLIGATION_ABORTED);
    if (st != ASX_OK) return st;

    o->state = ASX_OBLIGATION_ABORTED;

    /* Ghost linearity monitor: track obligation resolution */
    asx_ghost_obligation_resolved(id);

    return ASX_OK;
}

asx_status asx_obligation_get_state(asx_obligation_id id,
                                     asx_obligation_state *out_state)
{
    asx_obligation_slot *o;
    asx_status st;

    if (out_state == NULL) return ASX_E_INVALID_ARGUMENT;

    st = asx_obligation_slot_lookup(id, &o);
    if (st != ASX_OK) return st;

    *out_state = o->state;
    return ASX_OK;
}
