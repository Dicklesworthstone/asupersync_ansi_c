/*
 * supervisor.c — supervision trees and restart strategies
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/actor/supervisor.h>
#include <asx/runtime/runtime.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Supervisor phases                                                    */
/* ------------------------------------------------------------------ */

typedef enum {
    SUP_PHASE_INIT = 0,     /* starting children */
    SUP_PHASE_RUNNING = 1,  /* monitoring children */
    SUP_PHASE_STOPPING = 2, /* waiting for children to die (restart) */
    SUP_PHASE_RESTART = 3,  /* restarting children */
    SUP_PHASE_SHUTDOWN = 4, /* stopping all children (final) */
    SUP_PHASE_DONE = 5
} sup_phase;

/* ------------------------------------------------------------------ */
/* Internal state                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    /* Identity */
    uint32_t generation;
    int alive;

    /* Config */
    asx_supervisor_config config;
    asx_region_id region;

    /* Children */
    asx_child_spec specs[ASX_SUPERVISOR_MAX_CHILDREN];
    asx_actor_handle children[ASX_SUPERVISOR_MAX_CHILDREN];
    asx_task_id child_tasks[ASX_SUPERVISOR_MAX_CHILDREN];
    uint32_t child_count;

    /* Restart tracking */
    uint32_t restart_count;

    /* State machine */
    sup_phase phase;
    uint32_t restart_mask; /* bitmask: which children to restart */

    /* Task */
    asx_task_id task_id;
    asx_status exit_reason;
} asx_supervisor_slot;

/* ------------------------------------------------------------------ */
/* Global arena                                                        */
/* ------------------------------------------------------------------ */

static asx_supervisor_slot g_supervisors[ASX_MAX_SUPERVISORS];
static uint32_t g_supervisor_count;

/* ------------------------------------------------------------------ */
/* Generation helper                                                   */
/* ------------------------------------------------------------------ */

static uint32_t next_gen(uint32_t g) {
    g++;
    if (g == 0) g = 1;
    return g;
}

/* ------------------------------------------------------------------ */
/* Slot lookup                                                         */
/* ------------------------------------------------------------------ */

static asx_supervisor_slot *sup_lookup(asx_supervisor_handle h) {
    asx_supervisor_slot *s;
    if (h.slot >= ASX_MAX_SUPERVISORS) return NULL;
    s = &g_supervisors[h.slot];
    if (!s->alive) return NULL;
    if (s->generation != h.generation) return NULL;
    return s;
}

/* ------------------------------------------------------------------ */
/* Child management helpers                                            */
/* ------------------------------------------------------------------ */

static asx_status start_child(asx_supervisor_slot *s, uint32_t idx) {
    asx_status st;
    asx_task_id tid;

    st = s->specs[idx].start_fn(s->specs[idx].user_data, s->region, &s->children[idx]);
    if (st != ASX_OK) return st;

    /* Record the actor's task ID for outcome checking */
    st = asx_actor_task_id(s->children[idx], &tid);
    if (st == ASX_OK) { s->child_tasks[idx] = tid; }
    return ASX_OK;
}

static void stop_child(asx_supervisor_slot *s, uint32_t idx) {
    if (asx_actor_is_alive(s->children[idx])) {
        asx_status st_ = asx_actor_stop(s->children[idx]);
        (void)st_;
    }
}

static int child_should_restart(const asx_supervisor_slot *s, uint32_t idx) {
    switch (s->specs[idx].restart) {
    case ASX_CHILD_PERMANENT: return 1;

    case ASX_CHILD_TRANSIENT:
        /* Only restart on abnormal exit (non-OK exit reason) */
        if (asx_actor_exit_reason(s->children[idx]) == ASX_OK) {
            return 0; /* normal exit, don't restart */
        }
        return 1; /* error exit, restart */

    case ASX_CHILD_TEMPORARY: return 0;
    }
    return 0;
}

