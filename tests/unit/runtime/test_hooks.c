/*
 * test_hooks.c — unit tests for runtime hooks, config, fault injection
 *
 * Exercises the hook management, validation, dispatch, allocator seal,
 * fault injection, safety profiles, and containment policy APIs from
 * asx_config.h as implemented in hooks.c.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include "../../test_runtime.h"
#include <asx/asx.h>
#include <asx/asx_config.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Custom hook implementations for testing                             */
/* ------------------------------------------------------------------ */

static int g_alloc_count;
static int g_free_count;
static int g_realloc_count;
static int g_clock_count;
static int g_entropy_count;
static int g_reactor_count;
static int g_native_reactor_count;
static int g_blocking_submit_count;
static int g_blocking_shutdown_count;
static int g_log_count;
static int g_log_level;
static char g_log_buf[256];

static void *test_malloc(void *ctx, size_t size) {
    (void)ctx;
    g_alloc_count++;
    return malloc(size);
}

static void *test_realloc(void *ctx, void *ptr, size_t size) {
    (void)ctx;
    g_realloc_count++;
    return realloc(ptr, size);
}

static void test_free(void *ctx, void *ptr) {
    (void)ctx;
    g_free_count++;
    free(ptr);
}

static asx_time test_wall_clock(void *ctx) {
    (void)ctx;
    g_clock_count++;
    return 1000000;
}

static asx_time test_logical_clock(void *ctx) {
    (void)ctx;
    g_clock_count++;
    return 42;
}

static uint64_t test_entropy(void *ctx) {
    (void)ctx;
    g_entropy_count++;
    return 0xCAFEBABEu;
}

static asx_status test_ghost_reactor(void *ctx, uint64_t logical_step, uint32_t *ready_count) {
    (void)ctx;
    (void)logical_step;
    g_reactor_count++;
    *ready_count = 3;
    return ASX_OK;
}

static asx_status test_native_reactor(void *ctx, uint32_t timeout_ms, uint32_t *ready_count) {
    (void)ctx;
    (void)timeout_ms;
    g_native_reactor_count++;
    *ready_count = 7;
    return ASX_OK;
}

static void test_blocking_job(void *job_ctx) { (void)job_ctx; }

static asx_status test_blocking_submit(void *ctx, asx_blocking_job_fn job_fn, void *job_ctx) {
    (void)ctx;
    g_blocking_submit_count++;
    if (job_fn != NULL) { job_fn(job_ctx); }
    return ASX_OK;
}

static void test_blocking_shutdown(void *ctx) {
    (void)ctx;
    g_blocking_shutdown_count++;
}

static uint32_t test_blocking_capacity(void *ctx) {
    (void)ctx;
    return 8u;
}

static void test_log_sink(void *ctx, int level, const char *message) {
    (void)ctx;
    g_log_count++;
    g_log_level = level;
    if (message) {
        strncpy(g_log_buf, message, sizeof(g_log_buf) - 1);
        g_log_buf[sizeof(g_log_buf) - 1] = '\0';
    }
}

static void reset_counters(void) {
    g_alloc_count = 0;
    g_free_count = 0;
    g_realloc_count = 0;
    g_clock_count = 0;
    g_entropy_count = 0;
    g_reactor_count = 0;
    g_native_reactor_count = 0;
    g_blocking_submit_count = 0;
    g_blocking_shutdown_count = 0;
    g_log_count = 0;
    g_log_level = 0;
    g_log_buf[0] = '\0';
}

static asx_status install_test_hooks(void) {
    asx_runtime_hooks hooks;
    asx_status st = asx_runtime_hooks_init(&hooks);
    if (st != ASX_OK) return st;

    hooks.allocator.malloc_fn = test_malloc;
    hooks.allocator.realloc_fn = test_realloc;
    hooks.allocator.free_fn = test_free;
    hooks.clock.now_ns_fn = test_wall_clock;
    hooks.clock.logical_now_ns_fn = test_logical_clock;
    hooks.entropy.random_u64_fn = test_entropy;
    hooks.reactor.ghost_wait_fn = test_ghost_reactor;
    hooks.log.write_fn = test_log_sink;
    hooks.deterministic_seeded_prng = 1;

    return asx_runtime_set_hooks(&hooks);
}

static int ptr_in_span(const void *ptr, const unsigned char *base, size_t len) {
    const unsigned char *p = (const unsigned char *)ptr;
    return p >= base && (size_t)(p - base) < len;
}

static asx_status static_arena_alloc_once(size_t request, size_t *used_out) {
    unsigned char memory[128];
    asx_static_arena arena;
    asx_static_arena_config cfg;
    asx_runtime_hooks hooks;
    void *ptr = NULL;
    asx_status st;

    asx_runtime_reset(test_runtime());
    st = asx_static_arena_config_init(&cfg, memory, sizeof(memory), ASX_CLASS_R2);
    if (st != ASX_OK) return st;
    st = asx_static_arena_init(&arena, &cfg);
    if (st != ASX_OK) return st;
    st = asx_runtime_hooks_init(&hooks);
    if (st != ASX_OK) return st;
    st = asx_static_arena_bind_hooks(&arena, &hooks);
    if (st != ASX_OK) return st;
    st = asx_runtime_set_hooks(&hooks);
    if (st != ASX_OK) return st;

    st = asx_runtime_alloc(request, &ptr);
    if (used_out != NULL) *used_out = asx_static_arena_used(&arena);
    asx_runtime_reset(test_runtime());
    return st;
}

/* ------------------------------------------------------------------ */
/* Hook init                                                           */
/* ------------------------------------------------------------------ */

