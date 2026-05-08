/*
 * blocking.c — blocking pool for offloading blocking work
 *
 * Deterministic builds execute blocking work inline. Live-mode platforms can
 * install a bounded submit hook; this module still owns slots, handles,
 * results, active counts, and completion wakers.
 * See docs/DEFERRED_STUBS_REGISTER.md.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/platform/atomics.h>
#include <asx/runtime/blocking.h>
#include <asx/runtime/browser_boundary.h>
#include <string.h>

#if ASX_HAS_BLOCKING_SURFACE
/* ------------------------------------------------------------------ */
/* Internal slot                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_blocking_fn fn;
    void *user_data;
    asx_waker completion_waker;
    int has_waker;
    uint16_t generation;
    asx_atomic_u32 state;
    uint64_t result;
} asx_blocking_slot;

typedef struct {
    uint32_t slot;
    uint16_t generation;
} asx_blocking_job_ctx;

/* ------------------------------------------------------------------ */
/* Arena                                                               */
/* ------------------------------------------------------------------ */

static asx_blocking_slot g_slots[ASX_MAX_BLOCKING_TASKS];
static asx_blocking_job_ctx g_job_ctx[ASX_MAX_BLOCKING_TASKS];
static uint32_t g_slot_count = 0;
static asx_atomic_u32 g_active_count;
static asx_atomic_u32 g_pool_lock;
static int g_initialized = 0;

static uint16_t next_gen(uint16_t g) {
    g++;
    if (g == 0) g = 1;
    return g;
}

static void pool_lock(void) {
    uint32_t expected;
    for (;;) {
        expected = 0u;
        if (asx_atomic_u32_compare_exchange(&g_pool_lock, &expected, 1u)) return;
    }
}

static void pool_unlock(void) { asx_atomic_u32_store(&g_pool_lock, 0u); }

static asx_blocking_state slot_state_load(const asx_blocking_slot *s) {
    return (asx_blocking_state)asx_atomic_u32_load(&s->state);
}

static void slot_state_store(asx_blocking_slot *s, asx_blocking_state state) {
    asx_atomic_u32_store(&s->state, (uint32_t)state);
}

static void active_inc(void) { (void)asx_atomic_u32_fetch_add(&g_active_count, 1u); }

static void active_dec(void) { (void)asx_atomic_u32_fetch_add(&g_active_count, (uint32_t)~0u); }

static int hooks_use_blocking_submit(const asx_runtime_hooks *hooks) {
#if ASX_DETERMINISTIC
    (void)hooks;
    return 0;
#else
    return hooks != NULL && hooks->blocking.submit_fn != NULL;
#endif
}

static void hooks_blocking_shutdown(void) {
#if !ASX_DETERMINISTIC
    const asx_runtime_hooks *hooks = asx_runtime_get_hooks();
    if (hooks != NULL && hooks->blocking.shutdown_fn != NULL) {
        hooks->blocking.shutdown_fn(hooks->blocking.ctx);
    }
#endif
}

static void blocking_job_run(void *job_ctx);

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

asx_status asx_blocking_pool_init(void) {
    asx_status st = asx_surface_gate(ASX_SURFACE_BLOCKING);
    if (st != ASX_OK) return st;

    /* Re-initialization must start from a fresh pool state so stale handles
     * and completed results cannot leak across runtime lifecycles. */
    asx_blocking_pool_reset();
    g_initialized = 1;
    return ASX_OK;
}

void asx_blocking_pool_shutdown(void) {
    asx_blocking_pool_reset();
    g_initialized = 0;
}

int asx_blocking_pool_is_initialized(void) { return g_initialized; }

void asx_blocking_pool_reset(void) {
    uint32_t i;
    hooks_blocking_shutdown();

    pool_lock();
    for (i = 0; i < ASX_MAX_BLOCKING_TASKS; i++) {
        g_slots[i].generation = next_gen(g_slots[i].generation);
        slot_state_store(&g_slots[i], ASX_BLOCKING_COMPLETED);
        g_slots[i].fn = NULL;
        g_slots[i].user_data = NULL;
        g_slots[i].has_waker = 0;
        g_slots[i].result = 0;
        g_job_ctx[i].slot = i;
        g_job_ctx[i].generation = g_slots[i].generation;
    }
    g_slot_count = 0;
    asx_atomic_u32_store(&g_active_count, 0u);
    pool_unlock();
}

/* ------------------------------------------------------------------ */
/* Spawn blocking                                                      */
/* ------------------------------------------------------------------ */

