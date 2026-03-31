/*
 * pool.c — generic resource pool with health checks and stats
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/sync/pool.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Arena                                                               */
/* ------------------------------------------------------------------ */

typedef enum { RESOURCE_IDLE = 0, RESOURCE_ACTIVE = 1, RESOURCE_EMPTY = 2 } resource_state;

typedef struct {
    void *resource;
    resource_state state;
} resource_slot;

typedef struct {
    uint16_t generation;
    int alive;
    int closed;
    asx_pool_config config;
    resource_slot resources[ASX_POOL_MAX_RESOURCES];
    uint32_t resource_count; /* total slots used (active + idle) */
    uint64_t total_acquisitions;
    uint64_t total_creates;
    uint64_t health_failures;
} pool_slot;

static pool_slot g_slots[ASX_POOL_MAX];
static uint32_t g_slot_count;

static uint16_t next_gen(uint16_t g) {
    g++;
    if (g == 0) g = 1;
    return g;
}

static pool_slot *slot_lookup(uint32_t idx, uint16_t gen) {
    pool_slot *s;
    if (idx >= ASX_POOL_MAX) return NULL;
    s = &g_slots[idx];
    if (!s->alive || s->generation != gen) return NULL;
    return s;
}

static void destroy_resource(pool_slot *ps, resource_slot *rs) {
    if (rs->resource != NULL && ps->config.destroy_fn != NULL) {
        ps->config.destroy_fn(rs->resource, ps->config.factory_data);
    }
    rs->resource = NULL;
    rs->state = RESOURCE_EMPTY;
}

