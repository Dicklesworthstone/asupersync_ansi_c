/*
 * timer_wheel.c — timer wheel with deterministic ordering and O(1) cancel
 *
 * Walking-skeleton implementation using a flat arena of timer slots.
 * Provides deterministic tie-break ordering (deadline ASC, insertion_seq ASC)
 * and O(1) cancel via generation-validated handles.
 *
 * Phase 5 will upgrade to a hierarchical 4-level wheel with occupied
 * bitmaps for O(1) skip optimization. The flat approach is correct and
 * sufficient for proving semantics.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx_config.h>
#include <asx/runtime/trace.h>
#include <asx/time/timer_wheel.h>
#include <string.h>

/* -------------------------------------------------------------------
 * Timer slot (internal)
 * ------------------------------------------------------------------- */

typedef struct {
    asx_time deadline;      /* when this timer fires */
    void *waker_data;       /* opaque callback data */
    uint64_t insertion_seq; /* monotonic tie-break key */
    uint32_t generation;    /* for stale-handle detection */
    int alive;              /* 1 if slot is live (not cancelled/fired) */
} asx_timer_slot;

/* -------------------------------------------------------------------
 * Timer wheel structure
 * ------------------------------------------------------------------- */

struct asx_timer_wheel {
    asx_timer_slot slots[ASX_MAX_TIMERS];
    uint32_t slot_count;      /* high-water mark for slot allocation */
    uint32_t active_count;    /* number of alive timers */
    uint64_t next_insertion;  /* monotonic insertion sequence */
    asx_time current_time;    /* last advanced-to time */
    uint64_t max_duration_ns; /* maximum allowed timer duration */
};

/* -------------------------------------------------------------------
 * Global singleton instance
 * ------------------------------------------------------------------- */

static asx_timer_wheel g_wheel;
static int g_wheel_initialized = 0;

static uint32_t asx_timer_next_generation(uint32_t current) {
    current++;
    if (current == 0u) { current = 1u; }
    return current;
}

static uint64_t asx_timer_trace_entity_id(const asx_timer_handle *handle) {
    return ((uint64_t)handle->slot << 32) | (uint64_t)handle->generation;
}

asx_timer_wheel *asx_timer_wheel_global(void) {
    if (!g_wheel_initialized) {
        asx_timer_wheel_init(&g_wheel);
        g_wheel_initialized = 1;
    }
    return &g_wheel;
}

/* -------------------------------------------------------------------
 * Init / Reset
 * ------------------------------------------------------------------- */

void asx_timer_wheel_init(asx_timer_wheel *wheel) {
    uint32_t i;
    if (wheel == NULL) return;

    for (i = 0; i < ASX_MAX_TIMERS; i++) {
        wheel->slots[i].deadline = 0;
        wheel->slots[i].waker_data = NULL;
        wheel->slots[i].insertion_seq = 0;
        wheel->slots[i].generation = 0;
        wheel->slots[i].alive = 0;
    }
    wheel->slot_count = 0;
    wheel->active_count = 0;
    wheel->next_insertion = 0;
    wheel->current_time = 0;
    wheel->max_duration_ns = ASX_TIMER_MAX_DURATION_NS;
}

void asx_timer_wheel_reset(asx_timer_wheel *wheel) {
    uint32_t i;

    if (wheel == NULL) return;

    for (i = 0; i < ASX_MAX_TIMERS; i++) {
        wheel->slots[i].deadline = 0;
        wheel->slots[i].waker_data = NULL;
        wheel->slots[i].insertion_seq = 0;
        /* Preserve stale-handle safety across reset epochs. */
        wheel->slots[i].generation = asx_timer_next_generation(wheel->slots[i].generation);
        wheel->slots[i].alive = 0;
    }
    wheel->slot_count = 0;
    wheel->active_count = 0;
    wheel->next_insertion = 0;
    wheel->current_time = 0;
    wheel->max_duration_ns = ASX_TIMER_MAX_DURATION_NS;
}

/* -------------------------------------------------------------------
 * Timer registration
 * ------------------------------------------------------------------- */

asx_status asx_timer_register(asx_timer_wheel *wheel, asx_time deadline, void *waker_data,
                              asx_timer_handle *out_handle) {
    uint32_t idx;
    uint64_t delta;

    if (wheel == NULL || out_handle == NULL) return ASX_E_INVALID_ARGUMENT;

    /* Validate duration */
    if (deadline > wheel->current_time) {
        delta = deadline - wheel->current_time;
        if (delta > wheel->max_duration_ns) { return ASX_E_TIMER_DURATION_EXCEEDED; }
    }

    /* Find a free slot: try recycling a dead slot first */
    idx = ASX_MAX_TIMERS; /* sentinel */
    {
        uint32_t i;
        for (i = 0; i < wheel->slot_count; i++) {
            ASX_CHECKPOINT_WAIVER("bounded: slot_count <= ASX_MAX_TIMERS");
            if (!wheel->slots[i].alive) {
                idx = i;
                break;
            }
        }
    }

    /* No dead slot found — allocate a new one */
    if (idx == ASX_MAX_TIMERS) {
        if (wheel->slot_count >= ASX_MAX_TIMERS) { return ASX_E_RESOURCE_EXHAUSTED; }
        idx = wheel->slot_count++;
    }

    wheel->slots[idx].deadline = deadline;
    wheel->slots[idx].waker_data = waker_data;
    wheel->slots[idx].insertion_seq = wheel->next_insertion++;
    wheel->slots[idx].generation = asx_timer_next_generation(wheel->slots[idx].generation);
    wheel->slots[idx].alive = 1;

    wheel->active_count++;

    out_handle->slot = idx;
    out_handle->generation = wheel->slots[idx].generation;
    asx_trace_emit(ASX_TRACE_TIMER_SET, asx_timer_trace_entity_id(out_handle), deadline);

    return ASX_OK;
}

