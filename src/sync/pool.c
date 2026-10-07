/*
 * pool.c — generic resource pool with health checks and stats
 *
 * Wake-driven waiting: try_acquire reporting ASX_E_WOULD_BLOCK inside a
 * scheduler poll parks the calling task in the pool's FIFO wait queue.
 * After every state change the queue is settled: each idle resource (or
 * free creation slot) is owed to one parked task, oldest first, so a
 * return wakes one waiter, and nobody jumps the line (with k resources
 * available only the first k parked waiters may take one). Close wakes
 * every waiter so it observes ASX_E_DISCONNECTED. asx_pool_wait_cancel()
 * withdraws a task that stops waiting and passes any wake it held on.
 *
 * SPDX-License-Identifier: MIT
 */

#include "wait_queue.h"
#include <asx/sync/pool.h>
#include <string.h>

#if (ASX_POOL_MAX_WAITERS) < 1 || (ASX_POOL_MAX_WAITERS) > (ASX_WAIT_QUEUE_MAX_CAPACITY)
#error "ASX_POOL_MAX_WAITERS must be in [1, ASX_WAIT_QUEUE_MAX_CAPACITY]"
#endif

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
    asx_task_id wait_slots[ASX_POOL_MAX_WAITERS];
    asx_wait_queue waiters; /* tasks parked in try_acquire */
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

/* Resources a try_acquire could get right now: idle ones plus room to
 * create new ones (an idle resource failing its health check is replaced
 * by a creation, so it still counts). */
static uint32_t pool_available(const pool_slot *ps) {
    uint32_t i;
    uint32_t n = 0;
    for (i = 0; i < ASX_POOL_MAX_RESOURCES; i++) {
        if (ps->resources[i].state == RESOURCE_IDLE) n++;
    }
    if (ps->resource_count < ps->config.max_size) n += ps->config.max_size - ps->resource_count;
    return n;
}

/* Owe each available resource to one parked waiter, oldest first. */
static void pool_settle(pool_slot *ps) {
    if (ps->waiters.len > 0u) (void)asx_wait_queue_settle(&ps->waiters, pool_available(ps));
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
            asx_wait_queue_init(&ps->waiters, ps->wait_slots, ASX_POOL_MAX_WAITERS);
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

    /* Every parked waiter re-polls and observes the close. */
    if (ps->waiters.len > 0u) (void)asx_wait_queue_wake_all(&ps->waiters);
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
    if (ps->closed) {
        asx_wait_queue_leave_current(&ps->waiters);
        return ASX_E_DISCONNECTED;
    }

    /* FIFO window: with k resources available, the first k parked waiters
     * own them; later arrivals (and newcomers) wait their turn. */
    if (ps->waiters.len > 0u && asx_wait_queue_live_ahead(&ps->waiters) >= pool_available(ps)) {
        pool_settle(ps);
        (void)asx_wait_queue_park_current(&ps->waiters);
        return ASX_E_WOULD_BLOCK;
    }

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
            asx_wait_queue_leave_current(&ps->waiters);
            pool_settle(ps);
            return ASX_OK;
        }
    }

    /* No idle resources — try to create a new one if under max_size */
    if (ps->resource_count < ps->config.max_size) {
        for (i = 0; i < ASX_POOL_MAX_RESOURCES; i++) {
            if (ps->resources[i].state == RESOURCE_EMPTY) {
                void *new_resource = NULL;
                asx_status st = ps->config.create_fn(ps->config.factory_data, &new_resource);
                if (st != ASX_OK) {
                    asx_wait_queue_leave_current(&ps->waiters);
                    return st;
                }

                ps->resources[i].resource = new_resource;
                ps->resources[i].state = RESOURCE_ACTIVE;
                ps->resource_count++;
                ps->total_creates++;
                ps->total_acquisitions++;
                out->pool_slot = handle.slot;
                out->resource_slot = i;
                out->generation = handle.generation;
                out->resource = new_resource;
                asx_wait_queue_leave_current(&ps->waiters);
                pool_settle(ps);
                return ASX_OK;
            }
        }
    }

    /* Exhausted: inside a scheduler poll, park until a resource returns. */
    pool_settle(ps);
    (void)asx_wait_queue_park_current(&ps->waiters);
    return ASX_E_WOULD_BLOCK;
}

asx_status asx_pool_wait_cancel(asx_pool_handle handle, asx_task_id task) {
    pool_slot *ps = slot_lookup(handle.slot, handle.generation);
    if (ps == NULL) return ASX_E_STALE_HANDLE;

    /* A wake the withdrawn task held goes to the next waiter in line. */
    if (asx_wait_queue_remove(&ps->waiters, task)) pool_settle(ps);
    return ASX_OK;
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
        /* Return to idle pool and hand it to the oldest parked waiter */
        rs->state = RESOURCE_IDLE;
        pool_settle(ps);
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
    out->waiters = asx_wait_queue_len(&ps->waiters);
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
        asx_wait_queue_init(&g_slots[i].waiters, g_slots[i].wait_slots, ASX_POOL_MAX_WAITERS);
    }
    g_slot_count = 0;
}
