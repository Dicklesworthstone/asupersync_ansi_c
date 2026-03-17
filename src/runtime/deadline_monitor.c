/*
 * deadline_monitor.c — runtime deadline monitor
 *
 * Walking skeleton: single-threaded, check-on-poll model.
 * Deadlines are checked against a caller-provided timestamp.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/runtime/deadline_monitor.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Internal slot                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_time target_ns;
    uint64_t entity_id;
    asx_deadline_miss_fn on_miss;
    void *user_data;
    uint16_t generation;
    asx_deadline_state state;
} asx_dm_slot;

/* ------------------------------------------------------------------ */
/* Arena                                                               */
/* ------------------------------------------------------------------ */

static asx_dm_slot g_slots[ASX_MAX_DEADLINE_MONITORS];
static uint32_t g_slot_count = 0;
static int g_initialized = 0;

/* Running statistics */
static asx_deadline_monitor_stats g_stats;

static uint16_t next_gen(uint16_t g) {
    g++;
    if (g == 0) g = 1;
    return g;
}

static int slot_valid(const asx_deadline_monitor_handle *h) {
    if (h->slot >= g_slot_count) return 0;
    if (g_slots[h->slot].generation != h->generation) return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

asx_status asx_deadline_monitor_init(void) {
    asx_deadline_monitor_reset();
    g_initialized = 1;
    return ASX_OK;
}

void asx_deadline_monitor_shutdown(void) {
    /* Cancel all pending deadlines */
    uint32_t i;
    for (i = 0; i < g_slot_count; i++) {
        if (g_slots[i].state == ASX_DEADLINE_PENDING) {
            g_slots[i].state = ASX_DEADLINE_CANCELLED;
            g_stats.cancelled_count++;
            if (g_stats.active_count > 0) g_stats.active_count--;
        }
    }
    g_initialized = 0;
}

int asx_deadline_monitor_is_initialized(void) { return g_initialized; }

void asx_deadline_monitor_reset(void) {
    uint32_t i;
    for (i = 0; i < ASX_MAX_DEADLINE_MONITORS; i++) {
        g_slots[i].generation = next_gen(g_slots[i].generation);
        g_slots[i].state = ASX_DEADLINE_CANCELLED;
        g_slots[i].target_ns = 0;
        g_slots[i].entity_id = 0;
        g_slots[i].on_miss = NULL;
        g_slots[i].user_data = NULL;
    }
    g_slot_count = 0;
    memset(&g_stats, 0, sizeof(g_stats));
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

asx_status asx_deadline_monitor_register(asx_time target_ns, uint64_t entity_id,
                                          asx_deadline_miss_fn on_miss, void *user_data,
                                          asx_deadline_monitor_handle *out_handle) {
    uint32_t idx;
    asx_dm_slot *s;

    if (out_handle == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!g_initialized) return ASX_E_INVALID_STATE;

    /* Find free slot (reuse resolved/cancelled slots) */
    idx = ASX_MAX_DEADLINE_MONITORS;
    {
        uint32_t i;
        for (i = 0; i < g_slot_count; i++) {
            if (g_slots[i].state != ASX_DEADLINE_PENDING) {
                idx = i;
                break;
            }
        }
    }

    if (idx == ASX_MAX_DEADLINE_MONITORS) {
        if (g_slot_count >= ASX_MAX_DEADLINE_MONITORS) return ASX_E_RESOURCE_EXHAUSTED;
        idx = g_slot_count++;
    }

    s = &g_slots[idx];
    s->generation = next_gen(s->generation);
    s->target_ns = target_ns;
    s->entity_id = entity_id;
    s->on_miss = on_miss;
    s->user_data = user_data;
    s->state = ASX_DEADLINE_PENDING;

    out_handle->slot = idx;
    out_handle->generation = s->generation;

    g_stats.total_registered++;
    g_stats.active_count++;

    return ASX_OK;
}

asx_status asx_deadline_monitor_cancel(const asx_deadline_monitor_handle *handle) {
    asx_dm_slot *s;

    if (handle == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!slot_valid(handle)) return ASX_E_NOT_FOUND;

    s = &g_slots[handle->slot];
    if (s->state != ASX_DEADLINE_PENDING) return ASX_OK; /* already resolved */

    s->state = ASX_DEADLINE_CANCELLED;
    g_stats.cancelled_count++;
    if (g_stats.active_count > 0) g_stats.active_count--;

    return ASX_OK;
}

asx_status asx_deadline_monitor_complete(const asx_deadline_monitor_handle *handle,
                                          asx_time completion_ns) {
    asx_dm_slot *s;
    int64_t margin;

    if (handle == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!slot_valid(handle)) return ASX_E_NOT_FOUND;

    s = &g_slots[handle->slot];
    if (s->state != ASX_DEADLINE_PENDING) return ASX_E_INVALID_STATE;

    /* Compute margin: positive = completed before deadline, negative = missed */
    if (s->target_ns >= completion_ns) {
        uint64_t raw = s->target_ns - completion_ns;
        margin = (raw > (uint64_t)INT64_MAX) ? INT64_MAX : (int64_t)raw;
    } else {
        uint64_t raw = completion_ns - s->target_ns;
        margin = (raw > (uint64_t)INT64_MAX) ? INT64_MIN : -(int64_t)raw;
    }

    if (completion_ns <= s->target_ns) {
        s->state = ASX_DEADLINE_MET;
        g_stats.met_count++;
    } else {
        s->state = ASX_DEADLINE_MISSED;
        g_stats.missed_count++;
    }

    if (g_stats.active_count > 0) g_stats.active_count--;

    /* Update margin statistics */
    if (g_stats.met_count + g_stats.missed_count == 1) {
        g_stats.worst_margin_ns = margin;
        g_stats.best_margin_ns = margin;
    } else {
        if (margin < g_stats.worst_margin_ns) g_stats.worst_margin_ns = margin;
        if (margin > g_stats.best_margin_ns) g_stats.best_margin_ns = margin;
    }

    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Check / evaluation                                                  */
/* ------------------------------------------------------------------ */

uint32_t asx_deadline_monitor_check(asx_time now_ns) {
    uint32_t new_misses = 0;
    uint32_t i;

    if (!g_initialized) return 0;

    for (i = 0; i < g_slot_count; i++) {
        asx_dm_slot *s = &g_slots[i];
        if (s->state != ASX_DEADLINE_PENDING) continue;

        if (now_ns >= s->target_ns) {
            uint64_t overdue;
            int64_t margin;

            s->state = ASX_DEADLINE_MISSED;
            g_stats.missed_count++;
            if (g_stats.active_count > 0) g_stats.active_count--;
            new_misses++;

            overdue = now_ns - s->target_ns;
            margin = (overdue > (uint64_t)INT64_MAX) ? INT64_MIN : -(int64_t)overdue;

            if (g_stats.met_count + g_stats.missed_count == 1) {
                g_stats.worst_margin_ns = margin;
                g_stats.best_margin_ns = margin;
            } else {
                if (margin < g_stats.worst_margin_ns) g_stats.worst_margin_ns = margin;
                if (margin > g_stats.best_margin_ns) g_stats.best_margin_ns = margin;
            }

            /* Invoke miss callback */
            if (s->on_miss != NULL) {
                s->on_miss(s->entity_id, overdue, s->user_data);
            }
        }
    }

    return new_misses;
}

/* ------------------------------------------------------------------ */
/* Query                                                               */
/* ------------------------------------------------------------------ */

asx_deadline_state asx_deadline_monitor_get_state(const asx_deadline_monitor_handle *handle) {
    if (handle == NULL) return ASX_DEADLINE_CANCELLED;
    if (!slot_valid(handle)) return ASX_DEADLINE_CANCELLED;
    return g_slots[handle->slot].state;
}

uint32_t asx_deadline_monitor_active_count(void) { return g_stats.active_count; }

void asx_deadline_monitor_get_stats(asx_deadline_monitor_stats *out) {
    if (out == NULL) return;
    *out = g_stats;
}
