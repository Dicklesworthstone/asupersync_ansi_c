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
#include "runtime_internal.h"

/* -------------------------------------------------------------------
 * Global arenas (walking skeleton: fixed-size, no dynamic allocation)
 * ------------------------------------------------------------------- */

asx_region_slot g_regions[ASX_MAX_REGIONS];
uint32_t        g_region_count;

asx_task_slot   g_tasks[ASX_MAX_TASKS];
uint32_t        g_task_count;

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
    }
    g_task_count = 0;
    for (i = 0; i < ASX_MAX_OBLIGATIONS; i++) {
        g_obligations[i].state      = ASX_OBLIGATION_RESERVED;
        g_obligations[i].region     = ASX_INVALID_ID;
        g_obligations[i].generation = 0;
        g_obligations[i].alive      = 0;
    }
    g_obligation_count = 0;
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
    asx_cleanup_init(&g_regions[idx].cleanup);

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

    r->task_count++;
    r->task_total++;

    *out_id = asx_handle_pack(ASX_TYPE_TASK,
                              (uint16_t)(1u << (unsigned)ASX_TASK_CREATED),
                              asx_handle_pack_index(
                                  g_tasks[idx].generation,
                                  (uint16_t)idx));
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
    return ASX_OK;
}

asx_status asx_obligation_commit(asx_obligation_id id)
{
    asx_obligation_slot *o;
    asx_status st;

    st = asx_obligation_slot_lookup(id, &o);
    if (st != ASX_OK) return st;

    st = asx_obligation_transition_check(o->state, ASX_OBLIGATION_COMMITTED);
    if (st != ASX_OK) return st;

    o->state = ASX_OBLIGATION_COMMITTED;
    return ASX_OK;
}

asx_status asx_obligation_abort(asx_obligation_id id)
{
    asx_obligation_slot *o;
    asx_status st;

    st = asx_obligation_slot_lookup(id, &o);
    if (st != ASX_OK) return st;

    st = asx_obligation_transition_check(o->state, ASX_OBLIGATION_ABORTED);
    if (st != ASX_OK) return st;

    o->state = ASX_OBLIGATION_ABORTED;
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