static int all_mask_dead(const asx_supervisor_slot *s, uint32_t mask) {
    uint32_t i;
    for (i = 0; i < s->child_count; i++) {
        if ((mask & (1u << i)) && asx_actor_is_alive(s->children[i])) { return 0; }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Strategy application                                                */
/* ------------------------------------------------------------------ */

static void apply_strategy(asx_supervisor_slot *s, uint32_t failed_idx) {
    uint32_t i;

    s->restart_count++;

    switch (s->config.strategy) {
    case ASX_SUPERVISOR_ONE_FOR_ONE:
        s->restart_mask = (1u << failed_idx);
        /* Dead child, go directly to restart */
        s->phase = SUP_PHASE_RESTART;
        break;

    case ASX_SUPERVISOR_ONE_FOR_ALL:
        s->restart_mask = (s->child_count >= 32u) ? 0xFFFFFFFFu : ((1u << s->child_count) - 1u);
        /* Stop all live children */
        for (i = 0; i < s->child_count; i++) {
            if (i != failed_idx) { stop_child(s, i); }
        }
        /* Check if we can go directly to restart */
        if (all_mask_dead(s, s->restart_mask)) {
            s->phase = SUP_PHASE_RESTART;
        } else {
            s->phase = SUP_PHASE_STOPPING;
        }
        break;

    case ASX_SUPERVISOR_REST_FOR_ONE:
        s->restart_mask = 0;
        for (i = failed_idx; i < s->child_count; i++) {
            s->restart_mask |= (1u << i);
            if (i != failed_idx) { stop_child(s, i); }
        }
        if (all_mask_dead(s, s->restart_mask)) {
            s->phase = SUP_PHASE_RESTART;
        } else {
            s->phase = SUP_PHASE_STOPPING;
        }
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Supervisor poll function                                            */
/* ------------------------------------------------------------------ */

static asx_status supervisor_poll(void *user_data, asx_task_id self) {
    uint32_t slot_idx = (uint32_t)(uintptr_t)user_data;
    asx_supervisor_slot *s;
    uint32_t i;
    asx_status st;

    (void)self;

    if (slot_idx >= ASX_MAX_SUPERVISORS) return ASX_E_INVALID_STATE;
    s = &g_supervisors[slot_idx];
    if (!s->alive) return ASX_E_INVALID_STATE;

    switch (s->phase) {
    case SUP_PHASE_INIT:
        /* Start all children in order */
        for (i = 0; i < s->child_count; i++) {
            st = start_child(s, i);
            if (st != ASX_OK) {
                /* Failed to start child — stop previously started */
                {
                    uint32_t j;
                    for (j = 0; j < i; j++) { stop_child(s, j); }
                }
                s->exit_reason = st;
                s->phase = SUP_PHASE_SHUTDOWN;
                return ASX_E_PENDING;
            }
        }
        s->phase = SUP_PHASE_RUNNING;
        return ASX_E_PENDING;

    case SUP_PHASE_RUNNING:
        /* Monitor children */
        for (i = 0; i < s->child_count; i++) {
            if (!asx_actor_is_alive(s->children[i])) {
                if (!child_should_restart(s, i)) {
                    continue; /* temporary or normal transient exit */
                }

                /* Check restart intensity */
                if (s->restart_count >= s->config.max_restarts) {
                    s->exit_reason = ASX_E_RESOURCE_EXHAUSTED;
                    /* Stop all remaining children */
                    {
                        uint32_t j;
                        for (j = 0; j < s->child_count; j++) { stop_child(s, j); }
                    }
                    s->phase = SUP_PHASE_SHUTDOWN;
                    return ASX_E_PENDING;
                }

                apply_strategy(s, i);
                return ASX_E_PENDING;
            }
        }
        return ASX_E_PENDING;

    case SUP_PHASE_STOPPING:
        /* Wait for children in restart_mask to die */
        if (all_mask_dead(s, s->restart_mask)) { s->phase = SUP_PHASE_RESTART; }
        return ASX_E_PENDING;

    case SUP_PHASE_RESTART:
        /* Restart children in mask, in order */
        for (i = 0; i < s->child_count; i++) {
            if (!(s->restart_mask & (1u << i))) continue;
            st = start_child(s, i);
            if (st != ASX_OK) {
                s->exit_reason = st;
                {
                    uint32_t j;
                    for (j = 0; j < s->child_count; j++) { stop_child(s, j); }
                }
                s->phase = SUP_PHASE_SHUTDOWN;
                return ASX_E_PENDING;
            }
        }
        s->restart_mask = 0;
        s->phase = SUP_PHASE_RUNNING;
        return ASX_E_PENDING;

    case SUP_PHASE_SHUTDOWN:
        /* Wait for all children to die */
        {
            int any_alive = 0;
            for (i = 0; i < s->child_count; i++) {
                if (asx_actor_is_alive(s->children[i])) { any_alive = 1; }
            }
            if (!any_alive) {
                s->alive = 0;
                s->phase = SUP_PHASE_DONE;
                return s->exit_reason;
            }
        }
        return ASX_E_PENDING;

    case SUP_PHASE_DONE: return s->exit_reason;
    }

    return ASX_E_INVALID_STATE;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

asx_status asx_supervisor_start(asx_supervisor_handle *out, asx_region_id region,
                                const asx_supervisor_config *config, const asx_child_spec *children,
                                uint32_t child_count) {
    uint32_t idx;
    asx_supervisor_slot *s;
    asx_task_id tid;
    asx_status st;

    if (out == NULL || config == NULL) return ASX_E_INVALID_ARGUMENT;
    if (children == NULL && child_count > 0) return ASX_E_INVALID_ARGUMENT;
    if (child_count > ASX_SUPERVISOR_MAX_CHILDREN) return ASX_E_INVALID_ARGUMENT;

    /* Validate child specs */
    {
        uint32_t i;
        for (i = 0; i < child_count; i++) {
            if (children[i].start_fn == NULL) return ASX_E_INVALID_ARGUMENT;
        }
    }

    /* Find free slot */
    for (idx = 0; idx < ASX_MAX_SUPERVISORS; idx++) {
        if (!g_supervisors[idx].alive) break;
    }
    if (idx >= ASX_MAX_SUPERVISORS) return ASX_E_RESOURCE_EXHAUSTED;

    s = &g_supervisors[idx];

    /* Spawn supervisor task */
    st = asx_task_spawn(region, supervisor_poll, (void *)(uintptr_t)idx, &tid);
    if (st != ASX_OK) return st;

    /* Initialize slot */
    s->generation = next_gen(s->generation);
    s->alive = 1;
    s->config = *config;
    s->region = region;
    memcpy(s->specs, children, child_count * sizeof(asx_child_spec));
    s->child_count = child_count;
    memset(s->children, 0, sizeof(s->children));
    memset(s->child_tasks, 0, sizeof(s->child_tasks));
    s->restart_count = 0;
    s->phase = SUP_PHASE_INIT;
    s->restart_mask = 0;
    s->task_id = tid;
    s->exit_reason = ASX_OK;

    if (idx >= g_supervisor_count) g_supervisor_count = idx + 1;

    out->slot = idx;
    out->generation = s->generation;
    return ASX_OK;
}

asx_status asx_supervisor_stop(asx_supervisor_handle sup) {
    asx_supervisor_slot *s;
    uint32_t i;

    s = sup_lookup(sup);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;

    /* Stop all children in reverse order */
    for (i = s->child_count; i > 0; i--) { stop_child(s, i - 1); }

    s->exit_reason = ASX_OK;
    s->phase = SUP_PHASE_SHUTDOWN;
    return ASX_OK;
}

int asx_supervisor_is_alive(asx_supervisor_handle sup) { return sup_lookup(sup) != NULL; }

uint32_t asx_supervisor_child_count(asx_supervisor_handle sup) {
    asx_supervisor_slot *s = sup_lookup(sup);
    if (s == NULL) return 0;
    return s->child_count;
}

int asx_supervisor_child_alive(asx_supervisor_handle sup, uint32_t index) {
    asx_supervisor_slot *s = sup_lookup(sup);
    if (s == NULL) return 0;
    if (index >= s->child_count) return 0;
    return asx_actor_is_alive(s->children[index]);
}

uint32_t asx_supervisor_restart_count(asx_supervisor_handle sup) {
    asx_supervisor_slot *s = sup_lookup(sup);
    if (s == NULL) return 0;
    return s->restart_count;
}

/* ------------------------------------------------------------------ */
/* Reset                                                               */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Extended child spec                                                 */
/* ------------------------------------------------------------------ */

void asx_child_spec_ext_init(asx_child_spec_ext *spec, const char *name,
                               asx_child_start_fn start_fn, void *user_data) {
    if (spec == NULL) return;
    memset(spec, 0, sizeof(*spec));
    spec->start_fn = start_fn;
    spec->user_data = user_data;
    spec->restart = ASX_CHILD_PERMANENT;
    spec->required = 1u;
    spec->start_immediately = 1u;
    if (name != NULL) {
        size_t len = strlen(name);
        if (len >= ASX_CHILD_NAME_MAX) len = ASX_CHILD_NAME_MAX - 1u;
        memcpy(spec->name, name, len);
        spec->name[len] = '\0';
    }
}

asx_status asx_child_spec_ext_depends_on(asx_child_spec_ext *spec, uint32_t dep_index) {
    if (spec == NULL) return ASX_E_INVALID_ARGUMENT;
    if (spec->dep_count >= ASX_SUPERVISOR_MAX_DEPS) return ASX_E_RESOURCE_EXHAUSTED;
    spec->depends_on[spec->dep_count++] = dep_index;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Restart intensity                                                   */
/* ------------------------------------------------------------------ */

void asx_restart_intensity_init(asx_restart_intensity *ri, uint32_t max_restarts,
                                 uint64_t window_ns) {
    if (ri == NULL) return;
    memset(ri, 0, sizeof(*ri));
    ri->max_restarts = max_restarts;
    ri->window_ns = window_ns;
}

int asx_restart_intensity_record(asx_restart_intensity *ri, uint64_t now_ns) {
    uint32_t tail;

    if (ri == NULL) return 0;
    tail = (ri->head + ri->count) % 64u;
    ri->restart_timestamps[tail] = now_ns;
    if (ri->count < 64u) {
        ri->count++;
    } else {
        ri->head = (ri->head + 1u) % 64u;
    }
    return asx_restart_intensity_count(ri, now_ns) > ri->max_restarts;
}

uint32_t asx_restart_intensity_count(const asx_restart_intensity *ri, uint64_t now_ns) {
    uint32_t i, count;
    uint64_t cutoff;

    if (ri == NULL) return 0u;
    if (ri->window_ns == 0u) return ri->count; /* no window = cumulative */

    cutoff = now_ns > ri->window_ns ? now_ns - ri->window_ns : 0u;
    count = 0u;
    for (i = 0u; i < ri->count; i++) {
        uint32_t idx = (ri->head + i) % 64u;
        if (ri->restart_timestamps[idx] >= cutoff) count++;
    }
    return count;
}

int asx_restart_intensity_is_storm(const asx_restart_intensity *ri, uint64_t now_ns) {
    if (ri == NULL) return 0;
    return asx_restart_intensity_count(ri, now_ns) > ri->max_restarts;
}

/* ------------------------------------------------------------------ */
/* Extended supervisor start                                           */
/* ------------------------------------------------------------------ */

asx_status asx_supervisor_start_ext(asx_supervisor_handle *out, asx_region_id region,
                                     const asx_supervisor_config *config,
                                     const asx_child_spec_ext *children, uint32_t child_count) {
    /* Convert extended specs to basic specs and delegate to start */
    asx_child_spec basic[ASX_SUPERVISOR_MAX_CHILDREN];
    uint32_t i;

    if (children == NULL || child_count == 0u) return ASX_E_INVALID_ARGUMENT;
    if (child_count > ASX_SUPERVISOR_MAX_CHILDREN) return ASX_E_RESOURCE_EXHAUSTED;

    for (i = 0u; i < child_count; i++) {
        basic[i].start_fn = children[i].start_fn;
        basic[i].user_data = children[i].user_data;
        basic[i].restart = children[i].restart;
    }

    return asx_supervisor_start(out, region, config, basic, child_count);
}

const char *asx_supervisor_child_name(asx_supervisor_handle sup, uint32_t index) {
    (void)sup;
    (void)index;
    return ""; /* Names are stored in ext specs, not in basic supervisor slot */
}

asx_status asx_supervisor_exit_reason(asx_supervisor_handle sup) {
    asx_supervisor_slot *s = sup_lookup(sup);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    return s->exit_reason;
}

int asx_supervisor_intensity_exceeded(asx_supervisor_handle sup) {
    asx_supervisor_slot *s = sup_lookup(sup);
    if (s == NULL) return 0;
    return s->restart_count >= s->config.max_restarts;
}

/* ------------------------------------------------------------------ */
/* Reset                                                               */
/* ------------------------------------------------------------------ */

void asx_supervisor_reset(void) {
    memset(g_supervisors, 0, sizeof(g_supervisors));
    g_supervisor_count = 0;
}