TEST(hooks_init_null_returns_error) {
    ASSERT_EQ(asx_runtime_hooks_init(NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(hooks_init_sets_defaults) {
    asx_runtime_hooks hooks;
    memset(&hooks, 0xFF, sizeof(hooks));

    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    ASSERT_TRUE(hooks.allocator.malloc_fn != NULL);
    ASSERT_TRUE(hooks.allocator.realloc_fn != NULL);
    ASSERT_TRUE(hooks.allocator.free_fn != NULL);
    ASSERT_TRUE(hooks.clock.logical_now_ns_fn != NULL);
    ASSERT_TRUE(hooks.entropy.random_u64_fn != NULL);
    ASSERT_EQ(hooks.deterministic_seeded_prng, 1);
    ASSERT_EQ(hooks.allocator_sealed, 0);
    ASSERT_TRUE(hooks.blocking.submit_fn == NULL);
    ASSERT_TRUE(hooks.blocking.shutdown_fn == NULL);
    ASSERT_TRUE(hooks.blocking.capacity_fn == NULL);
}

/* ------------------------------------------------------------------ */
/* Hook validation                                                     */
/* ------------------------------------------------------------------ */

TEST(hooks_validate_null_returns_error) {
    ASSERT_EQ(asx_runtime_hooks_validate(NULL, 0), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_runtime_hooks_validate(NULL, 1), ASX_E_INVALID_ARGUMENT);
}

TEST(hooks_validate_default_passes) {
    asx_runtime_hooks hooks;
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    ASSERT_EQ(asx_runtime_hooks_validate(&hooks, 1), ASX_OK);
    ASSERT_EQ(asx_runtime_hooks_validate(&hooks, 0), ASX_OK);
}

TEST(hooks_validate_no_malloc_fails) {
    asx_runtime_hooks hooks;
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    hooks.allocator.malloc_fn = NULL;
    ASSERT_EQ(asx_runtime_hooks_validate(&hooks, 0), ASX_E_INVALID_ARGUMENT);
}

TEST(hooks_validate_no_free_fails) {
    asx_runtime_hooks hooks;
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    hooks.allocator.free_fn = NULL;
    ASSERT_EQ(asx_runtime_hooks_validate(&hooks, 0), ASX_E_INVALID_ARGUMENT);
}

TEST(hooks_validate_deterministic_no_logical_clock_fails) {
    asx_runtime_hooks hooks;
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    hooks.clock.logical_now_ns_fn = NULL;
    ASSERT_EQ(asx_runtime_hooks_validate(&hooks, 1), ASX_E_DETERMINISM_VIOLATION);
}

TEST(hooks_validate_deterministic_unseeded_entropy_fails) {
    asx_runtime_hooks hooks;
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    hooks.deterministic_seeded_prng = 0;
    /* entropy fn set + not seeded → determinism violation */
    ASSERT_EQ(asx_runtime_hooks_validate(&hooks, 1), ASX_E_DETERMINISM_VIOLATION);
}

TEST(hooks_validate_live_no_wall_clock_fails) {
    asx_runtime_hooks hooks;
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    hooks.clock.now_ns_fn = NULL;
    ASSERT_EQ(asx_runtime_hooks_validate(&hooks, 0), ASX_E_INVALID_ARGUMENT);
}

TEST(hooks_validate_blocking_submit_requires_shutdown) {
    asx_runtime_hooks hooks;
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    hooks.blocking.submit_fn = test_blocking_submit;
    ASSERT_EQ(asx_runtime_hooks_validate(&hooks, 0), ASX_E_INVALID_ARGUMENT);
}

TEST(hooks_validate_blocking_shutdown_requires_submit) {
    asx_runtime_hooks hooks;
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    hooks.blocking.shutdown_fn = test_blocking_shutdown;
    ASSERT_EQ(asx_runtime_hooks_validate(&hooks, 0), ASX_E_INVALID_ARGUMENT);
}

TEST(hooks_validate_blocking_capacity_requires_submit) {
    asx_runtime_hooks hooks;
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    hooks.blocking.capacity_fn = test_blocking_capacity;
    ASSERT_EQ(asx_runtime_hooks_validate(&hooks, 0), ASX_E_INVALID_ARGUMENT);
}

TEST(hooks_validate_blocking_pair_passes) {
    asx_runtime_hooks hooks;
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    hooks.blocking.submit_fn = test_blocking_submit;
    hooks.blocking.shutdown_fn = test_blocking_shutdown;
    hooks.blocking.capacity_fn = test_blocking_capacity;
    ASSERT_EQ(asx_runtime_hooks_validate(&hooks, 0), ASX_OK);
    ASSERT_EQ(asx_runtime_hooks_validate(&hooks, 1), ASX_OK);
    hooks.blocking.submit_fn(hooks.blocking.ctx, test_blocking_job, NULL);
    hooks.blocking.shutdown_fn(hooks.blocking.ctx);
    ASSERT_EQ(g_blocking_submit_count, 1);
    ASSERT_EQ(g_blocking_shutdown_count, 1);
}

/* ------------------------------------------------------------------ */
/* Hook install and retrieval                                          */
/* ------------------------------------------------------------------ */

TEST(set_hooks_null_returns_error) {
    ASSERT_EQ(asx_runtime_set_hooks(NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(set_hooks_installs_and_get_retrieves) {
    const asx_runtime_hooks *retrieved;

    asx_runtime_reset(test_runtime());
    ASSERT_EQ(install_test_hooks(), ASX_OK);

    retrieved = asx_runtime_get_hooks();
    ASSERT_TRUE(retrieved != NULL);
    ASSERT_TRUE(retrieved->allocator.malloc_fn == test_malloc);
    ASSERT_TRUE(retrieved->allocator.free_fn == test_free);
    ASSERT_TRUE(retrieved->log.write_fn == test_log_sink);
}

TEST(runtime_reset_clears_installed_hooks) {
    asx_runtime_reset(test_runtime());
    ASSERT_EQ(install_test_hooks(), ASX_OK);
    ASSERT_TRUE(asx_runtime_get_hooks() != NULL);

    asx_runtime_reset(test_runtime());
    ASSERT_TRUE(asx_runtime_get_hooks() == NULL);
}

TEST(default_entropy_restarts_with_the_runtime) {
    /* The default seeded stream restarts with each runtime, as the virtual
     * clock does: two runtimes in one process draw the same values. */
    asx_runtime_hooks hooks;
    uint64_t first[3];
    uint64_t again[3];
    uint32_t i;

    asx_runtime_reset(test_runtime());
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    ASSERT_EQ(asx_runtime_set_hooks(&hooks), ASX_OK);
    for (i = 0; i < 3u; i++) ASSERT_EQ(asx_runtime_random_u64(test_runtime(), &first[i]), ASX_OK);
    ASSERT_TRUE(first[0] != first[1] && first[1] != first[2]);

    asx_runtime_reset(test_runtime());
    ASSERT_EQ(asx_runtime_set_hooks(&hooks), ASX_OK);
    for (i = 0; i < 3u; i++) ASSERT_EQ(asx_runtime_random_u64(test_runtime(), &again[i]), ASX_OK);
    for (i = 0; i < 3u; i++) ASSERT_EQ(again[i], first[i]);
    asx_runtime_reset(test_runtime());
}

/* ------------------------------------------------------------------ */
/* Allocator seal                                                      */
/* ------------------------------------------------------------------ */

TEST(seal_without_hooks_returns_invalid_state) {
    asx_runtime_reset(test_runtime());
    /* No hooks installed after reset */
    ASSERT_EQ(asx_runtime_seal_allocator(), ASX_E_INVALID_STATE);
}

TEST(seal_then_alloc_returns_sealed) {
    void *ptr = NULL;

    asx_runtime_reset(test_runtime());
    ASSERT_EQ(install_test_hooks(), ASX_OK);
    ASSERT_EQ(asx_runtime_seal_allocator(), ASX_OK);
    ASSERT_EQ(asx_runtime_alloc(64, &ptr), ASX_E_ALLOCATOR_SEALED);
    ASSERT_TRUE(ptr == NULL);
}

TEST(seal_then_realloc_returns_sealed) {
    void *ptr = NULL;

    asx_runtime_reset(test_runtime());
    ASSERT_EQ(install_test_hooks(), ASX_OK);
    ASSERT_EQ(asx_runtime_seal_allocator(), ASX_OK);
    ASSERT_EQ(asx_runtime_realloc(NULL, 64, &ptr), ASX_E_ALLOCATOR_SEALED);
}

TEST(double_seal_returns_invalid_state) {
    asx_runtime_reset(test_runtime());
    ASSERT_EQ(install_test_hooks(), ASX_OK);
    ASSERT_EQ(asx_runtime_seal_allocator(), ASX_OK);
    ASSERT_EQ(asx_runtime_seal_allocator(), ASX_E_INVALID_STATE);
}

TEST(static_arena_config_init_sets_resource_defaults) {
    unsigned char memory[512];
    asx_static_arena_config cfg;
    asx_resource_limits r1;

    ASSERT_EQ(asx_static_arena_config_init(&cfg, memory, sizeof(memory), ASX_CLASS_R1), ASX_OK);
    r1 = asx_resource_limits_for_class(ASX_CLASS_R1);
    ASSERT_EQ(cfg.size, (uint32_t)sizeof(cfg));
    ASSERT_EQ(cfg.memory, memory);
    ASSERT_EQ(cfg.memory_len, sizeof(memory));
    ASSERT_EQ(cfg.alignment, sizeof(void *));
    ASSERT_EQ((int)cfg.resource_class, (int)ASX_CLASS_R1);
    ASSERT_EQ(cfg.limits.max_regions, r1.max_regions);
    ASSERT_EQ(cfg.limits.max_tasks, r1.max_tasks);
    ASSERT_EQ(cfg.limits.max_channels, r1.max_channels);
    ASSERT_EQ(cfg.trace_ring_capacity, 64u);
    ASSERT_TRUE(cfg.capture_bytes_per_region > 0u);
    ASSERT_TRUE(cfg.cleanup_slots_per_region > 0u);
}

TEST(static_arena_rejects_invalid_config) {
    unsigned char memory[128];
    asx_static_arena arena;
    asx_static_arena_config cfg;

    ASSERT_EQ(asx_static_arena_config_init(NULL, memory, sizeof(memory), ASX_CLASS_R2),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_static_arena_config_init(&cfg, memory, sizeof(memory), (asx_resource_class)99),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_static_arena_config_init(&cfg, memory, sizeof(memory), ASX_CLASS_R2), ASX_OK);
    cfg.alignment = 3u;
    ASSERT_EQ(asx_static_arena_init(&arena, &cfg), ASX_E_INVALID_ARGUMENT);
    cfg.alignment = sizeof(void *);
    cfg.limits.max_tasks = cfg.limits.max_tasks + 1u;
    ASSERT_EQ(asx_static_arena_init(&arena, &cfg), ASX_E_INVALID_ARGUMENT);
}

TEST(static_arena_rejects_undersized_memory) {
    unsigned char memory[1];
    asx_static_arena arena;
    asx_static_arena_config cfg;

    ASSERT_EQ(asx_static_arena_config_init(&cfg, memory, sizeof(memory), ASX_CLASS_R2), ASX_OK);
    ASSERT_EQ(asx_static_arena_init(&arena, &cfg), ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(asx_static_arena_used(&arena), 0u);
}

TEST(static_arena_allocates_from_static_span) {
    unsigned char memory[256];
    asx_static_arena arena;
    asx_static_arena_config cfg;
    asx_runtime_hooks hooks;
    void *ptr = NULL;

    asx_runtime_reset(test_runtime());
    ASSERT_EQ(asx_static_arena_config_init(&cfg, memory, sizeof(memory), ASX_CLASS_R2), ASX_OK);
    ASSERT_EQ(asx_static_arena_init(&arena, &cfg), ASX_OK);
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    ASSERT_EQ(asx_static_arena_bind_hooks(&arena, &hooks), ASX_OK);
    ASSERT_EQ(asx_runtime_set_hooks(&hooks), ASX_OK);

    ASSERT_EQ(asx_runtime_alloc(32u, &ptr), ASX_OK);
    ASSERT_TRUE(ptr != NULL);
    ASSERT_TRUE(ptr_in_span(ptr, memory, sizeof(memory)));
    ASSERT_TRUE(asx_static_arena_used(&arena) > 0u);
    ASSERT_TRUE(asx_static_arena_remaining(&arena) < sizeof(memory));
    ASSERT_EQ(asx_runtime_free(ptr), ASX_OK);
}

TEST(static_arena_zero_size_allocation_uses_static_span) {
    unsigned char memory[128];
    asx_static_arena arena;
    asx_static_arena_config cfg;
    asx_runtime_hooks hooks;
    void *ptr = NULL;

    asx_runtime_reset(test_runtime());
    ASSERT_EQ(asx_static_arena_config_init(&cfg, memory, sizeof(memory), ASX_CLASS_R2), ASX_OK);
    ASSERT_EQ(asx_static_arena_init(&arena, &cfg), ASX_OK);
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    ASSERT_EQ(asx_static_arena_bind_hooks(&arena, &hooks), ASX_OK);
    ASSERT_EQ(asx_runtime_set_hooks(&hooks), ASX_OK);

    ASSERT_EQ(asx_runtime_alloc(0u, &ptr), ASX_OK);
    ASSERT_TRUE(ptr != NULL);
    ASSERT_TRUE(ptr_in_span(ptr, memory, sizeof(memory)));
    ASSERT_TRUE(asx_static_arena_used(&arena) > 0u);
}

TEST(static_arena_boundary_allocation_consumes_usable_span) {
    size_t request;
    size_t max_request = 0u;
    size_t used = 0u;
    size_t max_used = 0u;

    for (request = 1u; request <= 128u; request++) {
        if (static_arena_alloc_once(request, &used) == ASX_OK) {
            max_request = request;
            max_used = used;
        } else if (max_request > 0u) {
            break;
        }
    }

    ASSERT_TRUE(max_request > 0u);
    ASSERT_EQ(max_used, 128u);
    ASSERT_EQ(static_arena_alloc_once(max_request + 1u, &used), ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(used, 0u);
}

TEST(static_arena_exhaustion_is_failure_atomic) {
    unsigned char memory[96];
    asx_static_arena arena;
    asx_static_arena_config cfg;
    asx_runtime_hooks hooks;
    void *ptr = NULL;
    void *too_large = NULL;
    size_t used_before;

    asx_runtime_reset(test_runtime());
    ASSERT_EQ(asx_static_arena_config_init(&cfg, memory, sizeof(memory), ASX_CLASS_R2), ASX_OK);
    ASSERT_EQ(asx_static_arena_init(&arena, &cfg), ASX_OK);
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    ASSERT_EQ(asx_static_arena_bind_hooks(&arena, &hooks), ASX_OK);
    ASSERT_EQ(asx_runtime_set_hooks(&hooks), ASX_OK);
    ASSERT_EQ(asx_runtime_alloc(24u, &ptr), ASX_OK);
    used_before = asx_static_arena_used(&arena);

    ASSERT_EQ(asx_runtime_alloc(sizeof(memory), &too_large), ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_TRUE(too_large == NULL);
    ASSERT_EQ(asx_static_arena_used(&arena), used_before);
    ASSERT_EQ(arena.failed_allocation_count, 1u);
    ASSERT_EQ(asx_runtime_free(ptr), ASX_OK);
}

TEST(static_arena_realloc_preserves_contents) {
    unsigned char memory[256];
    asx_static_arena arena;
    asx_static_arena_config cfg;
    asx_runtime_hooks hooks;
    unsigned char *ptr = NULL;
    unsigned char *grown = NULL;

    asx_runtime_reset(test_runtime());
    ASSERT_EQ(asx_static_arena_config_init(&cfg, memory, sizeof(memory), ASX_CLASS_R2), ASX_OK);
    ASSERT_EQ(asx_static_arena_init(&arena, &cfg), ASX_OK);
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    ASSERT_EQ(asx_static_arena_bind_hooks(&arena, &hooks), ASX_OK);
    ASSERT_EQ(asx_runtime_set_hooks(&hooks), ASX_OK);

    ASSERT_EQ(asx_runtime_alloc(8u, (void **)&ptr), ASX_OK);
    ptr[0] = 0x12u;
    ptr[7] = 0x34u;
    ASSERT_EQ(asx_runtime_realloc(ptr, 32u, (void **)&grown), ASX_OK);
    ASSERT_TRUE(grown != NULL);
    ASSERT_EQ(grown[0], 0x12u);
    ASSERT_EQ(grown[7], 0x34u);
}

TEST(static_arena_seal_blocks_alloc_but_not_free) {
    unsigned char memory[256];
    asx_static_arena arena;
    asx_static_arena_config cfg;
    asx_runtime_hooks hooks;
    void *ptr = NULL;
    void *after_seal = NULL;

    asx_runtime_reset(test_runtime());
    ASSERT_EQ(asx_static_arena_config_init(&cfg, memory, sizeof(memory), ASX_CLASS_R2), ASX_OK);
    ASSERT_EQ(asx_static_arena_init(&arena, &cfg), ASX_OK);
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    ASSERT_EQ(asx_static_arena_bind_hooks(&arena, &hooks), ASX_OK);
    ASSERT_EQ(asx_runtime_set_hooks(&hooks), ASX_OK);

    ASSERT_EQ(asx_runtime_alloc(16u, &ptr), ASX_OK);
    ASSERT_EQ(asx_runtime_seal_allocator(), ASX_OK);
    ASSERT_EQ(asx_runtime_alloc(16u, &after_seal), ASX_E_ALLOCATOR_SEALED);
    ASSERT_TRUE(after_seal == NULL);
    ASSERT_EQ(asx_runtime_free(ptr), ASX_OK);
}

TEST(static_arena_bind_can_install_presealed_hooks) {
    unsigned char memory[256];
    asx_static_arena arena;
    asx_static_arena_config cfg;
    asx_runtime_hooks hooks;
    void *ptr = NULL;

    asx_runtime_reset(test_runtime());
    ASSERT_EQ(asx_static_arena_config_init(&cfg, memory, sizeof(memory), ASX_CLASS_R2), ASX_OK);
    cfg.seal_after_init = 1u;
    ASSERT_EQ(asx_static_arena_init(&arena, &cfg), ASX_OK);
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    ASSERT_EQ(asx_static_arena_bind_hooks(&arena, &hooks), ASX_OK);
    ASSERT_EQ(asx_runtime_set_hooks(&hooks), ASX_OK);

    ASSERT_EQ(asx_runtime_alloc(16u, &ptr), ASX_E_ALLOCATOR_SEALED);
    ASSERT_TRUE(ptr == NULL);
}

/* ------------------------------------------------------------------ */
/* Hook-backed allocator dispatch                                      */
/* ------------------------------------------------------------------ */

TEST(alloc_null_out_returns_error) {
    asx_runtime_reset(test_runtime());
    ASSERT_EQ(install_test_hooks(), ASX_OK);
    ASSERT_EQ(asx_runtime_alloc(64, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(alloc_no_hooks_returns_hook_missing) {
    void *ptr = NULL;
    asx_runtime_reset(test_runtime());
    ASSERT_EQ(asx_runtime_alloc(64, &ptr), ASX_E_HOOK_MISSING);
}

TEST(alloc_dispatches_to_hook) {
    void *ptr = NULL;

    asx_runtime_reset(test_runtime());
    reset_counters();
    ASSERT_EQ(install_test_hooks(), ASX_OK);
    ASSERT_EQ(asx_runtime_alloc(64, &ptr), ASX_OK);
    ASSERT_TRUE(ptr != NULL);
    ASSERT_TRUE(g_alloc_count > 0);

    /* Clean up */
    ASSERT_EQ(asx_runtime_free(ptr), ASX_OK);
}

TEST(realloc_dispatches_to_hook) {
    void *ptr = NULL;
    void *ptr2 = NULL;

    asx_runtime_reset(test_runtime());
    reset_counters();
    ASSERT_EQ(install_test_hooks(), ASX_OK);
    ASSERT_EQ(asx_runtime_alloc(32, &ptr), ASX_OK);
    ASSERT_EQ(asx_runtime_realloc(ptr, 128, &ptr2), ASX_OK);
    ASSERT_TRUE(ptr2 != NULL);
    ASSERT_TRUE(g_realloc_count > 0);

    ASSERT_EQ(asx_runtime_free(ptr2), ASX_OK);
}

TEST(free_no_hooks_returns_hook_missing) {
    asx_runtime_reset(test_runtime());
    ASSERT_EQ(asx_runtime_free(NULL), ASX_E_HOOK_MISSING);
}

TEST(free_dispatches_to_hook) {
    void *ptr = NULL;

    asx_runtime_reset(test_runtime());
    reset_counters();
    ASSERT_EQ(install_test_hooks(), ASX_OK);
    ASSERT_EQ(asx_runtime_alloc(64, &ptr), ASX_OK);
    ASSERT_EQ(asx_runtime_free(ptr), ASX_OK);
    ASSERT_TRUE(g_free_count > 0);
}

/* ------------------------------------------------------------------ */
/* Clock dispatch                                                      */
/* ------------------------------------------------------------------ */

TEST(now_ns_null_returns_error) {
    asx_runtime_reset(test_runtime());
    ASSERT_EQ(install_test_hooks(), ASX_OK);
    ASSERT_EQ(asx_runtime_now_ns(test_runtime(), NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(now_ns_no_hooks_returns_hook_missing) {
    asx_time t;
    asx_runtime_reset(test_runtime());
    ASSERT_EQ(asx_runtime_now_ns(test_runtime(), &t), ASX_E_HOOK_MISSING);
}

TEST(now_ns_dispatches_by_build_mode) {
    asx_time t = 0;

    asx_runtime_reset(test_runtime());
    reset_counters();
    ASSERT_EQ(install_test_hooks(), ASX_OK);

    ASSERT_EQ(asx_runtime_now_ns(test_runtime(), &t), ASX_OK);
    /* Deterministic builds read the logical clock (42), live builds the
     * wall clock (1000000). */
#if ASX_DETERMINISTIC
    ASSERT_EQ(t, (asx_time)42);
#else
    ASSERT_EQ(t, (asx_time)1000000);
#endif
    ASSERT_TRUE(g_clock_count > 0);
}

/* ------------------------------------------------------------------ */
/* Entropy dispatch                                                    */
/* ------------------------------------------------------------------ */

TEST(random_u64_null_returns_error) {
    asx_runtime_reset(test_runtime());
    ASSERT_EQ(install_test_hooks(), ASX_OK);
    ASSERT_EQ(asx_runtime_random_u64(test_runtime(), NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(random_u64_no_hooks_returns_hook_missing) {
    uint64_t v;
    asx_runtime_reset(test_runtime());
    ASSERT_EQ(asx_runtime_random_u64(test_runtime(), &v), ASX_E_HOOK_MISSING);
}

TEST(random_u64_missing_entropy_stream_is_refused) {
    asx_runtime_hooks hooks;
    uint64_t v = 0;

    asx_runtime_reset(test_runtime());
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    hooks.entropy.random_u64_fn = NULL;
    hooks.deterministic_seeded_prng = 1;
    ASSERT_EQ(asx_runtime_set_hooks(&hooks), ASX_OK);
    /* A configuration error in deterministic builds, a missing hook in
     * live ones. */
#if ASX_DETERMINISTIC
    ASSERT_EQ(asx_runtime_random_u64(test_runtime(), &v), ASX_E_INVALID_STATE);
#else
    ASSERT_EQ(asx_runtime_random_u64(test_runtime(), &v), ASX_E_HOOK_MISSING);
#endif
}

TEST(random_u64_dispatches_to_hook) {
    uint64_t v = 0;

    asx_runtime_reset(test_runtime());
    reset_counters();
    ASSERT_EQ(install_test_hooks(), ASX_OK);

    ASSERT_EQ(asx_runtime_random_u64(test_runtime(), &v), ASX_OK);
    ASSERT_EQ(v, (uint64_t)0xCAFEBABEu);
    ASSERT_TRUE(g_entropy_count > 0);
}

/* ------------------------------------------------------------------ */
/* Fallible entropy source and secure draws (bd-9kll.16.5)             */
/* ------------------------------------------------------------------ */

#define SECURE_SENTINEL UINT64_C(0xA5A5A5A5A5A5A5A5)
#define TRY_VALUE UINT64_C(0x1122334455667788)

static asx_status g_try_status;
static int g_try_calls;

static asx_status test_try_entropy(void *ctx, uint64_t *out) {
    (void)ctx;
    g_try_calls++;
    if (g_try_status != ASX_OK) return g_try_status;
    *out = TRY_VALUE;
    return ASX_OK;
}

/* Hooks whose fallible source answers with `try_status`, next to an
 * infallible stand-in (test_entropy) that must never answer a draw. */
static asx_status install_fallible_hooks(asx_status try_status) {
    asx_runtime_hooks hooks;
    asx_status st = asx_runtime_hooks_init(&hooks);
    if (st != ASX_OK) return st;
    hooks.entropy.random_u64_fn = test_entropy;
    hooks.entropy.try_u64_fn = test_try_entropy;
    hooks.deterministic_seeded_prng = 1;
    g_try_status = try_status;
    g_try_calls = 0;
    reset_counters();
    return asx_runtime_set_hooks(&hooks);
}

TEST(random_u64_prefers_fallible_source) {
    uint64_t v = 0;

    asx_runtime_reset(test_runtime());
    ASSERT_EQ(install_fallible_hooks(ASX_OK), ASX_OK);
    ASSERT_EQ(asx_runtime_random_u64(test_runtime(), &v), ASX_OK);
    ASSERT_EQ(v, (uint64_t)TRY_VALUE);
    ASSERT_EQ(g_try_calls, 1);
    ASSERT_EQ(g_entropy_count, 0);
}

TEST(random_u64_reports_fallible_source_failure) {
    uint64_t v = SECURE_SENTINEL;

    asx_runtime_reset(test_runtime());
    ASSERT_EQ(install_fallible_hooks(ASX_E_RESOURCE_EXHAUSTED), ASX_OK);
    /* The source's own status reaches the caller, no value is invented,
     * and the infallible hook is not consulted as a stand-in. */
    ASSERT_EQ(asx_runtime_random_u64(test_runtime(), &v), ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(v, (uint64_t)SECURE_SENTINEL);
    ASSERT_EQ(g_try_calls, 1);
    ASSERT_EQ(g_entropy_count, 0);
}

TEST(hooks_validate_treats_fallible_source_as_entropy) {
    asx_runtime_hooks hooks;

    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    hooks.entropy.random_u64_fn = NULL;
    hooks.entropy.try_u64_fn = test_try_entropy;
    hooks.deterministic_seeded_prng = 0;
    /* Deterministic mode forbids an unseeded source, however it is
     * installed; live mode accepts it. */
    ASSERT_EQ(asx_runtime_hooks_validate(&hooks, 1), ASX_E_DETERMINISM_VIOLATION);
    ASSERT_EQ(asx_runtime_hooks_validate(&hooks, 0), ASX_OK);
    hooks.deterministic_seeded_prng = 1;
    ASSERT_EQ(asx_runtime_hooks_validate(&hooks, 1), ASX_OK);
}

TEST(secure_draw_rejects_null_and_missing_hooks) {
    uint64_t v = SECURE_SENTINEL;

    asx_runtime_reset(test_runtime());
    ASSERT_EQ(asx_runtime_random_secure_u64(test_runtime(), &v), ASX_E_HOOK_MISSING);
    ASSERT_EQ(v, (uint64_t)SECURE_SENTINEL);
    ASSERT_EQ(install_test_hooks(), ASX_OK);
    ASSERT_EQ(asx_runtime_random_secure_u64(test_runtime(), NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(secure_draw_serves_fallible_source) {
    uint64_t v = 0;

    asx_runtime_reset(test_runtime());
    ASSERT_EQ(install_fallible_hooks(ASX_OK), ASX_OK);
    ASSERT_EQ(asx_runtime_random_secure_u64(test_runtime(), &v), ASX_OK);
    ASSERT_EQ(v, (uint64_t)TRY_VALUE);
    ASSERT_EQ(g_entropy_count, 0);
}

TEST(secure_draw_reports_fallible_source_failure) {
    uint64_t v = SECURE_SENTINEL;

    asx_runtime_reset(test_runtime());
    ASSERT_EQ(install_fallible_hooks(ASX_E_HOOK_MISSING), ASX_OK);
    /* An OS source that failed is a status, never a guessable key. */
    ASSERT_EQ(asx_runtime_random_secure_u64(test_runtime(), &v), ASX_E_HOOK_MISSING);
    ASSERT_EQ(v, (uint64_t)SECURE_SENTINEL);
    ASSERT_EQ(g_entropy_count, 0);
}

#if ASX_DETERMINISTIC
TEST(secure_draw_keeps_seeded_stream_in_deterministic_builds) {
    asx_runtime_hooks hooks;
    uint64_t general = 0;
    uint64_t secure = 0;

    asx_runtime_reset(test_runtime());
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    ASSERT_EQ(asx_runtime_set_hooks(&hooks), ASX_OK);
    ASSERT_EQ(asx_runtime_random_u64(test_runtime(), &general), ASX_OK);

    /* The same default table, restarted: the secure draw is the first
     * value of the same seeded stream, which replay depends on. */
    asx_runtime_reset(test_runtime());
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    ASSERT_EQ(asx_runtime_set_hooks(&hooks), ASX_OK);
    ASSERT_EQ(asx_runtime_random_secure_u64(test_runtime(), &secure), ASX_OK);
    ASSERT_EQ(secure, general);
}
#else
TEST(secure_draw_refuses_builtin_seeded_prng_in_live_builds) {
    asx_runtime_hooks hooks;
    uint64_t v = SECURE_SENTINEL;

    asx_runtime_reset(test_runtime());
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    ASSERT_EQ(asx_runtime_set_hooks(&hooks), ASX_OK);

    /* This is what a live FREESTANDING / EMBEDDED_ROUTER / HFT / AUTOMOTIVE
     * runtime holds: the fixed-seed PRNG. Its draws are refused with a
     * typed status and write nothing... */
    ASSERT_EQ(asx_runtime_random_secure_u64(test_runtime(), &v), ASX_E_HOOK_INVALID);
    ASSERT_EQ(v, (uint64_t)SECURE_SENTINEL);
    /* ...while the general draw still serves callers that do not protect
     * anything with it. */
    ASSERT_EQ(asx_runtime_random_u64(test_runtime(), &v), ASX_OK);
}

TEST(secure_draw_refuses_when_no_source_in_live_builds) {
    asx_runtime_hooks hooks;
    uint64_t v = SECURE_SENTINEL;

    asx_runtime_reset(test_runtime());
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    hooks.entropy.random_u64_fn = NULL;
    ASSERT_EQ(asx_runtime_set_hooks(&hooks), ASX_OK);
    ASSERT_EQ(asx_runtime_random_secure_u64(test_runtime(), &v), ASX_E_HOOK_MISSING);
    ASSERT_EQ(v, (uint64_t)SECURE_SENTINEL);
}

TEST(secure_draw_serves_application_source_in_live_builds) {
    uint64_t v = 0;

    asx_runtime_reset(test_runtime());
    reset_counters();
    /* An application's own source (an infallible hook here, installed
     * over the defaults the way builder entropy hooks are) is its own to
     * vouch for: it is served, and consulted. */
    ASSERT_EQ(install_test_hooks(), ASX_OK);
    ASSERT_EQ(asx_runtime_random_secure_u64(test_runtime(), &v), ASX_OK);
    ASSERT_EQ(v, (uint64_t)0xCAFEBABEu);
    ASSERT_TRUE(g_entropy_count > 0);
}

TEST(default_init_security_draw_follows_the_profile_in_live_builds) {
    asx_runtime rt;
    uint64_t v = SECURE_SENTINEL;

    asx_runtime_reset(test_runtime());
    ASSERT_EQ(asx_runtime_init_default(&rt), ASX_OK);
#if defined(ASX_PROFILE_POSIX) || defined(ASX_PROFILE_WIN32)
    /* The live POSIX default installs the OS source; the live WIN32
     * default installs BCryptGenRandom. */
    ASSERT_EQ(asx_runtime_random_secure_u64(&rt, &v), ASX_OK);
    ASSERT_TRUE(v != (uint64_t)SECURE_SENTINEL);
#else
    /* CORE, FREESTANDING, EMBEDDED_ROUTER, HFT and AUTOMOTIVE install no
     * platform source: the runtime holds the built-in PRNG, and security
     * draws are refused rather than answered with its predictable stream. */
    ASSERT_EQ(asx_runtime_random_secure_u64(&rt, &v), ASX_E_HOOK_INVALID);
    ASSERT_EQ(v, (uint64_t)SECURE_SENTINEL);
#endif
    asx_runtime_shutdown(&rt);
}
#endif

/* ------------------------------------------------------------------ */
/* Reactor dispatch                                                    */
/* ------------------------------------------------------------------ */

TEST(reactor_wait_null_returns_error) {
    asx_runtime_reset(test_runtime());
    ASSERT_EQ(install_test_hooks(), ASX_OK);
    ASSERT_EQ(asx_runtime_reactor_wait(100, NULL, 0), ASX_E_INVALID_ARGUMENT);
}

TEST(reactor_wait_no_hooks_returns_hook_missing) {
    uint32_t ready = 0;
    asx_runtime_reset(test_runtime());
    ASSERT_EQ(asx_runtime_reactor_wait(100, &ready, 0), ASX_E_HOOK_MISSING);
}

TEST(reactor_wait_dispatches_to_ghost_hook) {
    uint32_t ready = 0;

    asx_runtime_reset(test_runtime());
    reset_counters();
    ASSERT_EQ(install_test_hooks(), ASX_OK);

#if ASX_DETERMINISTIC
    ASSERT_EQ(asx_runtime_reactor_wait(100, &ready, 5), ASX_OK);
    ASSERT_EQ(ready, 3u);
    ASSERT_TRUE(g_reactor_count > 0);
#else
    /* Live builds never wait on the ghost reactor: with no native hook
     * there is nothing to wait on. */
    ASSERT_EQ(asx_runtime_reactor_wait(100, &ready, 5), ASX_E_HOOK_MISSING);
    ASSERT_EQ(g_reactor_count, 0);
#endif
}

TEST(reactor_wait_prefers_native_hook_when_both_are_installed) {
    uint32_t ready = 0;
    asx_runtime_hooks hooks;

    asx_runtime_reset(test_runtime());
    reset_counters();
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    hooks.reactor.wait_fn = test_native_reactor;
    hooks.reactor.ghost_wait_fn = test_ghost_reactor;
    ASSERT_EQ(asx_runtime_set_hooks(&hooks), ASX_OK);

    ASSERT_EQ(asx_runtime_reactor_wait(100, &ready, 5), ASX_OK);
#if ASX_DETERMINISTIC
    /* In deterministic mode, ghost reactor takes priority for reproducibility. */
    ASSERT_EQ(ready, 3u);
    ASSERT_EQ(g_reactor_count, 1);
    ASSERT_EQ(g_native_reactor_count, 0);
#else
    /* In non-deterministic mode, native reactor takes priority for live I/O. */
    ASSERT_EQ(ready, 7u);
    ASSERT_EQ(g_native_reactor_count, 1);
    ASSERT_EQ(g_reactor_count, 0);
#endif
}

/* ------------------------------------------------------------------ */
/* Log dispatch                                                        */
/* ------------------------------------------------------------------ */

TEST(log_write_no_hooks_returns_hook_missing) {
    asx_runtime_reset(test_runtime());
    ASSERT_EQ(asx_runtime_log_write(1, "test"), ASX_E_HOOK_MISSING);
}

TEST(log_write_dispatches_to_hook) {
    asx_runtime_reset(test_runtime());
    reset_counters();
    ASSERT_EQ(install_test_hooks(), ASX_OK);

    ASSERT_EQ(asx_runtime_log_write(3, "hello world"), ASX_OK);
    ASSERT_EQ(g_log_count, 1);
    ASSERT_EQ(g_log_level, 3);
    ASSERT_EQ(strcmp(g_log_buf, "hello world"), 0);
}

TEST(log_write_no_sink_is_silent) {
    asx_runtime_hooks hooks;

    asx_runtime_reset(test_runtime());
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    hooks.log.write_fn = NULL;
    ASSERT_EQ(asx_runtime_set_hooks(&hooks), ASX_OK);

    /* Should return OK even without a log sink */
    ASSERT_EQ(asx_runtime_log_write(1, "ignored"), ASX_OK);
}

/* ------------------------------------------------------------------ */
/* Fault injection                                                     */
/* ------------------------------------------------------------------ */

TEST(fault_inject_null_returns_error) { ASSERT_EQ(asx_fault_inject(NULL), ASX_E_INVALID_ARGUMENT); }

TEST(fault_inject_none_kind_returns_error) {
    asx_fault_injection fi;
    memset(&fi, 0, sizeof(fi));
    fi.kind = ASX_FAULT_NONE;
    ASSERT_EQ(asx_fault_inject(&fi), ASX_E_INVALID_ARGUMENT);
}

TEST(fault_inject_and_count) {
    asx_fault_injection fi;

    asx_runtime_reset(test_runtime());
    ASSERT_EQ(asx_fault_clear(), ASX_OK);
    ASSERT_EQ(asx_fault_injection_count(), 0u);

    memset(&fi, 0, sizeof(fi));
    fi.kind = ASX_FAULT_CLOCK_SKEW;
    fi.param = 100;
    ASSERT_EQ(asx_fault_inject(&fi), ASX_OK);
    ASSERT_EQ(asx_fault_injection_count(), 1u);

    ASSERT_EQ(asx_fault_clear(), ASX_OK);
    ASSERT_EQ(asx_fault_injection_count(), 0u);
}

TEST(fault_inject_exhaustion) {
    asx_fault_injection fi;
    uint32_t i;

    ASSERT_EQ(asx_fault_clear(), ASX_OK);

    memset(&fi, 0, sizeof(fi));
    fi.kind = ASX_FAULT_ALLOC_FAIL;
    /* Fill all 8 slots */
    for (i = 0; i < 8; i++) { ASSERT_EQ(asx_fault_inject(&fi), ASX_OK); }
    /* 9th should fail */
    ASSERT_EQ(asx_fault_inject(&fi), ASX_E_RESOURCE_EXHAUSTED);

    ASSERT_EQ(asx_fault_clear(), ASX_OK);
}

TEST(fault_alloc_fail_triggers) {
    asx_fault_injection fi;
    void *ptr = NULL;

    asx_runtime_reset(test_runtime());
    ASSERT_EQ(install_test_hooks(), ASX_OK);
    ASSERT_EQ(asx_fault_clear(), ASX_OK);

    memset(&fi, 0, sizeof(fi));
    fi.kind = ASX_FAULT_ALLOC_FAIL;
    fi.trigger_after = 0;
    fi.trigger_count = 0; /* permanent */
    ASSERT_EQ(asx_fault_inject(&fi), ASX_OK);

    ASSERT_EQ(asx_runtime_alloc(64, &ptr), ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_TRUE(ptr == NULL);

    ASSERT_EQ(asx_fault_clear(), ASX_OK);
}

TEST(fault_clock_skew_adds_offset) {
    asx_fault_injection fi;
    asx_time t = 0;

    asx_runtime_reset(test_runtime());
    ASSERT_EQ(install_test_hooks(), ASX_OK);
    ASSERT_EQ(asx_fault_clear(), ASX_OK);

    memset(&fi, 0, sizeof(fi));
    fi.kind = ASX_FAULT_CLOCK_SKEW;
    fi.param = 1000;
    ASSERT_EQ(asx_fault_inject(&fi), ASX_OK);

    ASSERT_EQ(asx_runtime_now_ns(test_runtime(), &t), ASX_OK);
    /* The skew adds 1000 to the clock the build reads: the logical clock
     * (42) or, live, the wall clock (1000000). */
#if ASX_DETERMINISTIC
    ASSERT_EQ(t, (asx_time)1042);
#else
    ASSERT_EQ(t, (asx_time)1001000);
#endif

    ASSERT_EQ(asx_fault_clear(), ASX_OK);
}

TEST(fault_entropy_const_overrides) {
    asx_fault_injection fi;
    uint64_t v = 0;

    asx_runtime_reset(test_runtime());
    ASSERT_EQ(install_test_hooks(), ASX_OK);
    ASSERT_EQ(asx_fault_clear(), ASX_OK);

    memset(&fi, 0, sizeof(fi));
    fi.kind = ASX_FAULT_ENTROPY_CONST;
    fi.param = 0xDEADBEEFu;
    ASSERT_EQ(asx_fault_inject(&fi), ASX_OK);

    ASSERT_EQ(asx_runtime_random_u64(test_runtime(), &v), ASX_OK);
    ASSERT_EQ(v, (uint64_t)0xDEADBEEFu);

    ASSERT_EQ(asx_fault_clear(), ASX_OK);
}

/* ------------------------------------------------------------------ */
/* Config init                                                         */
/* ------------------------------------------------------------------ */

TEST(config_init_null_safe) { ASSERT_EQ(asx_runtime_config_init(NULL), ASX_E_INVALID_ARGUMENT); }

TEST(config_init_sets_defaults) {
    asx_runtime_config cfg;
    memset(&cfg, 0xFF, sizeof(cfg));

    ASSERT_EQ(asx_runtime_config_init(&cfg), ASX_OK);
    ASSERT_EQ(cfg.size, (uint32_t)sizeof(cfg));
    ASSERT_EQ(cfg.leak_response, (asx_leak_response)ASX_LEAK_PANIC);
    ASSERT_EQ(cfg.finalizer_poll_budget, 100u);
    ASSERT_EQ(cfg.finalizer_time_budget_ns, (uint64_t)5000000000ULL);
    ASSERT_EQ(cfg.finalizer_escalation, (asx_finalizer_escalation)ASX_FINALIZER_BOUNDED_LOG);
    ASSERT_EQ(cfg.max_cancel_chain_depth, 16u);
    ASSERT_EQ(cfg.max_cancel_chain_memory, 4096u);
}

/* ------------------------------------------------------------------ */
/* Safety profile queries                                              */
/* ------------------------------------------------------------------ */

TEST(safety_profile_active_returns_valid) {
    asx_safety_profile p = asx_safety_profile_active();
    ASSERT_TRUE(p == ASX_SAFETY_DEBUG || p == ASX_SAFETY_HARDENED || p == ASX_SAFETY_RELEASE);
}

TEST(safety_profile_str_returns_known_strings) {
    ASSERT_EQ(strcmp(asx_safety_profile_str(ASX_SAFETY_DEBUG), "debug"), 0);
    ASSERT_EQ(strcmp(asx_safety_profile_str(ASX_SAFETY_HARDENED), "hardened"), 0);
    ASSERT_EQ(strcmp(asx_safety_profile_str(ASX_SAFETY_RELEASE), "release"), 0);
}

/* ------------------------------------------------------------------ */
/* Containment policy                                                  */
/* ------------------------------------------------------------------ */

TEST(containment_policy_for_profile_maps_correctly) {
    ASSERT_EQ(asx_containment_policy_for_profile(ASX_SAFETY_DEBUG), ASX_CONTAIN_FAIL_FAST);
    ASSERT_EQ(asx_containment_policy_for_profile(ASX_SAFETY_HARDENED), ASX_CONTAIN_POISON_REGION);
    ASSERT_EQ(asx_containment_policy_for_profile(ASX_SAFETY_RELEASE), ASX_CONTAIN_ERROR_ONLY);
}

TEST(containment_policy_active_matches_profile) {
    asx_safety_profile p = asx_safety_profile_active();
    asx_containment_policy expected = asx_containment_policy_for_profile(p);
    ASSERT_EQ(asx_containment_policy_active(), expected);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    fprintf(stderr, "=== test_hooks ===\n");

    /* --- Run "no hooks installed" tests first to exercise pristine state --- */
    RUN_TEST(seal_without_hooks_returns_invalid_state);
    RUN_TEST(alloc_no_hooks_returns_hook_missing);
    RUN_TEST(free_no_hooks_returns_hook_missing);
    RUN_TEST(now_ns_no_hooks_returns_hook_missing);
    RUN_TEST(random_u64_no_hooks_returns_hook_missing);
    RUN_TEST(reactor_wait_no_hooks_returns_hook_missing);
    RUN_TEST(log_write_no_hooks_returns_hook_missing);

    /* Hook init */
    RUN_TEST(hooks_init_null_returns_error);
    RUN_TEST(hooks_init_sets_defaults);

    /* Hook validation */
    RUN_TEST(hooks_validate_null_returns_error);
    RUN_TEST(hooks_validate_default_passes);
    RUN_TEST(hooks_validate_no_malloc_fails);
    RUN_TEST(hooks_validate_no_free_fails);
    RUN_TEST(hooks_validate_deterministic_no_logical_clock_fails);
    RUN_TEST(hooks_validate_deterministic_unseeded_entropy_fails);
    RUN_TEST(hooks_validate_live_no_wall_clock_fails);
    RUN_TEST(hooks_validate_blocking_submit_requires_shutdown);
    RUN_TEST(hooks_validate_blocking_shutdown_requires_submit);
    RUN_TEST(hooks_validate_blocking_capacity_requires_submit);
    RUN_TEST(hooks_validate_blocking_pair_passes);

    /* Hook install/retrieval */
    RUN_TEST(set_hooks_null_returns_error);
    asx_runtime_reset(test_runtime());
    RUN_TEST(set_hooks_installs_and_get_retrieves);
    asx_runtime_reset(test_runtime());
    RUN_TEST(runtime_reset_clears_installed_hooks);
    RUN_TEST(default_entropy_restarts_with_the_runtime);

    /* Allocator seal */
    asx_runtime_reset(test_runtime());
    RUN_TEST(seal_then_alloc_returns_sealed);
    asx_runtime_reset(test_runtime());
    RUN_TEST(seal_then_realloc_returns_sealed);
    asx_runtime_reset(test_runtime());
    RUN_TEST(double_seal_returns_invalid_state);
    asx_runtime_reset(test_runtime());
    RUN_TEST(static_arena_config_init_sets_resource_defaults);
    asx_runtime_reset(test_runtime());
    RUN_TEST(static_arena_rejects_invalid_config);
    asx_runtime_reset(test_runtime());
    RUN_TEST(static_arena_rejects_undersized_memory);
    asx_runtime_reset(test_runtime());
    RUN_TEST(static_arena_allocates_from_static_span);
    asx_runtime_reset(test_runtime());
    RUN_TEST(static_arena_zero_size_allocation_uses_static_span);
    asx_runtime_reset(test_runtime());
    RUN_TEST(static_arena_boundary_allocation_consumes_usable_span);
    asx_runtime_reset(test_runtime());
    RUN_TEST(static_arena_exhaustion_is_failure_atomic);
    asx_runtime_reset(test_runtime());
    RUN_TEST(static_arena_realloc_preserves_contents);
    asx_runtime_reset(test_runtime());
    RUN_TEST(static_arena_seal_blocks_alloc_but_not_free);
    asx_runtime_reset(test_runtime());
    RUN_TEST(static_arena_bind_can_install_presealed_hooks);

    /* Allocator dispatch */
    asx_runtime_reset(test_runtime());
    RUN_TEST(alloc_null_out_returns_error);
    asx_runtime_reset(test_runtime());
    RUN_TEST(alloc_dispatches_to_hook);
    asx_runtime_reset(test_runtime());
    RUN_TEST(realloc_dispatches_to_hook);
    asx_runtime_reset(test_runtime());
    RUN_TEST(free_dispatches_to_hook);

    /* Clock dispatch */
    asx_runtime_reset(test_runtime());
    RUN_TEST(now_ns_null_returns_error);
    asx_runtime_reset(test_runtime());
    RUN_TEST(now_ns_dispatches_by_build_mode);

    /* Entropy dispatch */
    asx_runtime_reset(test_runtime());
    RUN_TEST(random_u64_null_returns_error);
    asx_runtime_reset(test_runtime());
    RUN_TEST(random_u64_no_hooks_returns_hook_missing);
    asx_runtime_reset(test_runtime());
    RUN_TEST(random_u64_missing_entropy_stream_is_refused);
    asx_runtime_reset(test_runtime());
    RUN_TEST(random_u64_dispatches_to_hook);

    /* Fallible entropy source and secure draws (bd-9kll.16.5) */
    asx_runtime_reset(test_runtime());
    RUN_TEST(random_u64_prefers_fallible_source);
    asx_runtime_reset(test_runtime());
    RUN_TEST(random_u64_reports_fallible_source_failure);
    asx_runtime_reset(test_runtime());
    RUN_TEST(hooks_validate_treats_fallible_source_as_entropy);
    asx_runtime_reset(test_runtime());
    RUN_TEST(secure_draw_rejects_null_and_missing_hooks);
    asx_runtime_reset(test_runtime());
    RUN_TEST(secure_draw_serves_fallible_source);
    asx_runtime_reset(test_runtime());
    RUN_TEST(secure_draw_reports_fallible_source_failure);
#if ASX_DETERMINISTIC
    asx_runtime_reset(test_runtime());
    RUN_TEST(secure_draw_keeps_seeded_stream_in_deterministic_builds);
#else
    asx_runtime_reset(test_runtime());
    RUN_TEST(secure_draw_refuses_builtin_seeded_prng_in_live_builds);
    asx_runtime_reset(test_runtime());
    RUN_TEST(secure_draw_refuses_when_no_source_in_live_builds);
    asx_runtime_reset(test_runtime());
    RUN_TEST(secure_draw_serves_application_source_in_live_builds);
    asx_runtime_reset(test_runtime());
    RUN_TEST(default_init_security_draw_follows_the_profile_in_live_builds);
#endif

    /* Reactor dispatch */
    asx_runtime_reset(test_runtime());
    RUN_TEST(reactor_wait_null_returns_error);
    asx_runtime_reset(test_runtime());
    RUN_TEST(reactor_wait_dispatches_to_ghost_hook);
    asx_runtime_reset(test_runtime());
    RUN_TEST(reactor_wait_prefers_native_hook_when_both_are_installed);

    /* Log dispatch */
    asx_runtime_reset(test_runtime());
    RUN_TEST(log_write_dispatches_to_hook);
    asx_runtime_reset(test_runtime());
    RUN_TEST(log_write_no_sink_is_silent);

    /* Fault injection */
    RUN_TEST(fault_inject_null_returns_error);
    RUN_TEST(fault_inject_none_kind_returns_error);
    asx_runtime_reset(test_runtime());
    RUN_TEST(fault_inject_and_count);
    RUN_TEST(fault_inject_exhaustion);
    asx_runtime_reset(test_runtime());
    RUN_TEST(fault_alloc_fail_triggers);
    asx_runtime_reset(test_runtime());
    RUN_TEST(fault_clock_skew_adds_offset);
    asx_runtime_reset(test_runtime());
    RUN_TEST(fault_entropy_const_overrides);

    /* Config init */
    RUN_TEST(config_init_null_safe);
    RUN_TEST(config_init_sets_defaults);

    /* Safety profile */
    RUN_TEST(safety_profile_active_returns_valid);
    RUN_TEST(safety_profile_str_returns_known_strings);

    /* Containment policy */
    RUN_TEST(containment_policy_for_profile_maps_correctly);
    RUN_TEST(containment_policy_active_matches_profile);

    TEST_REPORT();
    return test_failures;
}
