/*
 * cx.c — capability context implementation
 *
 * Zero dynamic allocation. All Cx state is caller-owned or
 * borrowed from the runtime's static tables.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/cx/cx.h>
#include <asx/runtime/runtime.h>
#include <string.h>
#include <stddef.h>

/* Simple splitmix64-style step for deterministic entropy */
static uint64_t cx_entropy_step(uint64_t *state)
{
    uint64_t z = (*state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

/* ------------------------------------------------------------------ */
/* Cx lifecycle                                                        */
/* ------------------------------------------------------------------ */

static uint32_t g_cx_generation = 1u;

asx_status asx_cx_init(
    asx_cx *cx,
    asx_region_id region_id,
    asx_task_id task_id,
    asx_cap_flags caps)
{
    if (cx == NULL)
        return ASX_E_INVALID_ARGUMENT;
    if (region_id == ASX_INVALID_ID)
        return ASX_E_INVALID_ARGUMENT;

    memset(cx, 0, sizeof(*cx));
    cx->region_id     = region_id;
    cx->task_id       = task_id;
    cx->caps          = caps;
    cx->budget        = NULL;
    cx->clock         = NULL;
    cx->entropy_state = NULL;
    cx->generation    = g_cx_generation++;
    return ASX_OK;
}

asx_status asx_cx_narrow(
    const asx_cx *parent,
    asx_cx *child,
    asx_cap_flags child_caps)
{
    if (parent == NULL || child == NULL)
        return ASX_E_INVALID_ARGUMENT;
    if (!asx_cx_is_valid(parent))
        return ASX_E_INVALID_STATE;

    /* Child cannot have caps that parent doesn't */
    if ((child_caps & ~parent->caps) != 0u)
        return ASX_E_INVALID_ARGUMENT;

    memset(child, 0, sizeof(*child));
    child->region_id     = parent->region_id;
    child->task_id       = parent->task_id;
    child->caps          = child_caps;
    child->budget        = parent->budget;
    child->clock         = parent->clock;
    child->entropy_state = parent->entropy_state;
    child->generation    = g_cx_generation++;
    return ASX_OK;
}

void asx_cx_invalidate(asx_cx *cx)
{
    if (cx == NULL) return;
    memset(cx, 0, sizeof(*cx));
}

int asx_cx_is_valid(const asx_cx *cx)
{
    if (cx == NULL) return 0;
    return cx->generation != 0u && cx->region_id != ASX_INVALID_ID;
}

/* ------------------------------------------------------------------ */
/* Identity accessors                                                  */
/* ------------------------------------------------------------------ */

asx_region_id asx_cx_region(const asx_cx *cx)
{
    if (cx == NULL) return ASX_INVALID_ID;
    return cx->region_id;
}

asx_task_id asx_cx_task(const asx_cx *cx)
{
    if (cx == NULL) return ASX_INVALID_ID;
    return cx->task_id;
}

/* ------------------------------------------------------------------ */
/* Capability queries                                                  */
/* ------------------------------------------------------------------ */

int asx_cx_has_cap(const asx_cx *cx, asx_cap_flags cap)
{
    if (cx == NULL) return 0;
    return (cx->caps & cap) == cap;
}

asx_cap_flags asx_cx_caps(const asx_cx *cx)
{
    if (cx == NULL) return ASX_CAP_NONE;
    return cx->caps;
}

/* ------------------------------------------------------------------ */
/* Budget access                                                       */
/* ------------------------------------------------------------------ */

asx_status asx_cx_bind_budget(asx_cx *cx, asx_budget *budget)
{
    if (cx == NULL || budget == NULL)
        return ASX_E_INVALID_ARGUMENT;
    if (!asx_cx_is_valid(cx))
        return ASX_E_INVALID_STATE;
    if (!asx_cx_has_cap(cx, ASX_CAP_BUDGET_READ))
        return ASX_E_PERMISSION_DENIED;

    cx->budget = budget;
    return ASX_OK;
}

const asx_budget *asx_cx_budget(const asx_cx *cx)
{
    if (cx == NULL || !asx_cx_has_cap(cx, ASX_CAP_BUDGET_READ))
        return NULL;
    return cx->budget;
}

asx_status asx_cx_consume_poll(asx_cx *cx)
{
    if (cx == NULL)
        return ASX_E_INVALID_ARGUMENT;
    if (!asx_cx_has_cap(cx, ASX_CAP_BUDGET_CONSUME))
        return ASX_E_PERMISSION_DENIED;
    if (cx->budget == NULL)
        return ASX_OK; /* no budget bound = unlimited */

    if (asx_budget_consume_poll(cx->budget) == 0u)
        return ASX_E_POLL_BUDGET_EXHAUSTED;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Clock access                                                        */
/* ------------------------------------------------------------------ */

asx_status asx_cx_bind_clock(asx_cx *cx, asx_time *clock)
{
    if (cx == NULL || clock == NULL)
        return ASX_E_INVALID_ARGUMENT;
    if (!asx_cx_is_valid(cx))
        return ASX_E_INVALID_STATE;
    if (!asx_cx_has_cap(cx, ASX_CAP_CLOCK_READ))
        return ASX_E_PERMISSION_DENIED;

    cx->clock = clock;
    return ASX_OK;
}

asx_time asx_cx_now(const asx_cx *cx)
{
    if (cx == NULL || !asx_cx_has_cap(cx, ASX_CAP_CLOCK_READ))
        return 0u;
    if (cx->clock != NULL)
        return *cx->clock;
    return 0u;  /* no clock bound */
}

/* ------------------------------------------------------------------ */
/* Entropy access                                                      */
/* ------------------------------------------------------------------ */

asx_status asx_cx_bind_entropy(asx_cx *cx, uint64_t *entropy_state)
{
    if (cx == NULL || entropy_state == NULL)
        return ASX_E_INVALID_ARGUMENT;
    if (!asx_cx_is_valid(cx))
        return ASX_E_INVALID_STATE;
    if (!asx_cx_has_cap(cx, ASX_CAP_ENTROPY))
        return ASX_E_PERMISSION_DENIED;

    cx->entropy_state = entropy_state;
    return ASX_OK;
}

uint64_t asx_cx_random_u64(asx_cx *cx)
{
    if (cx == NULL || !asx_cx_has_cap(cx, ASX_CAP_ENTROPY))
        return 0u;
    if (cx->entropy_state == NULL)
        return 0u;
    return cx_entropy_step(cx->entropy_state);
}

/* ------------------------------------------------------------------ */
/* Cancellation checkpoint                                             */
/* ------------------------------------------------------------------ */

int asx_cx_is_cancelled(const asx_cx *cx)
{
    asx_task_state state;
    if (cx == NULL || !asx_cx_has_cap(cx, ASX_CAP_CANCEL_CHECK))
        return 0;
    if (cx->task_id == ASX_INVALID_ID)
        return 0;
    if (asx_task_get_state(cx->task_id, &state) != ASX_OK)
        return 0;
    return state >= ASX_TASK_CANCEL_REQUESTED;
}

asx_status asx_cx_checkpoint(asx_cx *cx)
{
    if (cx == NULL)
        return ASX_E_INVALID_ARGUMENT;

    /* Check cancellation first */
    if (asx_cx_is_cancelled(cx))
        return ASX_E_CANCELLED;

    /* Consume a poll tick if budget is bound */
    if (cx->budget != NULL && asx_cx_has_cap(cx, ASX_CAP_BUDGET_CONSUME)) {
        if (asx_budget_consume_poll(cx->budget) == 0u)
            return ASX_E_POLL_BUDGET_EXHAUSTED;
    }

    return ASX_OK;
}