static int health_check(pool_slot *ps, resource_slot *rs) {
    if (ps->config.health_fn == NULL) return 1; /* no check = always healthy */
    return ps->config.health_fn(rs->resource, ps->config.factory_data);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

asx_status asx_pool_create(const asx_pool_config *config, asx_pool_handle *out) {
    uint32_t i;
    if (config == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    if (config->create_fn == NULL) return ASX_E_INVALID_ARGUMENT;
    if (config->max_size == 0 || config->max_size > ASX_POOL_MAX_RESOURCES)
        return ASX_E_INVALID_ARGUMENT;

    for (i = 0; i < ASX_POOL_MAX; i++) {
        if (!g_slots[i].alive) {
            pool_slot *ps = &g_slots[i];
            uint32_t j;
            ps->alive = 1;
            ps->closed = 0;
            ps->generation = next_gen(ps->generation);
            ps->config = *config;
            ps->resource_count = 0;
            ps->total_acquisitions = 0;
            ps->total_creates = 0;
            ps->health_failures = 0;
            for (j = 0; j < ASX_POOL_MAX_RESOURCES; j++) {
                ps->resources[j].resource = NULL;
                ps->resources[j].state = RESOURCE_EMPTY;
            }
            out->slot = i;
            out->generation = ps->generation;
            if (i >= g_slot_count) g_slot_count = i + 1;
            return ASX_OK;
        }
    }
    return ASX_E_RESOURCE_EXHAUSTED;
}

asx_status asx_pool_close(asx_pool_handle handle) {
    pool_slot *ps = slot_lookup(handle.slot, handle.generation);
    uint32_t i;
    if (ps == NULL) return ASX_E_STALE_HANDLE;

    ps->closed = 1;

    /* Destroy all idle resources */
    for (i = 0; i < ASX_POOL_MAX_RESOURCES; i++) {
        if (ps->resources[i].state == RESOURCE_IDLE) {
            destroy_resource(ps, &ps->resources[i]);
            ps->resource_count--;
        }
    }

    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Acquire                                                             */
/* ------------------------------------------------------------------ */

asx_status asx_pool_try_acquire(asx_pool_handle handle, asx_pooled_resource *out) {
    pool_slot *ps = slot_lookup(handle.slot, handle.generation);
    uint32_t i;

    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    if (ps == NULL) return ASX_E_STALE_HANDLE;
    if (ps->closed) return ASX_E_DISCONNECTED;

    /* Try to find a healthy idle resource */
    for (i = 0; i < ASX_POOL_MAX_RESOURCES; i++) {
        if (ps->resources[i].state == RESOURCE_IDLE) {
            if (!health_check(ps, &ps->resources[i])) {
                /* Health check failed — destroy and continue looking */
                destroy_resource(ps, &ps->resources[i]);
                ps->resource_count--;
                ps->health_failures++;
                continue;
            }
            /* Found a healthy idle resource */
            ps->resources[i].state = RESOURCE_ACTIVE;
            ps->total_acquisitions++;
            out->pool_slot = handle.slot;
            out->resource_slot = i;
            out->generation = handle.generation;
            out->resource = ps->resources[i].resource;
            return ASX_OK;
        }
    }

    /* No idle resources — try to create a new one if under max_size */
    if (ps->resource_count < ps->config.max_size) {
        for (i = 0; i < ASX_POOL_MAX_RESOURCES; i++) {
            if (ps->resources[i].state == RESOURCE_EMPTY) {
                void *new_resource = NULL;
                asx_status st = ps->config.create_fn(ps->config.factory_data, &new_resource);
                if (st != ASX_OK) return st;

                ps->resources[i].resource = new_resource;
                ps->resources[i].state = RESOURCE_ACTIVE;
                ps->resource_count++;
                ps->total_creates++;
                ps->total_acquisitions++;
                out->pool_slot = handle.slot;
                out->resource_slot = i;
                out->generation = handle.generation;
                out->resource = new_resource;
                return ASX_OK;
            }
        }
    }

    return ASX_E_WOULD_BLOCK;
}

/* ------------------------------------------------------------------ */
/* Return                                                              */
/* ------------------------------------------------------------------ */

asx_status asx_pool_return(asx_pooled_resource *pr) {
    pool_slot *ps;
    resource_slot *rs;

    if (pr == NULL) return ASX_E_INVALID_ARGUMENT;
    ps = slot_lookup(pr->pool_slot, pr->generation);
    if (ps == NULL) {
        /* Pool is gone — resource is orphaned, nothing to return to */
        return ASX_E_STALE_HANDLE;
    }

    if (pr->resource_slot >= ASX_POOL_MAX_RESOURCES) return ASX_E_INVALID_ARGUMENT;
    rs = &ps->resources[pr->resource_slot];

    if (rs->state != RESOURCE_ACTIVE) return ASX_E_INVALID_STATE;

    if (ps->closed) {
        /* Pool is closing — destroy the resource */
        destroy_resource(ps, rs);
        ps->resource_count--;
    } else {
        /* Return to idle pool */
        rs->state = RESOURCE_IDLE;
    }

    pr->resource = NULL; /* prevent use-after-return */
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Queries                                                             */
/* ------------------------------------------------------------------ */

asx_status asx_pool_get_stats(asx_pool_handle handle, asx_pool_stats *out) {
    pool_slot *ps = slot_lookup(handle.slot, handle.generation);
    uint32_t i;
    if (ps == NULL) return ASX_E_STALE_HANDLE;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));
    for (i = 0; i < ASX_POOL_MAX_RESOURCES; i++) {
        if (ps->resources[i].state == RESOURCE_ACTIVE)
            out->active++;
        else if (ps->resources[i].state == RESOURCE_IDLE)
            out->idle++;
    }
    out->total = out->active + out->idle;
    out->max_size = ps->config.max_size;
    out->total_acquisitions = ps->total_acquisitions;
    out->total_creates = ps->total_creates;
    out->health_failures = ps->health_failures;
    return ASX_OK;
}

int asx_pool_is_closed(asx_pool_handle handle) {
    pool_slot *ps = slot_lookup(handle.slot, handle.generation);
    if (ps == NULL) return 1;
    return ps->closed;
}

/* ------------------------------------------------------------------ */
/* Arena management                                                    */
/* ------------------------------------------------------------------ */

void asx_pool_reset(void) {
    uint32_t i, j;
    for (i = 0; i < g_slot_count; i++) {
        if (g_slots[i].alive) {
            for (j = 0; j < ASX_POOL_MAX_RESOURCES; j++) {
                if (g_slots[i].resources[j].state != RESOURCE_EMPTY) {
                    destroy_resource(&g_slots[i], &g_slots[i].resources[j]);
                }
            }
        }
        g_slots[i].generation = next_gen(g_slots[i].generation);
        g_slots[i].alive = 0;
        g_slots[i].closed = 0;
        g_slots[i].resource_count = 0;
    }
    g_slot_count = 0;
}
