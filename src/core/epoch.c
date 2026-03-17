/*
 * epoch.c — epoch-based execution phases and barrier triggers
 *
 * Walking skeleton: single-threaded, synchronous advancement.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/core/epoch.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Internal slot                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_epoch_observer_fn fn;
    void *user_data;
} asx_epoch_observer;

typedef struct {
    asx_epoch_policy policy;
    asx_epoch_state state;
    uint64_t phase;
    uint32_t arrival_count;
    uint32_t expected_arrivals; /* for BARRIER mode */
    asx_epoch_observer observers[ASX_MAX_EPOCH_OBSERVERS];
    uint32_t observer_count;
    uint16_t generation;
} asx_epoch_slot;

static asx_epoch_slot g_epochs[ASX_MAX_EPOCHS];
static uint32_t g_epoch_count = 0;

static uint16_t next_gen(uint16_t g) {
    g++;
    if (g == 0) g = 1;
    return g;
}

static asx_epoch_slot *slot_get(asx_epoch_handle h) {
    if (h.slot >= g_epoch_count) return NULL;
    if (g_epochs[h.slot].generation != h.generation) return NULL;
    return &g_epochs[h.slot];
}

static void notify_observers(asx_epoch_slot *s, uint64_t old_phase, uint64_t new_phase) {
    uint32_t i;
    uint64_t epoch_id = (uint64_t)s->generation;
    for (i = 0; i < s->observer_count; i++) {
        if (s->observers[i].fn != NULL) {
            s->observers[i].fn(epoch_id, old_phase, new_phase, s->observers[i].user_data);
        }
    }
}

static void do_advance(asx_epoch_slot *s) {
    uint64_t old = s->phase;
    s->phase++;
    s->arrival_count = 0;
    s->state = ASX_EPOCH_ACTIVE;
    notify_observers(s, old, s->phase);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

asx_status asx_epoch_create(const asx_epoch_policy *policy, asx_epoch_handle *out) {
    asx_epoch_slot *s;
    uint32_t idx;

    if (policy == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;

    /* Find free slot */
    idx = ASX_MAX_EPOCHS;
    {
        uint32_t i;
        for (i = 0; i < g_epoch_count; i++) {
            if (g_epochs[i].state == ASX_EPOCH_CLOSED) {
                idx = i;
                break;
            }
        }
    }
    if (idx == ASX_MAX_EPOCHS) {
        if (g_epoch_count >= ASX_MAX_EPOCHS) return ASX_E_RESOURCE_EXHAUSTED;
        idx = g_epoch_count++;
    }

    s = &g_epochs[idx];
    s->generation = next_gen(s->generation);
    s->policy = *policy;
    s->state = ASX_EPOCH_ACTIVE;
    s->phase = 0;
    s->arrival_count = 0;
    s->expected_arrivals = 0;
    s->observer_count = 0;
    memset(s->observers, 0, sizeof(s->observers));

    out->slot = idx;
    out->generation = s->generation;
    return ASX_OK;
}

asx_status asx_epoch_close(asx_epoch_handle handle) {
    asx_epoch_slot *s = slot_get(handle);
    if (s == NULL) return ASX_E_NOT_FOUND;
    s->state = ASX_EPOCH_CLOSED;
    return ASX_OK;
}

asx_epoch_state asx_epoch_get_state(asx_epoch_handle handle) {
    asx_epoch_slot *s = slot_get(handle);
    if (s == NULL) return ASX_EPOCH_CLOSED;
    return s->state;
}

/* ------------------------------------------------------------------ */
/* Phase management                                                    */
/* ------------------------------------------------------------------ */

uint64_t asx_epoch_current_phase(asx_epoch_handle handle) {
    asx_epoch_slot *s = slot_get(handle);
    if (s == NULL) return 0;
    return s->phase;
}

asx_status asx_epoch_advance(asx_epoch_handle handle) {
    asx_epoch_slot *s = slot_get(handle);
    if (s == NULL) return ASX_E_NOT_FOUND;
    if (s->state == ASX_EPOCH_CLOSED) return ASX_E_INVALID_STATE;
    if (s->policy.mode != ASX_EPOCH_ADVANCE_MANUAL) return ASX_E_INVALID_STATE;
    if (s->policy.max_phases > 0 && s->phase + 1 >= s->policy.max_phases)
        return ASX_E_RESOURCE_EXHAUSTED;

    do_advance(s);
    return ASX_OK;
}

asx_status asx_epoch_arrive(asx_epoch_handle handle, uint64_t entity_id) {
    asx_epoch_slot *s = slot_get(handle);
    (void)entity_id;

    if (s == NULL) return ASX_E_NOT_FOUND;
    if (s->state == ASX_EPOCH_CLOSED) return ASX_E_INVALID_STATE;
    if (s->policy.mode == ASX_EPOCH_ADVANCE_MANUAL) return ASX_E_INVALID_STATE;

    s->arrival_count++;

    /* Check if we should auto-advance */
    if (s->policy.mode == ASX_EPOCH_ADVANCE_BARRIER) {
        if (s->expected_arrivals > 0 && s->arrival_count >= s->expected_arrivals) {
            if (s->policy.max_phases == 0 || s->phase + 1 < s->policy.max_phases) {
                do_advance(s);
            }
        }
    } else if (s->policy.mode == ASX_EPOCH_ADVANCE_THRESHOLD) {
        if (s->arrival_count >= s->policy.threshold) {
            if (s->policy.max_phases == 0 || s->phase + 1 < s->policy.max_phases) {
                do_advance(s);
            }
        }
    }

    return ASX_OK;
}

uint32_t asx_epoch_arrival_count(asx_epoch_handle handle) {
    asx_epoch_slot *s = slot_get(handle);
    if (s == NULL) return 0;
    return s->arrival_count;
}

/* ------------------------------------------------------------------ */
/* Observers                                                           */
/* ------------------------------------------------------------------ */

asx_status asx_epoch_observe(asx_epoch_handle handle, asx_epoch_observer_fn fn,
                              void *user_data) {
    asx_epoch_slot *s = slot_get(handle);
    if (s == NULL) return ASX_E_NOT_FOUND;
    if (fn == NULL) return ASX_E_INVALID_ARGUMENT;
    if (s->observer_count >= ASX_MAX_EPOCH_OBSERVERS) return ASX_E_RESOURCE_EXHAUSTED;

    /* In BARRIER mode, track expected arrivals from observer count */
    s->observers[s->observer_count].fn = fn;
    s->observers[s->observer_count].user_data = user_data;
    s->observer_count++;
    s->expected_arrivals = s->observer_count;

    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Reset                                                               */
/* ------------------------------------------------------------------ */

void asx_epoch_reset(void) {
    uint32_t i;
    for (i = 0; i < ASX_MAX_EPOCHS; i++) {
        g_epochs[i].state = ASX_EPOCH_CLOSED;
        g_epochs[i].generation = next_gen(g_epochs[i].generation);
    }
    g_epoch_count = 0;
}