asx_status asx_spawn_blocking(asx_blocking_fn fn, void *user_data,
                              const asx_waker *completion_waker, asx_blocking_handle *out_handle) {
    uint32_t idx;
    asx_blocking_slot *s;
    asx_status st;
    const asx_runtime_hooks *hooks;

    if (fn == NULL || out_handle == NULL) return ASX_E_INVALID_ARGUMENT;
    st = asx_surface_gate(ASX_SURFACE_BLOCKING);
    if (st != ASX_OK) return st;
    if (!g_initialized) return ASX_E_INVALID_STATE;
    if (completion_waker != NULL && asx_waker_task(completion_waker) == ASX_INVALID_ID) {
        return ASX_E_INVALID_ARGUMENT;
    }

    hooks = asx_runtime_get_hooks();

    pool_lock();

    /* Find free slot */
    idx = ASX_MAX_BLOCKING_TASKS;
    {
        uint32_t i;
        for (i = 0; i < g_slot_count; i++) {
            /* ASX_CHECKPOINT_WAIVER("bounded slot search") */
            if (slot_state_load(&g_slots[i]) == ASX_BLOCKING_COMPLETED) {
                idx = i;
                break;
            }
        }
    }

    if (idx == ASX_MAX_BLOCKING_TASKS) {
        if (g_slot_count >= ASX_MAX_BLOCKING_TASKS) {
            pool_unlock();
            return ASX_E_RESOURCE_EXHAUSTED;
        }
        idx = g_slot_count++;
    }

    s = &g_slots[idx];
    s->generation = next_gen(s->generation);
    s->fn = fn;
    s->user_data = user_data;
    slot_state_store(s, ASX_BLOCKING_PENDING);

    if (completion_waker != NULL) {
        s->completion_waker = *completion_waker;
        s->has_waker = 1;
    } else {
        s->has_waker = 0;
    }

    out_handle->slot = idx;
    out_handle->generation = s->generation;
    g_job_ctx[idx].slot = idx;
    g_job_ctx[idx].generation = s->generation;

    active_inc();
    pool_unlock();

    if (hooks_use_blocking_submit(hooks)) {
        st = hooks->blocking.submit_fn(hooks->blocking.ctx, blocking_job_run, &g_job_ctx[idx]);
        if (st != ASX_OK) {
            pool_lock();
            if (g_slots[idx].generation == out_handle->generation &&
                slot_state_load(&g_slots[idx]) == ASX_BLOCKING_PENDING) {
                slot_state_store(&g_slots[idx], ASX_BLOCKING_COMPLETED);
                g_slots[idx].fn = NULL;
                g_slots[idx].user_data = NULL;
                g_slots[idx].has_waker = 0;
                g_slots[idx].result = 0;
                active_dec();
            }
            pool_unlock();
            return st;
        }
    } else {
        blocking_job_run(&g_job_ctx[idx]);
    }

    return ASX_OK;
}

static void blocking_job_run(void *job_ctx) {
    const asx_blocking_job_ctx *ctx = (const asx_blocking_job_ctx *)job_ctx;
    asx_blocking_fn fn;
    void *user_data;
    asx_waker waker;
    int has_waker;
    uint64_t result;
    uint32_t idx;
    uint16_t generation;

    if (ctx == NULL) return;
    idx = ctx->slot;
    generation = ctx->generation;
    if (idx >= ASX_MAX_BLOCKING_TASKS) return;

    pool_lock();
    if (idx >= g_slot_count || g_slots[idx].generation != generation ||
        slot_state_load(&g_slots[idx]) != ASX_BLOCKING_PENDING) {
        pool_unlock();
        return;
    }
    fn = g_slots[idx].fn;
    user_data = g_slots[idx].user_data;
    has_waker = g_slots[idx].has_waker;
    if (has_waker) { waker = g_slots[idx].completion_waker; }
    slot_state_store(&g_slots[idx], ASX_BLOCKING_RUNNING);
    pool_unlock();

    result = fn(user_data);

    pool_lock();
    if (idx < g_slot_count && g_slots[idx].generation == generation &&
        slot_state_load(&g_slots[idx]) == ASX_BLOCKING_RUNNING) {
        g_slots[idx].result = result;
        slot_state_store(&g_slots[idx], ASX_BLOCKING_COMPLETED);
        active_dec();
    } else {
        has_waker = 0;
    }
    pool_unlock();

    if (has_waker) {
        asx_status wake_st = asx_waker_wake(&waker);
        (void)wake_st;
    }
}

/* ------------------------------------------------------------------ */
/* Query                                                               */
/* ------------------------------------------------------------------ */

asx_blocking_state asx_blocking_get_state(const asx_blocking_handle *handle) {
    asx_blocking_state state;
    if (handle == NULL) return ASX_BLOCKING_COMPLETED;
    pool_lock();
    if (handle->slot >= g_slot_count) {
        pool_unlock();
        return ASX_BLOCKING_COMPLETED;
    }
    if (g_slots[handle->slot].generation != handle->generation) {
        pool_unlock();
        return ASX_BLOCKING_COMPLETED;
    }
    state = slot_state_load(&g_slots[handle->slot]);
    pool_unlock();
    return state;
}

asx_status asx_blocking_get_result(const asx_blocking_handle *handle, uint64_t *out_result) {
    const asx_blocking_slot *s;
    asx_status st;
    asx_blocking_state state;

    st = asx_surface_gate(ASX_SURFACE_BLOCKING);
    if (st != ASX_OK) return st;
    if (handle == NULL || out_result == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!g_initialized) return ASX_E_INVALID_STATE;

    pool_lock();
    if (handle->slot >= g_slot_count) {
        pool_unlock();
        return ASX_E_NOT_FOUND;
    }
    s = &g_slots[handle->slot];
    if (s->generation != handle->generation) {
        pool_unlock();
        return ASX_E_STALE_HANDLE;
    }
    state = slot_state_load(s);
    if (state != ASX_BLOCKING_COMPLETED) {
        pool_unlock();
        return ASX_E_PENDING;
    }

    *out_result = s->result;
    pool_unlock();
    return ASX_OK;
}

uint32_t asx_blocking_active_count(void) { return asx_atomic_u32_load(&g_active_count); }
#endif
