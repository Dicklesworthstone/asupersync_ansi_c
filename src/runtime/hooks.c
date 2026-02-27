/*
 * hooks.c — runtime hook management, validation, and dispatch
 *
 * Implements the hook contract from asx_config.h:
 *   - Hook initialization with safe defaults (malloc-based allocator, stderr log)
 *   - Deterministic mode validation (forbids ambient entropy, requires logical clock)
 *   - Allocator seal for hardened/no-allocation profiles
 *   - Hook-backed runtime helpers that dispatch through the active hook table
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx_config.h>
#include <asx/codec/codec.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Default hook implementations                                       */
/* ------------------------------------------------------------------ */

static void *default_malloc(void *ctx, size_t size) {
    (void)ctx;
    return malloc(size);
}

static void *default_realloc(void *ctx, void *ptr, size_t size) {
    (void)ctx;
    return realloc(ptr, size);
}

static void default_free(void *ctx, void *ptr) {
    (void)ctx;
    free(ptr);
}

static asx_time default_logical_clock(void *ctx) {
    (void)ctx;
    return 0; /* logical clock starts at 0, advanced by runtime */
}

static asx_time default_wall_clock(void *ctx) {
    (void)ctx;
    return 0; /* stub: real platforms override this */
}

static uint64_t default_seeded_entropy(void *ctx) {
    /* Deterministic PRNG: simple counter-based default.
     * Real deployments should provide a proper seeded PRNG. */
    static uint64_t state = 0x5DEECE66DULL;
    (void)ctx;
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return state;
}