/* -------------------------------------------------------------------
 * Timer cancellation — O(1)
 * ------------------------------------------------------------------- */

int asx_timer_cancel(asx_timer_wheel *wheel, const asx_timer_handle *handle) {
    asx_timer_slot *s;

    if (wheel == NULL || handle == NULL) return 0;
    if (handle->slot >= wheel->slot_count) return 0;

    s = &wheel->slots[handle->slot];

    /* Check generation and liveness */
    if (!s->alive) return 0;
    if (s->generation != handle->generation) return 0;

    /* Logical cancel: mark dead */
    s->alive = 0;
    if (wheel->active_count > 0) wheel->active_count--;
    asx_trace_emit(ASX_TRACE_TIMER_CANCEL, asx_timer_trace_entity_id(handle), s->deadline);

    return 1;
}

/* -------------------------------------------------------------------
 * Timer collection — deterministic ordering
 *
 * Collects all alive timers with deadline <= now.
 * Returns them sorted by (deadline ASC, insertion_seq ASC).
 * Uses insertion sort on the output for stability and simplicity.
 * ------------------------------------------------------------------- */

uint32_t asx_timer_collect_expired(asx_timer_wheel *wheel, asx_time now, void **out_wakers,
                                   uint32_t max_wakers) {
    uint32_t i, j, count;
    /* Collect candidates: (slot_index, deadline, insertion_seq) */
    uint32_t cand_idx[ASX_MAX_TIMERS];
    uint64_t cand_deadline[ASX_MAX_TIMERS];
    uint64_t cand_seq[ASX_MAX_TIMERS];

    if (wheel == NULL) return 0;

    /* Advance current time */
    if (now > wheel->current_time) { wheel->current_time = now; }

    if (out_wakers == NULL || max_wakers == 0) return 0;

    /* Scan for expired alive timers */
    count = 0;
    for (i = 0; i < wheel->slot_count && count < ASX_MAX_TIMERS; i++) {
        asx_timer_slot *s = &wheel->slots[i];
        if (!s->alive) continue;
        if (s->deadline <= now) {
            cand_idx[count] = i;
            cand_deadline[count] = s->deadline;
            cand_seq[count] = s->insertion_seq;
            count++;
        }
    }

    /* Sort by (deadline ASC, insertion_seq ASC) — insertion sort for stability */
    for (i = 1; i < count; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: count <= ASX_MAX_TIMERS");
        uint32_t tmp_idx = cand_idx[i];
        uint64_t tmp_dl = cand_deadline[i];
        uint64_t tmp_seq = cand_seq[i];
        j = i;
        while (j > 0 && (cand_deadline[j - 1] > tmp_dl ||
                         (cand_deadline[j - 1] == tmp_dl && cand_seq[j - 1] > tmp_seq))) {
            ASX_CHECKPOINT_WAIVER("bounded: j <= count <= ASX_MAX_TIMERS");
            cand_idx[j] = cand_idx[j - 1];
            cand_deadline[j] = cand_deadline[j - 1];
            cand_seq[j] = cand_seq[j - 1];
            j--;
        }
        cand_idx[j] = tmp_idx;
        cand_deadline[j] = tmp_dl;
        cand_seq[j] = tmp_seq;
    }

    /* Emit wakers in sorted order, up to max_wakers */
    if (count > max_wakers) count = max_wakers;

    for (i = 0; i < count; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: count <= max_wakers <= ASX_MAX_TIMERS");
        asx_timer_slot *s = &wheel->slots[cand_idx[i]];
        asx_timer_handle fired_handle;
        fired_handle.slot = cand_idx[i];
        fired_handle.generation = s->generation;
        out_wakers[i] = s->waker_data;
        asx_trace_emit(ASX_TRACE_TIMER_FIRE, asx_timer_trace_entity_id(&fired_handle), s->deadline);
        s->alive = 0;
        if (wheel->active_count > 0) wheel->active_count--;
    }

    return count;
}

/* -------------------------------------------------------------------
 * Timer update (cancel + re-register)
 * ------------------------------------------------------------------- */

asx_status asx_timer_update(asx_timer_wheel *wheel, const asx_timer_handle *old_handle,
                            asx_time new_deadline, void *waker_data, asx_timer_handle *out_handle) {
    if (wheel == NULL || out_handle == NULL) return ASX_E_INVALID_ARGUMENT;

    /* Cancel old timer (no-op if stale) */
    if (old_handle != NULL) { (void)asx_timer_cancel(wheel, old_handle); }

    /* Register new timer */
    return asx_timer_register(wheel, new_deadline, waker_data, out_handle);
}

/* -------------------------------------------------------------------
 * Queries
 * ------------------------------------------------------------------- */

uint32_t asx_timer_active_count(const asx_timer_wheel *wheel) {
    if (wheel == NULL) return 0;
    return wheel->active_count;
}

void asx_timer_set_max_duration(asx_timer_wheel *wheel, uint64_t max_duration_ns) {
    if (wheel == NULL) return;
    wheel->max_duration_ns = max_duration_ns;
}

void asx_timer_advance(asx_timer_wheel *wheel, asx_time now) {
    if (wheel == NULL) return;
    if (now > wheel->current_time) { wheel->current_time = now; }
}