static asx_status default_ghost_reactor_wait(void *ctx, uint64_t logical_step,
                                              uint32_t *ready_count) {
    (void)ctx; (void)logical_step;
    *ready_count = 0; /* no events in stub ghost reactor */
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Global hook state                                                  */
/* ------------------------------------------------------------------ */

static asx_runtime_hooks g_hooks;
static int g_hooks_installed = 0;

/* ------------------------------------------------------------------ */
/* Hook initialization                                                */
/* ------------------------------------------------------------------ */

asx_status asx_runtime_hooks_init(asx_runtime_hooks *hooks) {
    if (!hooks) return ASX_E_INVALID_ARGUMENT;
    memset(hooks, 0, sizeof(*hooks));

    /* Default allocator: stdlib malloc/realloc/free */
    hooks->allocator.malloc_fn  = default_malloc;
    hooks->allocator.realloc_fn = default_realloc;
    hooks->allocator.free_fn    = default_free;

    /* Log sink is opt-in (NULL by default) */

    /* Deterministic-safe defaults: logical clock, seeded PRNG, ghost reactor */
    hooks->clock.now_ns_fn         = default_wall_clock;
    hooks->clock.logical_now_ns_fn = default_logical_clock;
    hooks->entropy.random_u64_fn   = default_seeded_entropy;
    hooks->reactor.ghost_wait_fn   = default_ghost_reactor_wait;

    hooks->deterministic_seeded_prng = 1;
    hooks->allocator_sealed = 0;

    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Hook validation                                                    */
/* ------------------------------------------------------------------ */

asx_status asx_runtime_hooks_validate(const asx_runtime_hooks *hooks,
                                       int deterministic_mode) {
    if (!hooks) return ASX_E_INVALID_ARGUMENT;

    /* Allocator is mandatory */
    if (!hooks->allocator.malloc_fn || !hooks->allocator.free_fn)
        return ASX_E_INVALID_ARGUMENT;

    if (deterministic_mode) {
        /* Deterministic mode requires a logical clock */
        if (!hooks->clock.logical_now_ns_fn)
            return ASX_E_DETERMINISM_VIOLATION;

        /* Deterministic mode forbids ambient entropy unless seeded PRNG is configured */
        if (hooks->entropy.random_u64_fn && !hooks->deterministic_seeded_prng)
            return ASX_E_DETERMINISM_VIOLATION;

        /* Ghost reactor required if reactor is configured in deterministic mode */
        if (hooks->reactor.wait_fn && !hooks->reactor.ghost_wait_fn)
            return ASX_E_DETERMINISM_VIOLATION;
    } else {
        /* Live mode needs a real clock */
        if (!hooks->clock.now_ns_fn)
            return ASX_E_INVALID_ARGUMENT;
    }

    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Hook installation and retrieval                                    */
/* ------------------------------------------------------------------ */

asx_status asx_runtime_set_hooks(const asx_runtime_hooks *hooks) {
    if (!hooks) return ASX_E_INVALID_ARGUMENT;
    g_hooks = *hooks;
    g_hooks_installed = 1;
    return ASX_OK;
}

const asx_runtime_hooks *asx_runtime_get_hooks(void) {
    if (!g_hooks_installed) return NULL;
    return &g_hooks;
}

/* ------------------------------------------------------------------ */
/* Allocator seal                                                     */
/* ------------------------------------------------------------------ */

asx_status asx_runtime_seal_allocator(void) {
    if (!g_hooks_installed) return ASX_E_INVALID_STATE;
    if (g_hooks.allocator_sealed) return ASX_E_INVALID_STATE; /* already sealed */
    g_hooks.allocator_sealed = 1;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Hook-backed runtime helpers                                        */
/* ------------------------------------------------------------------ */

asx_status asx_runtime_alloc(size_t size, void **out_ptr) {
    void *p;
    if (!out_ptr) return ASX_E_INVALID_ARGUMENT;
    if (!g_hooks_installed) return ASX_E_INVALID_STATE;
    if (g_hooks.allocator_sealed) return ASX_E_ALLOCATOR_SEALED;
    if (!g_hooks.allocator.malloc_fn) return ASX_E_INVALID_STATE;

    p = g_hooks.allocator.malloc_fn(g_hooks.allocator.ctx, size);
    if (!p) return ASX_E_RESOURCE_EXHAUSTED;
    *out_ptr = p;
    return ASX_OK;
}

asx_status asx_runtime_realloc(void *ptr, size_t size, void **out_ptr) {
    void *p;
    if (!out_ptr) return ASX_E_INVALID_ARGUMENT;
    if (!g_hooks_installed) return ASX_E_INVALID_STATE;
    if (g_hooks.allocator_sealed) return ASX_E_ALLOCATOR_SEALED;
    if (!g_hooks.allocator.realloc_fn) return ASX_E_INVALID_STATE;

    p = g_hooks.allocator.realloc_fn(g_hooks.allocator.ctx, ptr, size);
    if (!p && size > 0) return ASX_E_RESOURCE_EXHAUSTED;
    *out_ptr = p;
    return ASX_OK;
}

asx_status asx_runtime_free(void *ptr) {
    if (!g_hooks_installed) return ASX_E_INVALID_STATE;
    if (!g_hooks.allocator.free_fn) return ASX_E_INVALID_STATE;
    g_hooks.allocator.free_fn(g_hooks.allocator.ctx, ptr);
    return ASX_OK;
}

asx_status asx_runtime_now_ns(asx_time *out_now) {
    if (!out_now) return ASX_E_INVALID_ARGUMENT;
    if (!g_hooks_installed) return ASX_E_INVALID_STATE;

#if ASX_DETERMINISTIC
    if (g_hooks.clock.logical_now_ns_fn) {
        *out_now = g_hooks.clock.logical_now_ns_fn(g_hooks.clock.ctx);
        return ASX_OK;
    }
#endif
    if (g_hooks.clock.now_ns_fn) {
        *out_now = g_hooks.clock.now_ns_fn(g_hooks.clock.ctx);
        return ASX_OK;
    }
    return ASX_E_INVALID_STATE;
}

asx_status asx_runtime_random_u64(uint64_t *out_value) {
    if (!out_value) return ASX_E_INVALID_ARGUMENT;
    if (!g_hooks_installed) return ASX_E_INVALID_STATE;

#if ASX_DETERMINISTIC
    /* In deterministic mode, entropy is forbidden unless seeded PRNG */
    if (!g_hooks.deterministic_seeded_prng)
        return ASX_E_INVALID_STATE;
#endif
    if (!g_hooks.entropy.random_u64_fn) return ASX_E_INVALID_STATE;
    *out_value = g_hooks.entropy.random_u64_fn(g_hooks.entropy.ctx);
    return ASX_OK;
}

asx_status asx_runtime_reactor_wait(uint32_t timeout_ms,
                                     uint32_t *out_ready_count,
                                     uint64_t logical_step) {
    if (!out_ready_count) return ASX_E_INVALID_ARGUMENT;
    if (!g_hooks_installed) return ASX_E_INVALID_STATE;

#if ASX_DETERMINISTIC
    if (g_hooks.reactor.ghost_wait_fn) {
        return g_hooks.reactor.ghost_wait_fn(g_hooks.reactor.ctx,
                                              logical_step, out_ready_count);
    }
#endif
    if (g_hooks.reactor.wait_fn) {
        return g_hooks.reactor.wait_fn(g_hooks.reactor.ctx,
                                        timeout_ms, out_ready_count);
    }
    return ASX_E_INVALID_STATE;
}

asx_status asx_runtime_log_write(int level, const char *message) {
    if (!g_hooks_installed) return ASX_E_INVALID_STATE;
    if (!g_hooks.log.write_fn) return ASX_OK; /* silent if no log hook */
    g_hooks.log.write_fn(g_hooks.log.ctx, level, message);
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Config initialization                                              */
/* ------------------------------------------------------------------ */

void asx_runtime_config_init(asx_runtime_config *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->size = (uint32_t)sizeof(*cfg);
    cfg->wait_policy = ASX_WAIT_YIELD;
    cfg->leak_response = ASX_LEAK_LOG;
    cfg->finalizer_poll_budget = 100;
    cfg->finalizer_time_budget_ns = (uint64_t)5000000000ULL; /* 5 seconds */
    cfg->finalizer_escalation = ASX_FINALIZER_BOUNDED_LOG;
    cfg->max_cancel_chain_depth = 16;
    cfg->max_cancel_chain_memory = 4096;
}
