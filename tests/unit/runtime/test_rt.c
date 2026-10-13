/*
 * test_rt.c — unit tests for runtime object lifecycle and state queries
 *
 * Tests cover:
 *   - Config validation
 *   - Runtime init/shutdown lifecycle
 *   - Default initialization
 *   - State queries (region/task/obligation counts)
 *   - Config/hooks retrieval
 *   - Guard behavior for uninitialized/NULL runtime
 *   - Capacity queries
 *
 * SPDX-License-Identifier: MIT
 */

/* pipe, write, close: the io-driver tests register real descriptors. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "../../../src/runtime/runtime_internal.h"
#include "../../test_harness.h"
#include "../../test_task.h"
#include <asx/asx.h>
#include <asx/core/cleanup.h>
#include <string.h>

#if defined(_WIN32)
#include <stdlib.h>
static void rt_setenv(const char *name, const char *value) { _putenv_s(name, value); }
static void rt_unsetenv(const char *name) { _putenv_s(name, ""); }
#else
#include <stdlib.h>
extern int setenv(const char *name, const char *value, int overwrite);
extern int unsetenv(const char *name);
static void rt_setenv(const char *name, const char *value) { (void)setenv(name, value, 1); }
static void rt_unsetenv(const char *name) { (void)unsetenv(name); }
#endif

#if ASX_HAS_NATIVE_IO_DRIVER
#if defined(_WIN32)
static int rt_io_fd_open(int fds[2]) {
    fds[0] = fds[1] = -1;
    return 42;
}
static void rt_io_fd_close(const int fds[2]) { (void)fds; }
static void rt_io_fd_make_readable(const int fds[2]) { (void)fds; }
#else
#include <unistd.h>
/* A pipe's read end. A live build registers descriptors with the native
 * reactor, which refuses a number that is not an open descriptor; the
 * deterministic driver takes any number. A byte written to the pipe makes
 * it readable for real. */
static int rt_io_fd_open(int fds[2]) { return pipe(fds) == 0 ? fds[0] : -1; }
static void rt_io_fd_close(const int fds[2]) {
    (void)close(fds[0]);
    (void)close(fds[1]);
}
static void rt_io_fd_make_readable(const int fds[2]) {
    char byte = 1;
    ssize_t n = write(fds[1], &byte, 1);
    (void)n;
}
#endif
#endif

/* Suppress warn_unused_result in test helpers where we don't check */
static asx_status rt_test_sink_;
#define MUST_OK(expr)                                                                              \
    do {                                                                                           \
        rt_test_sink_ = (expr);                                                                    \
        (void)rt_test_sink_;                                                                       \
    } while (0)

/* ------------------------------------------------------------------ */
/* Dummy poll function for task spawn tests                            */
/* ------------------------------------------------------------------ */

static asx_status dummy_poll(void *user_data, asx_task_id self) {
    (void)user_data;
    (void)self;
    return ASX_OK;
}

static void cleanup_mark(void *ctx) {
    int *flag = (int *)ctx;
    *flag += 1;
}

#if ASX_HAS_BLOCKING_SURFACE
static uint64_t add_one(void *user_data) {
    uint64_t value = *(uint64_t *)user_data;
    return value + 1u;
}
#endif

static uint32_t g_rt_ready_count;

static asx_status rt_fixed_ready_reactor(void *ctx, uint64_t logical_step, uint32_t *ready_count) {
    (void)ctx;
    (void)logical_step;
    if (ready_count == NULL) return ASX_E_INVALID_ARGUMENT;
    *ready_count = g_rt_ready_count;
    return ASX_OK;
}

static asx_status lab_noop_step(asx_lab *lab, void *user_data) {
    (void)user_data;
    return asx_lab_open_region(lab, (asx_region_id *)user_data);
}

static void clear_rt_test_env(void) {
    rt_unsetenv("ASX_RUNTIME_PRESET");
    rt_unsetenv("ASX_RUNTIME_WAIT_POLICY");
    rt_unsetenv("ASX_RUNTIME_IO_BACKEND");
    rt_unsetenv("ASX_RUNTIME_LEAK_RESPONSE");
    rt_unsetenv("ASX_RUNTIME_FINALIZER_POLL_BUDGET");
    rt_unsetenv("ASX_RUNTIME_FINALIZER_TIME_BUDGET_NS");
    rt_unsetenv("ASX_RUNTIME_FINALIZER_ESCALATION");
    rt_unsetenv("ASX_RUNTIME_MAX_CANCEL_CHAIN_DEPTH");
    rt_unsetenv("ASX_RUNTIME_MAX_CANCEL_CHAIN_MEMORY");

    rt_unsetenv("TESTRT_PRESET");
    rt_unsetenv("TESTRT_WAIT_POLICY");
    rt_unsetenv("TESTRT_IO_BACKEND");
    rt_unsetenv("TESTRT_LEAK_RESPONSE");
    rt_unsetenv("TESTRT_FINALIZER_POLL_BUDGET");
    rt_unsetenv("TESTRT_FINALIZER_TIME_BUDGET_NS");
    rt_unsetenv("TESTRT_FINALIZER_ESCALATION");
    rt_unsetenv("TESTRT_MAX_CANCEL_CHAIN_DEPTH");
    rt_unsetenv("TESTRT_MAX_CANCEL_CHAIN_MEMORY");
}

/* ------------------------------------------------------------------ */
/* Helper: create a valid config                                       */
/* ------------------------------------------------------------------ */

static void make_valid_config(asx_runtime_config *cfg) { asx_runtime_config_init(cfg); }

static void make_valid_hooks(asx_runtime_hooks *hooks) { asx_runtime_hooks_init(hooks); }

/* ------------------------------------------------------------------ */
/* Config validation tests                                             */
/* ------------------------------------------------------------------ */

TEST(config_validate_null_fails) {
    ASSERT_EQ(asx_runtime_config_validate(NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(config_validate_bad_size_fails) {
    asx_runtime_config cfg;
    make_valid_config(&cfg);
    cfg.size = 0;
    ASSERT_EQ(asx_runtime_config_validate(&cfg), ASX_E_INVALID_ARGUMENT);
}

TEST(config_validate_zero_cancel_depth_fails) {
    asx_runtime_config cfg;
    make_valid_config(&cfg);
    cfg.max_cancel_chain_depth = 0;
    ASSERT_EQ(asx_runtime_config_validate(&cfg), ASX_E_INVALID_ARGUMENT);
}

TEST(config_validate_zero_finalizer_budget_fails) {
    asx_runtime_config cfg;
    make_valid_config(&cfg);
    cfg.finalizer_poll_budget = 0;
    ASSERT_EQ(asx_runtime_config_validate(&cfg), ASX_E_INVALID_ARGUMENT);
}

TEST(config_validate_invalid_wait_policy_fails) {
    asx_runtime_config cfg;
    make_valid_config(&cfg);
    cfg.wait_policy = (asx_wait_policy)99;
    ASSERT_EQ(asx_runtime_config_validate(&cfg), ASX_E_INVALID_ARGUMENT);
}

TEST(config_validate_invalid_io_backend_fails) {
    asx_runtime_config cfg;
    make_valid_config(&cfg);
    cfg.io_backend = (asx_io_backend)99;
    ASSERT_EQ(asx_runtime_config_validate(&cfg), ASX_E_INVALID_ARGUMENT);
}

TEST(config_validate_invalid_leak_response_fails) {
    asx_runtime_config cfg;
    make_valid_config(&cfg);
    cfg.leak_response = (asx_leak_response)99;
    ASSERT_EQ(asx_runtime_config_validate(&cfg), ASX_E_INVALID_ARGUMENT);
}

TEST(config_validate_invalid_finalizer_escalation_fails) {
    asx_runtime_config cfg;
    make_valid_config(&cfg);
    cfg.finalizer_escalation = (asx_finalizer_escalation)99;
    ASSERT_EQ(asx_runtime_config_validate(&cfg), ASX_E_INVALID_ARGUMENT);
}

TEST(config_validate_invalid_leak_escalation_fails) {
    asx_runtime_config cfg;
    asx_leak_escalation_config escalation;
    make_valid_config(&cfg);
    escalation.threshold = 1u;
    escalation.escalate_to = (asx_leak_response)99;
    cfg.leak_escalation = &escalation;
    ASSERT_EQ(asx_runtime_config_validate(&cfg), ASX_E_INVALID_ARGUMENT);
}

TEST(config_validate_defaults_ok) {
    asx_runtime_config cfg;
    make_valid_config(&cfg);
    ASSERT_EQ(asx_runtime_config_validate(&cfg), ASX_OK);
    ASSERT_EQ(cfg.io_backend, ASX_IO_BACKEND_GHOST);
}

/* ------------------------------------------------------------------ */
/* Init/shutdown lifecycle tests                                       */
/* ------------------------------------------------------------------ */

TEST(init_null_rt_fails) {
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    make_valid_config(&cfg);
    make_valid_hooks(&hooks);
    ASSERT_EQ(asx_runtime_init(NULL, &cfg, &hooks), ASX_E_INVALID_ARGUMENT);
}

TEST(mismatched_creation_preserves_live_work_and_all_objects) {
    asx_runtime active, destination;
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    asx_runtime_builder builder;
    unsigned char saved[sizeof(destination)];
    asx_region_id region;
    asx_task_id task;
    asx_task_state state;
    asx_budget budget;
    uint64_t mismatch = ASX_CONFIG_FINGERPRINT ^ UINT64_C(1);

    ASSERT_EQ(asx_runtime_init_default(&active), ASX_OK);
    ASSERT_EQ(asx_region_open(&region), ASX_OK);
    ASSERT_EQ(asx_task_spawn(region, dummy_poll, NULL, &task), ASX_OK);
    ASSERT_EQ(asx_runtime_config_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    ASSERT_EQ(asx_runtime_builder_init(&builder), ASX_OK);
    memset(&destination, 0xa5, sizeof(destination));
    memcpy(saved, &destination, sizeof(destination));

    ASSERT_EQ(asx_runtime_init_checked(&destination, &cfg, &hooks, mismatch),
              ASX_E_CONFIG_MISMATCH);
    ASSERT_EQ(asx_runtime_init_default_checked(&destination, mismatch), ASX_E_CONFIG_MISMATCH);
    ASSERT_EQ(asx_runtime_init_from_env_checked(&destination, "ASX_RT_TEST_", mismatch),
              ASX_E_CONFIG_MISMATCH);
    ASSERT_EQ(asx_runtime_builder_build_checked(&builder, &destination, mismatch),
              ASX_E_CONFIG_MISMATCH);
    ASSERT_TRUE(memcmp(saved, &destination, sizeof(destination)) == 0);
    ASSERT_TRUE(asx_runtime_is_initialized(&active));
    ASSERT_EQ(asx_task_get_state(task, &state), ASX_OK);
    ASSERT_EQ(state, ASX_TASK_CREATED);
    budget = asx_budget_from_polls(64u);
    ASSERT_EQ(asx_scheduler_run(region, &budget), ASX_OK);
    ASSERT_EQ(asx_task_get_state(task, &state), ASX_OK);
    ASSERT_EQ(state, ASX_TASK_COMPLETED);
    asx_runtime_shutdown(&active);
}

TEST(init_null_config_fails) {
    asx_runtime rt;
    asx_runtime_hooks hooks;
    make_valid_hooks(&hooks);
    ASSERT_EQ(asx_runtime_init(&rt, NULL, &hooks), ASX_E_INVALID_ARGUMENT);
}

TEST(init_null_hooks_fails) {
    asx_runtime rt;
    asx_runtime_config cfg;
    make_valid_config(&cfg);
    ASSERT_EQ(asx_runtime_init(&rt, &cfg, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(init_bad_config_fails) {
    asx_runtime rt;
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    make_valid_config(&cfg);
    make_valid_hooks(&hooks);
    cfg.size = 0; /* invalid */
    ASSERT_EQ(asx_runtime_init(&rt, &cfg, &hooks), ASX_E_INVALID_ARGUMENT);
}

TEST(init_invalid_enum_config_fails) {
    asx_runtime rt;
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    make_valid_config(&cfg);
    make_valid_hooks(&hooks);
    cfg.wait_policy = (asx_wait_policy)99;
    ASSERT_EQ(asx_runtime_init(&rt, &cfg, &hooks), ASX_E_INVALID_ARGUMENT);
}

TEST(init_unsupported_io_backend_fails_closed) {
    asx_runtime rt;
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    make_valid_config(&cfg);
    make_valid_hooks(&hooks);
    cfg.io_backend = ASX_IO_BACKEND_IO_URING;
    ASSERT_EQ(asx_runtime_init(&rt, &cfg, &hooks), ASX_E_PERMISSION_DENIED);
}

TEST(init_invalid_leak_escalation_config_fails) {
    asx_runtime rt;
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    asx_leak_escalation_config escalation;
    make_valid_config(&cfg);
    make_valid_hooks(&hooks);
    escalation.threshold = 1u;
    escalation.escalate_to = (asx_leak_response)99;
    cfg.leak_escalation = &escalation;
    ASSERT_EQ(asx_runtime_init(&rt, &cfg, &hooks), ASX_E_INVALID_ARGUMENT);
}

TEST(init_success) {
    asx_runtime rt;
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    make_valid_config(&cfg);
    make_valid_hooks(&hooks);
    ASSERT_EQ(asx_runtime_init(&rt, &cfg, &hooks), ASX_OK);
    ASSERT_TRUE(asx_runtime_is_initialized(&rt));
    ASSERT_NE(rt.generation, 0u);
    asx_runtime_shutdown(&rt);
}

TEST(init_generation_increments) {
    asx_runtime a, b;
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    uint32_t gen_a;
    make_valid_config(&cfg);
    make_valid_hooks(&hooks);

    MUST_OK(asx_runtime_init(&a, &cfg, &hooks));
    gen_a = a.generation;
    asx_runtime_shutdown(&a);

    MUST_OK(asx_runtime_init(&b, &cfg, &hooks));
    ASSERT_TRUE(b.generation > gen_a);
    asx_runtime_shutdown(&b);
}

/* The task commits the obligation it holds in its own poll: only a holder's
 * poll settles an obligation. */
typedef struct {
    int polls;
    asx_obligation_id obligation;
    asx_status commit;
} preserved_task_state;

static asx_status poll_preserved_task(void *data, asx_task_id self) {
    preserved_task_state *state = (preserved_task_state *)data;
    (void)self;
    state->polls++;
    state->commit = asx_obligation_commit(state->obligation);
    return ASX_OK;
}

TEST(conflicting_init_preserves_live_runtime_and_destination) {
    asx_runtime a, b, before_a, before_b;
    asx_runtime_config cfg;
    asx_runtime_hooks hooks, after_hooks;
    asx_region_id region;
    asx_task_id task;
    asx_obligation_id obligation;
    asx_obligation_state obligation_state;
    asx_budget run;
    asx_outcome outcome;
    preserved_task_state state;
    asx_runtime_reset();
    make_valid_config(&cfg);
    make_valid_hooks(&hooks);
    MUST_OK(asx_runtime_init(&a, &cfg, &hooks));
    ASSERT_EQ(asx_region_open(&region), ASX_OK);
    state.polls = 0;
    state.obligation = ASX_INVALID_ID;
    state.commit = ASX_E_PENDING;
    ASSERT_EQ(asx_task_spawn(region, poll_preserved_task, &state, &task), ASX_OK);
    /* Held by the live, not yet polled task, which commits it when it runs. */
    ASSERT_EQ(asx_obligation_reserve_ex(region, ASX_OBLIGATION_KIND_GENERIC, task, &obligation),
              ASX_OK);
    state.obligation = obligation;
    memset(&b, 0, sizeof(b));
    b.generation = 42u;
    memcpy(&before_a, &a, sizeof(a));
    memcpy(&before_b, &b, sizeof(b));
    cfg.size = 0u;
    ASSERT_EQ(asx_runtime_init(&b, &cfg, &hooks), ASX_E_INVALID_ARGUMENT);
    make_valid_config(&cfg);
    hooks.allocator.malloc_fn = NULL;
    ASSERT_EQ(asx_runtime_init(&b, &cfg, &hooks), ASX_E_INVALID_ARGUMENT);
    make_valid_hooks(&hooks);
    ASSERT_EQ(asx_runtime_init(&b, &cfg, &hooks), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_runtime_init(&a, &cfg, &hooks), ASX_E_INVALID_STATE);
    ASSERT_EQ(memcmp(&a, &before_a, sizeof(a)), 0);
    ASSERT_EQ(memcmp(&b, &before_b, sizeof(b)), 0);
    ASSERT_TRUE(asx_runtime_is_initialized(&a));
    ASSERT_EQ(asx_runtime_get_hooks_from(&a, &after_hooks), ASX_OK);
    ASSERT_TRUE(after_hooks.allocator.malloc_fn == hooks.allocator.malloc_fn);
    ASSERT_EQ(asx_obligation_get_state(obligation, &obligation_state), ASX_OK);
    ASSERT_EQ(obligation_state, ASX_OBLIGATION_RESERVED);
    run = asx_budget_from_polls(8u);
    ASSERT_EQ(asx_scheduler_run(region, &run), ASX_OK);
    ASSERT_EQ(state.polls, 1);
    ASSERT_EQ(state.commit, ASX_OK);
    ASSERT_EQ(asx_obligation_get_state(obligation, &obligation_state), ASX_OK);
    ASSERT_EQ(obligation_state, ASX_OBLIGATION_COMMITTED);
    ASSERT_EQ(asx_task_get_outcome(task, &outcome), ASX_OK);
    ASSERT_EQ(asx_outcome_severity_of(&outcome), ASX_OUTCOME_OK);
    asx_runtime_shutdown(&a);
    MUST_OK(asx_runtime_init(&b, &cfg, &hooks));
    ASSERT_TRUE(asx_runtime_is_initialized(&b));
    asx_runtime_shutdown(&b);
}

/* ---- Runtime-owned state storage (K18, bd-9kll.3.20) ---- */

/* An aligned block of asx_runtime_storage_size() bytes carved from `raw`,
 * filled with `fill` so a runtime that bound it is told from one that did
 * not. */
typedef struct {
    unsigned char *raw;
    unsigned char *block;
    size_t size;
} storage_block;

static int storage_block_make(storage_block *b, unsigned char fill) {
    size_t size = asx_runtime_storage_size();
    size_t align = asx_runtime_storage_align();
    uintptr_t p;

    memset(b, 0, sizeof(*b));
    b->raw = (unsigned char *)malloc(size + align);
    if (b->raw == NULL) return 0;
    p = ((uintptr_t)b->raw + (align - 1u)) & ~((uintptr_t)align - 1u);
    b->block = (unsigned char *)p;
    b->size = size;
    memset(b->block, fill, size);
    return 1;
}

static void storage_block_free(storage_block *b) {
    free(b->raw);
    memset(b, 0, sizeof(*b));
}

static int pointer_in_block(const void *p, const storage_block *b) {
    const unsigned char *c = (const unsigned char *)p;
    return c >= b->block && c < b->block + b->size;
}

static int block_is_filled(const storage_block *b, unsigned char fill) {
    size_t i;
    for (i = 0; i < b->size; i++) {
        if (b->block[i] != fill) return 0;
    }
    return 1;
}

TEST(runtime_storage_size_and_alignment_are_published) {
    size_t size = asx_runtime_storage_size();
    size_t align = asx_runtime_storage_align();

    ASSERT_TRUE(size > 0u);
    ASSERT_TRUE(align > 0u);
    ASSERT_EQ(align & (align - 1u), (size_t)0u);
    ASSERT_EQ(size % align, (size_t)0u);
    ASSERT_TRUE(size >=
                (size_t)ASX_MAX_REGIONS + (size_t)ASX_MAX_TASKS + (size_t)ASX_MAX_OBLIGATIONS);
}

TEST(runtime_arenas_live_in_the_callers_storage_and_return_to_the_static_block) {
    asx_runtime rt;
    asx_runtime_config cfg;
    asx_runtime_config got;
    asx_runtime_hooks hooks;
    storage_block blk;
    const void *static_regions;
    const void *static_tasks;
    const void *static_obligations;
    asx_region_id region;
    asx_task_id task;
    asx_budget run;
    asx_outcome outcome;

    asx_runtime_reset();
    static_regions = (const void *)g_kernels[0]->regions;
    static_tasks = (const void *)g_kernels[0]->tasks;
    static_obligations = (const void *)g_kernels[0]->obligations;
    ASSERT_TRUE(storage_block_make(&blk, 0xA5u));
    make_valid_config(&cfg);
    make_valid_hooks(&hooks);
    cfg.storage = blk.block;
    cfg.storage_size = blk.size;
    MUST_OK(asx_runtime_init(&rt, &cfg, &hooks));
    ASSERT_TRUE(asx_runtime_is_initialized(&rt));
    ASSERT_TRUE(pointer_in_block(g_kernels[0]->regions, &blk));
    ASSERT_TRUE(pointer_in_block(g_kernels[0]->tasks, &blk));
    ASSERT_TRUE(pointer_in_block(g_kernels[0]->obligations, &blk));
    ASSERT_EQ(asx_runtime_get_config(&rt, &got), ASX_OK);
    ASSERT_TRUE(got.storage == (void *)blk.block);
    ASSERT_EQ(got.storage_size, blk.size);

    /* The runtime works in it: init zeroed the block, a region opens in
     * slot 0 and a task completes. */
    ASSERT_EQ(asx_region_open(&region), ASX_OK);
    ASSERT_TRUE(g_kernels[0]->regions[0].alive);
    ASSERT_EQ(asx_task_spawn(region, dummy_poll, NULL, &task), ASX_OK);
    ASSERT_TRUE(g_kernels[0]->tasks[0].alive);
    run = asx_budget_from_polls(8u);
    ASSERT_EQ(asx_scheduler_run(region, &run), ASX_OK);
    ASSERT_EQ(asx_task_get_outcome(task, &outcome), ASX_OK);
    ASSERT_EQ(asx_outcome_severity_of(&outcome), ASX_OUTCOME_OK);

    /* Shutdown ends the binding. */
    asx_runtime_shutdown(&rt);
    ASSERT_TRUE((const void *)g_kernels[0]->regions == static_regions);
    ASSERT_TRUE((const void *)g_kernels[0]->tasks == static_tasks);
    ASSERT_TRUE((const void *)g_kernels[0]->obligations == static_obligations);

    /* A runtime given no storage uses the static block again. */
    cfg.storage = NULL;
    cfg.storage_size = 0u;
    MUST_OK(asx_runtime_init(&rt, &cfg, &hooks));
    ASSERT_TRUE((const void *)g_kernels[0]->regions == static_regions);
    ASSERT_EQ(asx_region_open(&region), ASX_OK);
    asx_runtime_shutdown(&rt);
    storage_block_free(&blk);
}

/* K18: the registry names a kernel for the tag of a runtime that holds one
 * and no other; a runtime object holds the kernel of its tag exactly while
 * it is initialized, and a caller's block takes the library's place at
 * tag 0 until the runtime shuts down. */
TEST(kernel_registry_names_only_the_kernels_runtimes_hold) {
    asx_runtime rt;
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    storage_block blk;
    asx_region_id region;
    uint16_t tag;

    asx_runtime_reset();
    ASSERT_TRUE(g_kernels[0] == &g_kernel_static);
    ASSERT_TRUE(asx_kernel_for_tag(0u) == &g_kernel_static);
    ASSERT_EQ(g_kernel_static.tag, (uint16_t)0u);
    for (tag = 1u; tag < ASX_HANDLE_RT_TAG_LIMIT; tag++) {
        ASSERT_TRUE(asx_kernel_for_tag(tag) == NULL);
    }
    ASSERT_FALSE(asx_kernel_any_held());

    /* A handle is told apart by the tag it carries. */
    ASSERT_EQ(asx_region_open(&region), ASX_OK);
    ASSERT_TRUE(asx_kernel_of(region) == &g_kernel_static);
    ASSERT_TRUE(asx_kernel_of(asx_handle_with_runtime_tag(region, 1u)) == NULL);

    /* An initialized runtime holds the kernel of its tag, and only while it
     * is initialized. */
    ASSERT_TRUE(storage_block_make(&blk, 0xA5u));
    make_valid_config(&cfg);
    make_valid_hooks(&hooks);
    cfg.storage = blk.block;
    cfg.storage_size = blk.size;
    MUST_OK(asx_runtime_init(&rt, &cfg, &hooks));
    ASSERT_EQ(rt.tag, (uint16_t)0u);
    ASSERT_TRUE(g_kernels[0] == (asx_kernel *)(void *)blk.block);
    ASSERT_TRUE(asx_kernel_any_held());
    ASSERT_TRUE(asx_kernel_holds(rt.tag, rt.generation));
    ASSERT_FALSE(asx_kernel_holds(rt.tag, rt.generation + 1u));
    ASSERT_TRUE(asx_runtime_is_initialized(&rt));

    /* A second initialization is refused while the kernel is held. */
    {
        asx_runtime second;
        asx_runtime_config cfg2;

        make_valid_config(&cfg2);
        ASSERT_EQ(asx_runtime_init(&second, &cfg2, &hooks), ASX_E_INVALID_STATE);
    }

    asx_runtime_shutdown(&rt);
    ASSERT_TRUE(g_kernels[0] == &g_kernel_static);
    ASSERT_FALSE(asx_kernel_any_held());
    ASSERT_FALSE(asx_runtime_is_initialized(&rt));
    storage_block_free(&blk);
}

/* K18: a clear is per kernel. The kernel a query names is never "another
 * runtime's", and a kernel another runtime holds keeps its hold and the
 * process-wide subsystems it shares when this one is cleared. With one
 * registry slot there is no other kernel; the second slot is filled by
 * hand until initialization allocates tags. */
TEST(kernel_clear_leaves_another_held_kernel_and_the_process_state_alone) {
    asx_runtime rt;
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;

    asx_runtime_reset();
    ASSERT_FALSE(asx_kernel_other_held(NULL));
    ASSERT_FALSE(asx_kernel_other_held(g_kernels[0]));

    make_valid_config(&cfg);
    make_valid_hooks(&hooks);
    MUST_OK(asx_runtime_init(&rt, &cfg, &hooks));
    ASSERT_TRUE(asx_kernel_other_held(NULL));
    ASSERT_FALSE(asx_kernel_other_held(g_kernels[0]));
    ASSERT_EQ(asx_kernel_other_held(NULL), asx_kernel_any_held());

#if ASX_MAX_RUNTIMES >= 2u
    {
        storage_block blk2;
        asx_kernel *k2;
        asx_region_id region;

        ASSERT_TRUE(storage_block_make(&blk2, 0u));
        k2 = (asx_kernel *)(void *)blk2.block;
        k2->tag = 1u;
        k2->rt_generation = 9u;
        g_kernels[1] = k2;
        ASSERT_TRUE(asx_kernel_other_held(g_kernels[0]));
        ASSERT_TRUE(asx_kernel_other_held(k2));

        /* Clearing kernel 0 drops its own hold, arenas and trace, and leaves
         * the other kernel's hold and trace, and the process-wide event
         * log, alone. */
        ASSERT_EQ(asx_region_open(&region), ASX_OK);
        asx_trace_emit_internal(k2, ASX_TRACE_USER, 7u, 8u);
        ASSERT_TRUE(asx_trace_emitted_total() != 0u);
        ASSERT_TRUE(asx_event_log_count() != 0u);
        asx_runtime_clear_internal(g_kernels[0]);
        ASSERT_EQ(g_kernels[0]->rt_generation, 0u);
        ASSERT_EQ(g_kernels[0]->region_count, 0u);
        ASSERT_EQ(asx_trace_emitted_total(), (uint64_t)0);
        ASSERT_EQ(asx_trace_emitted_total_internal(k2), (uint64_t)1);
        ASSERT_EQ(k2->rt_generation, 9u);
        ASSERT_TRUE(asx_event_log_count() != 0u);

        /* With no other kernel held the clear is the last one's: the
         * process-wide subsystems are reset too, k2's own trace is not. */
        k2->rt_generation = 0u;
        ASSERT_FALSE(asx_kernel_other_held(g_kernels[0]));
        asx_runtime_clear_internal(g_kernels[0]);
        ASSERT_EQ(asx_event_log_count(), 0u);
        ASSERT_EQ(asx_trace_emitted_total_internal(k2), (uint64_t)1);
        g_kernels[1] = NULL;
        storage_block_free(&blk2);
    }
#endif
    asx_runtime_shutdown(&rt);
    asx_runtime_reset();
}

/* The default seeded entropy stream's step (hooks.c), as the channel-wake
 * test mirrors it: the stream every kernel draws, from the same seed. */
static uint64_t default_stream_next(uint64_t state) {
    return state * 6364136223846793005ULL + 1442695040888963407ULL;
}

/* K18: the default clock and entropy hooks answer from the kernel asked. A
 * kernel starts at time 0 and draws from the default seed, its clock and its
 * stream move only through it, and the public calls are kernel 0's. */
TEST(kernel_clock_and_entropy_belong_to_the_kernel) {
    asx_runtime rt;
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    asx_time now = 0;
    uint64_t draw = 0;
    uint64_t s1 = default_stream_next(0x5DEECE66DULL);
    uint64_t s2 = default_stream_next(s1);

    make_valid_config(&cfg);
    make_valid_hooks(&hooks);
    MUST_OK(asx_runtime_init(&rt, &cfg, &hooks));

    ASSERT_EQ(asx_kernel_virtual_now(g_kernels[0]), (asx_time)0);
    ASSERT_EQ(asx_kernel_random_u64(g_kernels[0], &draw), ASX_OK);
    ASSERT_EQ(draw, s1);
    ASSERT_EQ(asx_runtime_random_u64(&draw), ASX_OK);
    ASSERT_EQ(draw, s2);

    /* Advance never goes back; set does. The public clock is kernel 0's. */
    asx_kernel_virtual_advance(g_kernels[0], 500u);
    ASSERT_EQ(asx_runtime_virtual_now(), (asx_time)500);
    ASSERT_EQ(asx_kernel_now_ns(g_kernels[0], &now), ASX_OK);
    ASSERT_EQ(now, (asx_time)500);
    asx_kernel_virtual_advance(g_kernels[0], 100u);
    ASSERT_EQ(asx_kernel_virtual_now(g_kernels[0]), (asx_time)500);
    asx_kernel_virtual_set_internal(g_kernels[0], 100u);
    ASSERT_EQ(asx_runtime_virtual_now(), (asx_time)100);

    /* A reset restarts both. */
    asx_kernel_clock_reset_internal(g_kernels[0]);
    ASSERT_EQ(asx_kernel_virtual_now(g_kernels[0]), (asx_time)0);
    ASSERT_EQ(asx_kernel_random_u64(g_kernels[0], &draw), ASX_OK);
    ASSERT_EQ(draw, s1);

#if ASX_MAX_RUNTIMES >= 2u
    {
        storage_block blk2;
        asx_kernel *k2;

        ASSERT_TRUE(storage_block_make(&blk2, 0u));
        k2 = (asx_kernel *)(void *)blk2.block;
        k2->tag = 1u;
        k2->rt_generation = 9u;
        g_kernels[1] = k2;

        /* A zeroed kernel is at time 0 and at the seed, whatever kernel 0
         * has done; moving one clock does not move the other. */
        ASSERT_EQ(asx_kernel_virtual_now(k2), (asx_time)0);
        asx_kernel_virtual_advance(k2, 900u);
        ASSERT_EQ(asx_kernel_now_ns(k2, &now), ASX_OK);
        ASSERT_EQ(now, (asx_time)900);
        ASSERT_EQ(asx_kernel_now_ns(g_kernels[0], &now), ASX_OK);
        ASSERT_EQ(now, (asx_time)0);

        /* Interleaved draws: each kernel steps its own stream. */
        ASSERT_EQ(asx_kernel_random_u64(k2, &draw), ASX_OK);
        ASSERT_EQ(draw, s1);
        ASSERT_EQ(asx_kernel_random_u64(g_kernels[0], &draw), ASX_OK);
        ASSERT_EQ(draw, s2);
        ASSERT_EQ(asx_kernel_random_u64(k2, &draw), ASX_OK);
        ASSERT_EQ(draw, s2);

        g_kernels[1] = NULL;
        storage_block_free(&blk2);
    }
#endif
    asx_runtime_shutdown(&rt);
    asx_runtime_reset();
}

/* K18: a runtime's trace (ring, counters, digest) is its kernel's; the
 * public handle-less trace API is kernel 0's. A zeroed kernel's trace is a
 * reset one: empty, with the digest at the trace seed. */
TEST(kernel_trace_belongs_to_the_kernel) {
    const uint64_t seed = 0x517cc1b727220a95ULL; /* trace.c's digest seed */
    asx_runtime rt;
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    asx_trace_event e;
    uint64_t d0;

    make_valid_config(&cfg);
    make_valid_hooks(&hooks);
    MUST_OK(asx_runtime_init(&rt, &cfg, &hooks));
    asx_trace_reset_internal(g_kernels[0]);
    ASSERT_EQ(asx_trace_digest_internal(g_kernels[0]), seed);
    ASSERT_EQ(asx_trace_digest(), seed);

    asx_trace_emit_internal(g_kernels[0], ASX_TRACE_USER, 11u, 22u);
    asx_trace_emit(ASX_TRACE_USER, 33u, 44u); /* the public call: kernel 0 */
    ASSERT_EQ(asx_trace_emitted_total_internal(g_kernels[0]), (uint64_t)2);
    ASSERT_EQ(asx_trace_emitted_total(), (uint64_t)2);
    ASSERT_EQ(asx_trace_kind_total_internal(g_kernels[0], ASX_TRACE_USER), (uint64_t)2);
    ASSERT_EQ(asx_trace_event_get_internal(g_kernels[0], 1u, &e), 1);
    ASSERT_EQ(e.entity_id, (uint64_t)33);
    d0 = asx_trace_digest_internal(g_kernels[0]);
    ASSERT_TRUE(d0 != seed);

#if ASX_MAX_RUNTIMES >= 2u
    {
        storage_block blk2;
        asx_kernel *k2;

        ASSERT_TRUE(storage_block_make(&blk2, 0u));
        k2 = (asx_kernel *)(void *)blk2.block;
        k2->tag = 1u;
        k2->rt_generation = 9u;
        g_kernels[1] = k2;

        /* A zeroed kernel's trace is empty at the seed. */
        ASSERT_EQ(asx_trace_emitted_total_internal(k2), (uint64_t)0);
        ASSERT_EQ(asx_trace_digest_internal(k2), seed);

        /* Events land in the kernel named, and in no other. */
        asx_trace_emit_internal(k2, ASX_TRACE_USER, 11u, 22u);
        ASSERT_EQ(asx_trace_emitted_total_internal(k2), (uint64_t)1);
        ASSERT_EQ(asx_trace_emitted_total_internal(g_kernels[0]), (uint64_t)2);
        ASSERT_EQ(asx_trace_digest_internal(g_kernels[0]), d0);
        ASSERT_TRUE(asx_trace_digest_internal(k2) != seed);
        ASSERT_TRUE(asx_trace_digest_internal(k2) != d0);

        /* The same events in the same order give the same digest in any
         * kernel: the first event of kernel 0 and kernel 2's agree. */
        asx_trace_reset_internal(k2);
        asx_trace_reset_internal(g_kernels[0]);
        asx_trace_emit_internal(k2, ASX_TRACE_USER, 5u, 6u);
        asx_trace_emit_internal(g_kernels[0], ASX_TRACE_USER, 5u, 6u);
        ASSERT_EQ(asx_trace_digest_internal(k2), asx_trace_digest_internal(g_kernels[0]));

        /* Resetting one leaves the other. */
        asx_trace_reset_internal(k2);
        ASSERT_EQ(asx_trace_emitted_total_internal(k2), (uint64_t)0);
        ASSERT_EQ(asx_trace_emitted_total_internal(g_kernels[0]), (uint64_t)1);

        g_kernels[1] = NULL;
        storage_block_free(&blk2);
    }
#endif
    asx_runtime_shutdown(&rt);
    asx_runtime_reset();
}

/* K18: a poll run from inside another poll (a second runtime's scheduler
 * driven from a task) gives its caller back the current task and the
 * ledger's bound task; outside a poll both are unbound afterwards. */
typedef struct {
    asx_task_slot *inner_slot;
    asx_task_id inner_id;
    asx_task_id outer_current;
    asx_task_id outer_bound;
    asx_task_id inner_current;
    asx_task_id inner_bound;
    asx_task_id after_current;
    asx_task_id after_bound;
} nested_poll_ctx;

static asx_status nested_inner_poll(void *data, asx_task_id self) {
    nested_poll_ctx *c = (nested_poll_ctx *)data;
    (void)self;
    c->inner_current = asx_task_current();
    c->inner_bound = asx_error_ledger_bound_task();
    return ASX_OK;
}

static asx_status nested_outer_poll(void *data, asx_task_id self) {
    nested_poll_ctx *c = (nested_poll_ctx *)data;
    (void)self;
    c->outer_current = asx_task_current();
    c->outer_bound = asx_error_ledger_bound_task();
    (void)asx_task_poll_current_internal(c->inner_slot, c->inner_id);
    c->after_current = asx_task_current();
    c->after_bound = asx_error_ledger_bound_task();
    return ASX_OK;
}

TEST(nested_poll_restores_the_current_task_and_the_ledger_binding) {
    nested_poll_ctx c;
    asx_region_id region;
    asx_task_id outer;
    asx_task_id inner;
    asx_task_slot *outer_slot;

    memset(&c, 0, sizeof(c));
    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&region), ASX_OK);
    ASSERT_EQ(asx_task_spawn(region, nested_outer_poll, &c, &outer), ASX_OK);
    ASSERT_EQ(asx_task_spawn(region, nested_inner_poll, &c, &inner), ASX_OK);
    ASSERT_EQ(asx_task_slot_lookup(outer, &outer_slot), ASX_OK);
    ASSERT_EQ(asx_task_slot_lookup(inner, &c.inner_slot), ASX_OK);
    c.inner_id = inner;

    ASSERT_EQ(asx_task_current(), ASX_INVALID_ID);
    (void)asx_task_poll_current_internal(outer_slot, outer);

    ASSERT_EQ(c.outer_current, outer);
    ASSERT_EQ(c.outer_bound, outer);
    ASSERT_EQ(c.inner_current, inner);
    ASSERT_EQ(c.inner_bound, inner);
    ASSERT_EQ(c.after_current, outer);
    ASSERT_EQ(c.after_bound, outer);
    /* The outermost poll leaves both unbound, as a sweep poll always did. */
    ASSERT_EQ(asx_task_current(), ASX_INVALID_ID);
    ASSERT_EQ(asx_error_ledger_bound_task(), ASX_INVALID_ID);
    asx_runtime_reset();
}

TEST(runtime_storage_refusals_bind_nothing_and_change_no_byte) {
    asx_runtime rt;
    asx_runtime before;
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    storage_block blk;
    const void *static_regions;

    asx_runtime_reset();
    static_regions = (const void *)g_kernels[0]->regions;
    ASSERT_TRUE(storage_block_make(&blk, 0xA5u));
    make_valid_config(&cfg);
    make_valid_hooks(&hooks);
    memset(&rt, 0x5A, sizeof(rt));
    memcpy(&before, &rt, sizeof(rt));

    /* Too small by one byte. */
    cfg.storage = blk.block;
    cfg.storage_size = blk.size - 1u;
    ASSERT_EQ(asx_runtime_init(&rt, &cfg, &hooks), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_runtime_config_validate(&cfg), ASX_E_INVALID_ARGUMENT);

    /* A size without a block. */
    cfg.storage = NULL;
    cfg.storage_size = blk.size;
    ASSERT_EQ(asx_runtime_init(&rt, &cfg, &hooks), ASX_E_INVALID_ARGUMENT);

    /* Misaligned, where the alignment says anything. */
    if (asx_runtime_storage_align() > 1u) {
        cfg.storage = blk.block + 1;
        cfg.storage_size = blk.size;
        ASSERT_EQ(asx_runtime_init(&rt, &cfg, &hooks), ASX_E_INVALID_ARGUMENT);
    }

    /* The library's own block. */
    cfg.storage = (void *)g_kernels[0]->regions;
    cfg.storage_size = blk.size;
    ASSERT_EQ(asx_runtime_init(&rt, &cfg, &hooks), ASX_E_INVALID_ARGUMENT);

    /* Nothing was bound, reset or written. */
    ASSERT_TRUE(!asx_runtime_is_initialized(&rt));
    ASSERT_EQ(memcmp(&rt, &before, sizeof(rt)), 0);
    ASSERT_TRUE((const void *)g_kernels[0]->regions == static_regions);
    ASSERT_TRUE(block_is_filled(&blk, 0xA5u));
    storage_block_free(&blk);
}

TEST(refused_storage_init_preserves_the_live_runtime_and_the_block) {
    asx_runtime a;
    asx_runtime b;
    asx_runtime before_a;
    asx_runtime before_b;
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    storage_block blk;
    const void *static_regions;
    asx_region_id region;
    asx_task_id task;
    asx_budget run;
    asx_outcome outcome;

    asx_runtime_reset();
    make_valid_config(&cfg);
    make_valid_hooks(&hooks);
    MUST_OK(asx_runtime_init(&a, &cfg, &hooks));
    static_regions = (const void *)g_kernels[0]->regions;
    ASSERT_EQ(asx_region_open(&region), ASX_OK);
    ASSERT_EQ(asx_task_spawn(region, dummy_poll, NULL, &task), ASX_OK);

    ASSERT_TRUE(storage_block_make(&blk, 0xA5u));
    cfg.storage = blk.block;
    cfg.storage_size = blk.size;
    memset(&b, 0, sizeof(b));
    memcpy(&before_a, &a, sizeof(a));
    memcpy(&before_b, &b, sizeof(b));
    ASSERT_EQ(asx_runtime_init(&b, &cfg, &hooks), ASX_E_INVALID_STATE);

    /* The live runtime is untouched, still in its own block, and the
     * refused block was neither bound nor zeroed. */
    ASSERT_TRUE(asx_runtime_is_initialized(&a));
    ASSERT_EQ(memcmp(&a, &before_a, sizeof(a)), 0);
    ASSERT_EQ(memcmp(&b, &before_b, sizeof(b)), 0);
    ASSERT_TRUE((const void *)g_kernels[0]->regions == static_regions);
    ASSERT_TRUE(block_is_filled(&blk, 0xA5u));
    run = asx_budget_from_polls(8u);
    ASSERT_EQ(asx_scheduler_run(region, &run), ASX_OK);
    ASSERT_EQ(asx_task_get_outcome(task, &outcome), ASX_OK);
    ASSERT_EQ(asx_outcome_severity_of(&outcome), ASX_OUTCOME_OK);
    asx_runtime_shutdown(&a);
    storage_block_free(&blk);
}

TEST(runtimes_in_turn_in_their_own_blocks_do_not_share_arenas) {
    asx_runtime rt;
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    storage_block blk_a;
    storage_block blk_b;
    unsigned char *snapshot;
    asx_region_id ra;
    asx_region_id rb;
    asx_task_id ta;
    asx_task_id tb;
    asx_budget run;

    asx_runtime_reset();
    make_valid_config(&cfg);
    make_valid_hooks(&hooks);
    ASSERT_TRUE(storage_block_make(&blk_a, 0xA5u));
    ASSERT_TRUE(storage_block_make(&blk_b, 0xA5u));

    cfg.storage = blk_a.block;
    cfg.storage_size = blk_a.size;
    MUST_OK(asx_runtime_init(&rt, &cfg, &hooks));
    ASSERT_EQ(asx_region_open(&ra), ASX_OK);
    ASSERT_EQ(asx_task_spawn(ra, dummy_poll, NULL, &ta), ASX_OK);
    run = asx_budget_from_polls(8u);
    ASSERT_EQ(asx_scheduler_run(ra, &run), ASX_OK);
    asx_runtime_shutdown(&rt);
    snapshot = (unsigned char *)malloc(blk_a.size);
    ASSERT_TRUE(snapshot != NULL);
    memcpy(snapshot, blk_a.block, blk_a.size);

    /* The second runtime starts from a fresh arena of its own: the same
     * slots and generations come out, and nothing it does reaches the
     * first block. */
    cfg.storage = blk_b.block;
    cfg.storage_size = blk_b.size;
    MUST_OK(asx_runtime_init(&rt, &cfg, &hooks));
    ASSERT_EQ(asx_region_open(&rb), ASX_OK);
    ASSERT_EQ(rb, ra);
    ASSERT_EQ(asx_task_spawn(rb, dummy_poll, NULL, &tb), ASX_OK);
    ASSERT_EQ(tb, ta);
    run = asx_budget_from_polls(8u);
    ASSERT_EQ(asx_scheduler_run(rb, &run), ASX_OK);
    asx_runtime_shutdown(&rt);

    ASSERT_EQ(memcmp(snapshot, blk_a.block, blk_a.size), 0);
    ASSERT_TRUE(!block_is_filled(&blk_b, 0xA5u));
    free(snapshot);
    storage_block_free(&blk_a);
    storage_block_free(&blk_b);
}

TEST(stale_runtime_reports_uninitialized_after_new_init) {
    asx_runtime a, b;
    asx_runtime_config out;

    MUST_OK(asx_runtime_init_default(&a));
    ASSERT_TRUE(asx_runtime_is_initialized(&a));
    asx_runtime_reset(); /* Explicit teardown permits the new singleton. */
    MUST_OK(asx_runtime_init_default(&b));
    ASSERT_FALSE(asx_runtime_is_initialized(&a));
    ASSERT_TRUE(asx_runtime_is_initialized(&b));
    ASSERT_EQ(asx_runtime_get_config(&a, &out), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_runtime_get_config(&b, &out), ASX_OK);

    asx_runtime_shutdown(&a);
    ASSERT_TRUE(asx_runtime_is_initialized(&b));
    asx_runtime_shutdown(&b);
}

TEST(second_init_is_refused_while_a_runtime_is_live) {
    /* bd-9kll.3.20: the runtime state is process-wide, so a second
     * runtime's init would destroy the live one's regions and tasks. It is
     * refused and changes nothing; after the live one shuts down it
     * succeeds, and the old object is stale. */
    asx_runtime a, b;
    asx_runtime_config out;
    asx_region_id r;
    asx_region_state rs;

    MUST_OK(asx_runtime_init_default(&a));
    ASSERT_TRUE(asx_runtime_is_initialized(&a));
    ASSERT_EQ(asx_region_open(&r), ASX_OK);

    memset(&b, 0, sizeof(b));
    ASSERT_EQ(asx_runtime_init_default(&b), ASX_E_INVALID_STATE);
    ASSERT_FALSE(asx_runtime_is_initialized(&b));
    ASSERT_TRUE(asx_runtime_is_initialized(&a));
    ASSERT_EQ(asx_region_get_state(r, &rs), ASX_OK); /* a's region survived */
    ASSERT_EQ(rs, ASX_REGION_OPEN);
    ASSERT_EQ(asx_runtime_get_config(&a, &out), ASX_OK);

    asx_runtime_shutdown(&a);
    MUST_OK(asx_runtime_init_default(&b));
    ASSERT_TRUE(asx_runtime_is_initialized(&b));
    ASSERT_FALSE(asx_runtime_is_initialized(&a));
    ASSERT_EQ(asx_runtime_get_config(&a, &out), ASX_E_INVALID_STATE);
    asx_runtime_shutdown(&b);
}

TEST(explicit_shutdown_permits_runtime_restart) {
    /* Active self-init preserves live work; explicit teardown permits a
     * fresh generation with empty runtime state. */
    asx_runtime a;
    asx_region_id r;
    asx_region_state state;
    uint32_t gen;

    MUST_OK(asx_runtime_init_default(&a));
    gen = a.generation;
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_runtime_init_default(&a), ASX_E_INVALID_STATE);
    ASSERT_TRUE(asx_runtime_is_initialized(&a));
    ASSERT_EQ(a.generation, gen);
    ASSERT_EQ(asx_region_get_state(r, &state), ASX_OK);
    ASSERT_EQ(state, ASX_REGION_OPEN);
    asx_runtime_shutdown(&a);
    ASSERT_EQ(asx_runtime_init_default(&a), ASX_OK);
    ASSERT_TRUE(asx_runtime_is_initialized(&a));
    ASSERT_TRUE(a.generation > gen);
    ASSERT_EQ(asx_runtime_region_count(&a), 0u);
    asx_runtime_shutdown(&a);
}

TEST(runtime_reset_releases_the_live_runtime) {
    /* asx_runtime_reset destroys the runtime state and its hooks: the
     * runtime that was live is no longer initialized, and another may be. */
    asx_runtime a, b;

    MUST_OK(asx_runtime_init_default(&a));
    asx_runtime_reset();
    ASSERT_FALSE(asx_runtime_is_initialized(&a));
    memset(&b, 0, sizeof(b));
    ASSERT_EQ(asx_runtime_init_default(&b), ASX_OK);
    asx_runtime_shutdown(&b);
}

TEST(shutdown_clears_state) {
    asx_runtime rt;
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    make_valid_config(&cfg);
    make_valid_hooks(&hooks);

    MUST_OK(asx_runtime_init(&rt, &cfg, &hooks));
    ASSERT_TRUE(asx_runtime_is_initialized(&rt));
    asx_runtime_shutdown(&rt);
    ASSERT_FALSE(asx_runtime_is_initialized(&rt));
    ASSERT_EQ(rt.generation, 0u);
}

TEST(shutdown_null_safe) { asx_runtime_shutdown(NULL); }

TEST(shutdown_double_safe) {
    asx_runtime rt;
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    make_valid_config(&cfg);
    make_valid_hooks(&hooks);

    MUST_OK(asx_runtime_init(&rt, &cfg, &hooks));
    asx_runtime_shutdown(&rt);
    asx_runtime_shutdown(&rt); /* should not crash */
}

TEST(stale_shutdown_does_not_tear_down_active_runtime) {
    asx_runtime a, b;
    asx_runtime_config out;
    asx_runtime stale;

    MUST_OK(asx_runtime_init_default(&a));
    stale = a; /* a copy of the live object, stale once a shuts down */
    asx_runtime_shutdown(&a);
    MUST_OK(asx_runtime_init_default(&b));

    ASSERT_FALSE(asx_runtime_is_initialized(&stale));
    ASSERT_TRUE(asx_runtime_is_initialized(&b));

    asx_runtime_shutdown(&stale);

    ASSERT_TRUE(asx_runtime_is_initialized(&b));
    ASSERT_EQ(asx_runtime_get_config(&b, &out), ASX_OK);

    asx_runtime_shutdown(&b);
}

TEST(shutdown_uninitialized_safe) {
    asx_runtime rt;
    memset(&rt, 0, sizeof(rt));
    asx_runtime_shutdown(&rt); /* should not crash */
}

/* ------------------------------------------------------------------ */
/* Default initialization tests                                        */
/* ------------------------------------------------------------------ */

TEST(init_default_null_fails) { ASSERT_EQ(asx_runtime_init_default(NULL), ASX_E_INVALID_ARGUMENT); }

TEST(init_from_env_null_rt_fails) {
    clear_rt_test_env();
    ASSERT_EQ(asx_runtime_init_from_env(NULL, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(init_default_success) {
    asx_runtime rt;
    ASSERT_EQ(asx_runtime_init_default(&rt), ASX_OK);
    ASSERT_TRUE(asx_runtime_is_initialized(&rt));

    /* Verify defaults were applied */
    ASSERT_EQ(rt.config.io_backend, ASX_IO_BACKEND_GHOST);
    ASSERT_EQ(rt.config.leak_response, ASX_LEAK_PANIC);
    ASSERT_EQ(rt.config.finalizer_poll_budget, 100u);
    ASSERT_EQ(rt.config.max_cancel_chain_depth, 16u);

    asx_runtime_shutdown(&rt);
}

TEST(init_from_env_applies_default_prefix_overrides) {
    asx_runtime rt;
    asx_runtime_config cfg;

    clear_rt_test_env();
    rt_setenv("ASX_RUNTIME_PRESET", "high-throughput");
    rt_setenv("ASX_RUNTIME_WAIT_POLICY", "yield");
    rt_setenv("ASX_RUNTIME_IO_BACKEND", "ghost");
    rt_setenv("ASX_RUNTIME_FINALIZER_POLL_BUDGET", "77");

    ASSERT_EQ(asx_runtime_init_from_env(&rt, NULL), ASX_OK);
    ASSERT_EQ(asx_runtime_get_config(&rt, &cfg), ASX_OK);
    ASSERT_EQ(cfg.wait_policy, ASX_WAIT_YIELD);
    ASSERT_EQ(cfg.io_backend, ASX_IO_BACKEND_GHOST);
    ASSERT_EQ(cfg.finalizer_poll_budget, 77u);
    ASSERT_EQ(cfg.max_cancel_chain_depth, 32u);

    asx_runtime_shutdown(&rt);
    clear_rt_test_env();
}

TEST(init_from_env_applies_custom_prefix_overrides) {
    asx_runtime rt;
    asx_runtime_config cfg;

    clear_rt_test_env();
    rt_setenv("TESTRT_PRESET", "current-thread");
    rt_setenv("TESTRT_IO_BACKEND", "ghost");
    rt_setenv("TESTRT_FINALIZER_TIME_BUDGET_NS", "4444");
    rt_setenv("TESTRT_MAX_CANCEL_CHAIN_MEMORY", "12345");

    ASSERT_EQ(asx_runtime_init_from_env(&rt, "TESTRT_"), ASX_OK);
    ASSERT_EQ(asx_runtime_get_config(&rt, &cfg), ASX_OK);
    ASSERT_EQ(cfg.wait_policy, ASX_WAIT_BUSY_SPIN);
    ASSERT_EQ(cfg.io_backend, ASX_IO_BACKEND_GHOST);
    ASSERT_EQ(cfg.finalizer_time_budget_ns, (uint64_t)4444u);
    ASSERT_EQ(cfg.max_cancel_chain_memory, 12345u);

    asx_runtime_shutdown(&rt);
    clear_rt_test_env();
}

TEST(init_from_env_rejects_invalid_env_without_initializing) {
    asx_runtime rt;

    memset(&rt, 0, sizeof(rt));
    clear_rt_test_env();
    rt_setenv("ASX_RUNTIME_MAX_CANCEL_CHAIN_DEPTH", "0");

    ASSERT_EQ(asx_runtime_init_from_env(&rt, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_FALSE(asx_runtime_is_initialized(&rt));

    clear_rt_test_env();
}

TEST(init_from_env_rejects_unsupported_io_backend_without_initializing) {
    asx_runtime rt;

    memset(&rt, 0, sizeof(rt));
    clear_rt_test_env();
    rt_setenv("ASX_RUNTIME_IO_BACKEND", "io_uring");

    ASSERT_EQ(asx_runtime_init_from_env(&rt, NULL), ASX_E_PERMISSION_DENIED);
    ASSERT_FALSE(asx_runtime_is_initialized(&rt));

    clear_rt_test_env();
}

TEST(init_default_wires_blocking_surface) {
    asx_runtime rt;
#if ASX_HAS_BLOCKING_SURFACE
    asx_blocking_handle h;
    /* Static: in live builds a pool worker reads it on another thread,
     * possibly after this test has already returned on a failed assert. */
    static uint64_t input = 9;
    uint64_t result = 0;
    asx_status got;
    uint32_t spins = 0;
#endif

    ASSERT_EQ(asx_runtime_init_default(&rt), ASX_OK);

#if ASX_HAS_BLOCKING_SURFACE
    if (asx_surface_available_active(ASX_SURFACE_BLOCKING)) {
        ASSERT_EQ(asx_spawn_blocking(add_one, &input, NULL, &h), ASX_OK);
        /* CORE completes inline; live POSIX builds complete on a pool
         * worker, so wait (bounded) for the result. */
        do {
            got = asx_blocking_get_result(&h, &result);
        } while (got == ASX_E_PENDING && ++spins < 2000000000u);
        ASSERT_EQ(got, ASX_OK);
        ASSERT_EQ(result, (uint64_t)10);
    } else {
        ASSERT_EQ(asx_spawn_blocking(add_one, &input, NULL, &h), ASX_E_PERMISSION_DENIED);
    }
#else
    ASSERT_FALSE(asx_surface_available_active(ASX_SURFACE_BLOCKING));
    ASSERT_FALSE(asx_runtime_blocking_pool_initialized(&rt));
    ASSERT_EQ(asx_runtime_blocking_active_count(&rt), 0u);
#endif

    asx_runtime_shutdown(&rt);
}

TEST(init_default_wires_io_surface) {
    asx_runtime rt;
    asx_waker w;
#if ASX_HAS_NATIVE_IO_DRIVER
    asx_io_token tok;
#endif

    ASSERT_EQ(asx_runtime_init_default(&rt), ASX_OK);
    ASSERT_EQ(asx_waker_register(1, &w), ASX_OK);

#if ASX_HAS_NATIVE_IO_DRIVER
    if (asx_surface_available_active(ASX_SURFACE_IO_DRIVER)) {
        int fds[2];
        int fd = rt_io_fd_open(fds);
        ASSERT_TRUE(fd >= 0);
        ASSERT_EQ(asx_io_register(fd, ASX_IO_READABLE, &w, &tok), ASX_OK);
        asx_io_deregister(&tok);
        rt_io_fd_close(fds);
    } else {
        ASSERT_EQ(asx_io_register(42, ASX_IO_READABLE, &w, &tok), ASX_E_PERMISSION_DENIED);
    }
#else
    ASSERT_FALSE(asx_surface_available_active(ASX_SURFACE_IO_DRIVER));
    ASSERT_FALSE(asx_runtime_io_driver_initialized(&rt));
    ASSERT_EQ(asx_runtime_io_registration_count(&rt), 0u);
#endif

    asx_runtime_shutdown(&rt);
}

TEST(init_default_io_poll_wakes_registered_task) {
    asx_runtime rt;
    asx_runtime_hooks hooks;
    asx_waker w;
#if ASX_HAS_NATIVE_IO_DRIVER
    asx_io_token tok;
    asx_io_event event;
#endif

    ASSERT_EQ(asx_runtime_init_default(&rt), ASX_OK);
    ASSERT_EQ(asx_runtime_get_hooks_from(&rt, &hooks), ASX_OK);
    hooks.reactor.ghost_wait_fn = rt_fixed_ready_reactor;
    ASSERT_EQ(asx_runtime_set_hooks(&hooks), ASX_OK);
    g_rt_ready_count = 0u;

    ASSERT_EQ(asx_waker_register(7, &w), ASX_OK);

#if ASX_HAS_NATIVE_IO_DRIVER
    if (asx_surface_available_active(ASX_SURFACE_IO_DRIVER)) {
        int fds[2];
        int fd = rt_io_fd_open(fds);
        ASSERT_TRUE(fd >= 0);
        ASSERT_EQ(asx_io_register(fd, ASX_IO_READABLE, &w, &tok), ASX_OK);
        ASSERT_FALSE(asx_waker_is_signaled(&w));
        /* Readable for the ghost reactor (deterministic) and for real
         * (the native reactor of a live build). */
        g_rt_ready_count = 1u;
        rt_io_fd_make_readable(fds);
        ASSERT_EQ(asx_io_driver_poll(&event, 1u, 5u), 1u);
        ASSERT_EQ(event.token.slot, tok.slot);
        ASSERT_EQ(event.token.generation, tok.generation);
        ASSERT_EQ(event.ready, ASX_IO_READABLE);
        ASSERT_TRUE(asx_waker_is_signaled(&w));
        asx_io_deregister(&tok);
        rt_io_fd_close(fds);
    } else {
        ASSERT_EQ(asx_io_register(42, ASX_IO_READABLE, &w, &tok), ASX_E_PERMISSION_DENIED);
    }
#else
    ASSERT_FALSE(asx_surface_available_active(ASX_SURFACE_IO_DRIVER));
    ASSERT_FALSE(asx_runtime_io_driver_initialized(&rt));
    ASSERT_FALSE(asx_waker_is_signaled(&w));
#endif

    asx_runtime_shutdown(&rt);
}

/* ------------------------------------------------------------------ */
/* is_initialized tests                                                */
/* ------------------------------------------------------------------ */

TEST(is_initialized_null_returns_false) { ASSERT_FALSE(asx_runtime_is_initialized(NULL)); }

TEST(is_initialized_zeroed_returns_false) {
    asx_runtime rt;
    memset(&rt, 0, sizeof(rt));
    ASSERT_FALSE(asx_runtime_is_initialized(&rt));
}

/* ------------------------------------------------------------------ */
/* Config/hooks retrieval tests                                        */
/* ------------------------------------------------------------------ */

TEST(get_config_null_rt_fails) {
    asx_runtime_config out;
    ASSERT_EQ(asx_runtime_get_config(NULL, &out), ASX_E_INVALID_ARGUMENT);
}

TEST(get_config_null_out_fails) {
    asx_runtime rt;
    MUST_OK(asx_runtime_init_default(&rt));
    ASSERT_EQ(asx_runtime_get_config(&rt, NULL), ASX_E_INVALID_ARGUMENT);
    asx_runtime_shutdown(&rt);
}

TEST(get_config_uninitialized_fails) {
    asx_runtime rt;
    asx_runtime_config out;
    memset(&rt, 0, sizeof(rt));
    ASSERT_EQ(asx_runtime_get_config(&rt, &out), ASX_E_INVALID_STATE);
}

TEST(get_config_returns_stored_config) {
    asx_runtime rt;
    asx_runtime_config cfg, out;
    asx_runtime_hooks hooks;
    make_valid_config(&cfg);
    make_valid_hooks(&hooks);
    cfg.finalizer_poll_budget = 42;
    cfg.io_backend = ASX_IO_BACKEND_GHOST;

    MUST_OK(asx_runtime_init(&rt, &cfg, &hooks));
    ASSERT_EQ(asx_runtime_get_config(&rt, &out), ASX_OK);
    ASSERT_EQ(out.finalizer_poll_budget, 42u);
    ASSERT_EQ(out.io_backend, ASX_IO_BACKEND_GHOST);
    asx_runtime_shutdown(&rt);
}

TEST(runtime_io_backend_query_tracks_runtime_config) {
    asx_runtime rt;

    MUST_OK(asx_runtime_init_default(&rt));
    ASSERT_EQ(asx_runtime_io_backend(&rt), ASX_IO_BACKEND_GHOST);
    asx_runtime_shutdown(&rt);
    ASSERT_EQ(asx_runtime_io_backend(&rt), ASX_IO_BACKEND_GHOST);
}

TEST(init_copies_leak_escalation_config) {
    asx_runtime rt;
    asx_runtime_config cfg, out;
    asx_runtime_hooks hooks;
    asx_leak_escalation_config escalation;

    make_valid_config(&cfg);
    make_valid_hooks(&hooks);
    escalation.threshold = 7u;
    escalation.escalate_to = ASX_LEAK_RECOVER;
    cfg.leak_escalation = &escalation;

    MUST_OK(asx_runtime_init(&rt, &cfg, &hooks));

    escalation.threshold = 99u;
    escalation.escalate_to = ASX_LEAK_PANIC;

    ASSERT_EQ(asx_runtime_get_config(&rt, &out), ASX_OK);
    ASSERT_TRUE(out.leak_escalation != NULL);
    ASSERT_TRUE(out.leak_escalation != &escalation);
    ASSERT_EQ(out.leak_escalation->threshold, (uint64_t)7u);
    ASSERT_EQ(out.leak_escalation->escalate_to, ASX_LEAK_RECOVER);
    asx_runtime_shutdown(&rt);
}

TEST(get_hooks_null_rt_fails) {
    asx_runtime_hooks out;
    ASSERT_EQ(asx_runtime_get_hooks_from(NULL, &out), ASX_E_INVALID_ARGUMENT);
}

TEST(get_hooks_null_out_fails) {
    asx_runtime rt;
    MUST_OK(asx_runtime_init_default(&rt));
    ASSERT_EQ(asx_runtime_get_hooks_from(&rt, NULL), ASX_E_INVALID_ARGUMENT);
    asx_runtime_shutdown(&rt);
}

TEST(get_hooks_uninitialized_fails) {
    asx_runtime rt;
    asx_runtime_hooks out;
    memset(&rt, 0, sizeof(rt));
    ASSERT_EQ(asx_runtime_get_hooks_from(&rt, &out), ASX_E_INVALID_STATE);
}

TEST(validate_reload_uninitialized_fails) {
    asx_runtime rt;
    asx_runtime_config cfg;

    memset(&rt, 0, sizeof(rt));
    make_valid_config(&cfg);
    ASSERT_EQ(asx_runtime_validate_reload_config(&rt, &cfg, NULL), ASX_E_INVALID_STATE);
}

TEST(validate_reload_reports_restart_required_field) {
    asx_runtime rt;
    asx_runtime_config cfg, proposed;
    const char *rejection = NULL;

    MUST_OK(asx_runtime_init_default(&rt));
    ASSERT_EQ(asx_runtime_get_config(&rt, &cfg), ASX_OK);
    proposed = cfg;
    proposed.max_cancel_chain_depth = (uint16_t)(cfg.max_cancel_chain_depth + 1u);

    ASSERT_EQ(asx_runtime_validate_reload_config(&rt, &proposed, &rejection),
              ASX_E_CONFIG_RESTART_REQ);
    ASSERT_TRUE(rejection != NULL);
    ASSERT_STR_EQ(rejection, "max_cancel_chain_depth");
    asx_runtime_shutdown(&rt);
}

TEST(reload_config_updates_reloadable_fields) {
    asx_runtime rt;
    asx_runtime_config cfg, proposed, out;

    MUST_OK(asx_runtime_init_default(&rt));
    ASSERT_EQ(asx_runtime_get_config(&rt, &cfg), ASX_OK);
    proposed = cfg;
    proposed.wait_policy = ASX_WAIT_BUSY_SPIN;
    proposed.finalizer_poll_budget = 222u;
    proposed.finalizer_time_budget_ns = (uint64_t)2222222u;

    ASSERT_EQ(asx_runtime_reload_config(&rt, &proposed, NULL), ASX_OK);
    ASSERT_EQ(asx_runtime_get_config(&rt, &out), ASX_OK);
    ASSERT_EQ(out.wait_policy, ASX_WAIT_BUSY_SPIN);
    ASSERT_EQ(out.finalizer_poll_budget, 222u);
    ASSERT_EQ(out.finalizer_time_budget_ns, (uint64_t)2222222u);
    asx_runtime_shutdown(&rt);
}

TEST(reload_config_copies_leak_escalation_config) {
    asx_runtime rt;
    asx_runtime_config cfg, proposed, out;
    asx_leak_escalation_config escalation;

    MUST_OK(asx_runtime_init_default(&rt));
    ASSERT_EQ(asx_runtime_get_config(&rt, &cfg), ASX_OK);
    proposed = cfg;
    escalation.threshold = 11u;
    escalation.escalate_to = ASX_LEAK_SILENT;
    proposed.leak_escalation = &escalation;

    ASSERT_EQ(asx_runtime_reload_config(&rt, &proposed, NULL), ASX_OK);

    escalation.threshold = 22u;
    escalation.escalate_to = ASX_LEAK_PANIC;

    ASSERT_EQ(asx_runtime_get_config(&rt, &out), ASX_OK);
    ASSERT_TRUE(out.leak_escalation != NULL);
    ASSERT_TRUE(out.leak_escalation != &escalation);
    ASSERT_EQ(out.leak_escalation->threshold, (uint64_t)11u);
    ASSERT_EQ(out.leak_escalation->escalate_to, ASX_LEAK_SILENT);
    asx_runtime_shutdown(&rt);
}

TEST(reload_config_rejects_restart_required_change_without_mutation) {
    asx_runtime rt;
    asx_runtime_config cfg, proposed, out;
    const char *rejection = NULL;

    MUST_OK(asx_runtime_init_default(&rt));
    ASSERT_EQ(asx_runtime_get_config(&rt, &cfg), ASX_OK);
    proposed = cfg;
    proposed.wait_policy = ASX_WAIT_BUSY_SPIN;
    proposed.max_cancel_chain_memory = cfg.max_cancel_chain_memory + 512u;

    ASSERT_EQ(asx_runtime_reload_config(&rt, &proposed, &rejection), ASX_E_CONFIG_RESTART_REQ);
    ASSERT_TRUE(rejection != NULL);
    ASSERT_STR_EQ(rejection, "max_cancel_chain_memory");
    ASSERT_EQ(asx_runtime_get_config(&rt, &out), ASX_OK);
    ASSERT_EQ(out.wait_policy, cfg.wait_policy);
    ASSERT_EQ(out.max_cancel_chain_memory, cfg.max_cancel_chain_memory);
    asx_runtime_shutdown(&rt);
}

TEST(umbrella_exposes_runtime_support_surfaces) {
    asx_runtime_snapshot snap;
    asx_auto_deadline_tracker dt;
    asx_lab_config lab_cfg;
    asx_vtime_state vt;
    asx_adapter_decision adapter_decision;
    asx_hindsight_policy hindsight_policy;
    asx_perf_snapshot perf_snapshot;
    asx_evidence_sink evidence_sink;
    asx_oracle_suite oracle_suite;

    asx_runtime_snapshot_init(&snap);
    asx_auto_deadline_init(&dt);
    asx_lab_config_init(&lab_cfg);
    asx_vtime_init(&vt, 0u, 1000u);
    asx_event_log_reset();
    asx_hindsight_init();
    asx_telemetry_reset();
    asx_parallel_reset();
    memset(&adapter_decision, 0, sizeof(adapter_decision));
    memset(&hindsight_policy, 0, sizeof(hindsight_policy));
    memset(&perf_snapshot, 0, sizeof(perf_snapshot));
    asx_evidence_sink_init(&evidence_sink);
    asx_oracle_suite_init(&oracle_suite);

    ASSERT_EQ(snap.region_count, 0u);
    ASSERT_EQ(asx_auto_deadline_miss_rate(&dt), 0u);
    ASSERT_EQ(lab_cfg.max_polls, 1024u);
    ASSERT_EQ(asx_vtime_current(&vt), (asx_time)0u);
    ASSERT_EQ(dt.total_deadlines, 0u);
    ASSERT_EQ(asx_event_log_count(), 0u);
    ASSERT_EQ(asx_parallel_cancel_streak_limit(), 16u);
    ASSERT_EQ(asx_parallel_is_initialized(), 0);
    ASSERT_EQ(asx_telemetry_get_tier(), ASX_TELEMETRY_FORENSIC);
    ASSERT_EQ(asx_hindsight_readable_count(), 0u);
    ASSERT_STR_EQ(asx_adapter_domain_str(ASX_ADAPTER_DOMAIN_AUTOMOTIVE), "Automotive");
    ASSERT_STR_EQ(asx_event_kind_str(ASX_EVENT_TASK_SPAWN), "task_spawn");
    ASSERT_STR_EQ(asx_nd_event_kind_str(ASX_ND_IO_READY), "io_ready");
    ASSERT_STR_EQ(asx_telemetry_tier_str(ASX_TELEMETRY_ULTRA_MIN), "ultra_min");
    ASSERT_STR_EQ(asx_adapter_name(ASX_ADAPTER_HFT), "HFT_LATENCY");
    ASSERT_STR_EQ(asx_subsystem_name(ASX_SUBSYS_TIMER), "timer");
    ASSERT_EQ(adapter_decision.triggered, 0);
    ASSERT_EQ(evidence_sink.count, 0u);
    ASSERT_EQ(asx_evidence_verdict(&evidence_sink), ASX_EVIDENCE_PASS);
    ASSERT_EQ(oracle_suite.count, 0u);
    ASSERT_EQ(perf_snapshot.total_events, 0u);
    ASSERT_EQ(hindsight_policy.flush_on_invariant, 0);
    ASSERT_EQ(hindsight_policy.flush_on_divergence, 0);
}

/* ------------------------------------------------------------------ */
/* State query tests                                                   */
/* ------------------------------------------------------------------ */

TEST(region_count_null_returns_zero) { ASSERT_EQ(asx_runtime_region_count(NULL), 0u); }

TEST(task_count_null_returns_zero) { ASSERT_EQ(asx_runtime_task_count(NULL), 0u); }

TEST(obligation_count_null_returns_zero) { ASSERT_EQ(asx_runtime_obligation_count(NULL), 0u); }

TEST(io_registration_count_null_returns_zero) {
    ASSERT_EQ(asx_runtime_io_registration_count(NULL), 0u);
}

TEST(blocking_active_count_null_returns_zero) {
    ASSERT_EQ(asx_runtime_blocking_active_count(NULL), 0u);
}

TEST(io_driver_initialized_null_returns_false) {
    ASSERT_FALSE(asx_runtime_io_driver_initialized(NULL));
}

TEST(blocking_pool_initialized_null_returns_false) {
    ASSERT_FALSE(asx_runtime_blocking_pool_initialized(NULL));
}

TEST(runtime_is_quiescent_null_returns_false) { ASSERT_FALSE(asx_runtime_is_quiescent(NULL)); }

TEST(counts_zero_after_init) {
    asx_runtime rt;
    MUST_OK(asx_runtime_init_default(&rt));
    ASSERT_EQ(asx_runtime_region_count(&rt), 0u);
    ASSERT_EQ(asx_runtime_task_count(&rt), 0u);
    ASSERT_EQ(asx_runtime_obligation_count(&rt), 0u);
    ASSERT_TRUE(asx_runtime_is_quiescent(&rt));
    asx_runtime_shutdown(&rt);
}

TEST(counts_reflect_opened_region) {
    asx_runtime rt;
    asx_region_id rid;
    MUST_OK(asx_runtime_init_default(&rt));

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_runtime_region_count(&rt), 1u);

    asx_runtime_shutdown(&rt);
}

TEST(counts_reflect_spawned_task) {
    asx_runtime rt;
    asx_region_id rid;
    asx_task_id tid;
    MUST_OK(asx_runtime_init_default(&rt));

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, dummy_poll, NULL, &tid), ASX_OK);
    ASSERT_EQ(asx_runtime_task_count(&rt), 1u);

    asx_runtime_shutdown(&rt);
}

TEST(task_count_retains_completed_task_slot_until_reset) {
    asx_runtime rt;
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;

    MUST_OK(asx_runtime_init_default(&rt));
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, dummy_poll, NULL, &tid), ASX_OK);

    budget = asx_budget_from_polls(8);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);
    ASSERT_EQ(asx_runtime_task_count(&rt), 1u);

    asx_runtime_shutdown(&rt);
}

TEST(counts_uninitialized_returns_zero) {
    asx_runtime rt;
    memset(&rt, 0, sizeof(rt));
    ASSERT_EQ(asx_runtime_region_count(&rt), 0u);
    ASSERT_EQ(asx_runtime_task_count(&rt), 0u);
    ASSERT_EQ(asx_runtime_obligation_count(&rt), 0u);
    ASSERT_EQ(asx_runtime_io_registration_count(&rt), 0u);
    ASSERT_EQ(asx_runtime_blocking_active_count(&rt), 0u);
    ASSERT_FALSE(asx_runtime_io_driver_initialized(&rt));
    ASSERT_FALSE(asx_runtime_blocking_pool_initialized(&rt));
    ASSERT_FALSE(asx_runtime_is_quiescent(&rt));
}

/* Rust's RuntimeState::is_quiescent (bd-me9t): an idle Open region does
 * not block quiescence; a region still closing (Closing, Draining,
 * Finalizing) does. */
TEST(runtime_is_quiescent_true_with_idle_open_region) {
    asx_runtime rt;
    asx_region_id rid;

    MUST_OK(asx_runtime_init_default(&rt));
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_TRUE(asx_runtime_is_quiescent(&rt));
    asx_runtime_shutdown(&rt);
}

TEST(runtime_is_quiescent_false_with_closing_region) {
    static const asx_region_state closing[3] = {ASX_REGION_CLOSING, ASX_REGION_DRAINING,
                                                ASX_REGION_FINALIZING};
    asx_runtime rt;
    asx_region_id rid;
    asx_region_slot *region = NULL;
    uint32_t i;

    MUST_OK(asx_runtime_init_default(&rt));
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);
    for (i = 0; i < 3u; i++) {
        region->state = closing[i];
        ASSERT_FALSE(asx_runtime_is_quiescent(&rt));
    }
    region->state = ASX_REGION_CLOSED;
    ASSERT_TRUE(asx_runtime_is_quiescent(&rt));
    asx_runtime_shutdown(&rt);
}

/* Under lab dispatch a cancel request waits in the lab's command mailbox
 * until the next step (Rust's pending cancel dispatch): not quiescent
 * until it is applied. */
TEST(runtime_is_quiescent_false_with_pending_lab_cancel_command) {
    asx_runtime rt;
    asx_region_id rid;
    asx_cancel_reason reason;
    asx_budget budget;

    MUST_OK(asx_runtime_init_default(&rt));
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(7u), ASX_OK);
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_TRUE(asx_runtime_is_quiescent(&rt));
    memset(&reason, 0, sizeof(reason));
    reason.kind = ASX_CANCEL_SHUTDOWN;
    reason.origin_region = rid;
    reason.origin_task = ASX_INVALID_ID;
    ASSERT_EQ(asx_region_cancel_request(rid, &reason), ASX_OK);
    ASSERT_FALSE(asx_runtime_is_quiescent(&rt));
    budget = asx_budget_from_polls(8);
    MUST_OK(asx_scheduler_run(rid, &budget));
    ASSERT_TRUE(asx_runtime_is_quiescent(&rt));
    asx_runtime_shutdown(&rt);
}

TEST(runtime_is_quiescent_false_with_live_task) {
    asx_runtime rt;
    asx_region_id rid;
    asx_task_id tid;
    asx_region_slot *region = NULL;

    MUST_OK(asx_runtime_init_default(&rt));
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, dummy_poll, NULL, &tid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);
    region->state = ASX_REGION_CLOSED;
    ASSERT_FALSE(asx_runtime_is_quiescent(&rt));
    asx_runtime_shutdown(&rt);
}

TEST(runtime_is_quiescent_true_with_completed_task_in_closed_region) {
    asx_runtime rt;
    asx_region_id rid;
    asx_task_id tid;
    asx_region_slot *region = NULL;
    asx_budget budget;

    MUST_OK(asx_runtime_init_default(&rt));
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, dummy_poll, NULL, &tid), ASX_OK);

    budget = asx_budget_from_polls(8);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);
    region->state = ASX_REGION_CLOSED;

    ASSERT_TRUE(asx_runtime_is_quiescent(&rt));
    asx_runtime_shutdown(&rt);
}

TEST(runtime_is_quiescent_false_with_pending_obligation) {
    asx_runtime rt;
    asx_region_id rid;
    asx_task_id holder;
    asx_obligation_id oid;
    asx_region_slot *region = NULL;
    asx_task_slot *holder_slot = NULL;

    MUST_OK(asx_runtime_init_default(&rt));
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, dummy_poll, NULL, &holder), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve_ex(rid, ASX_OBLIGATION_KIND_GENERIC, holder, &oid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);
    region->state = ASX_REGION_CLOSED;
    /* A live holder alone would keep the runtime from quiescing. No public
     * sequence leaves an obligation reserved once its holder is terminal,
     * so the holder's slot is forced terminal to isolate the pending
     * obligation, as the region's state is forced above. */
    ASSERT_EQ(asx_task_slot_lookup(holder, &holder_slot), ASX_OK);
    holder_slot->state = ASX_TASK_COMPLETED;
    ASSERT_FALSE(asx_runtime_is_quiescent(&rt));
    asx_runtime_shutdown(&rt);
}

TEST(runtime_is_quiescent_false_with_pending_region_cleanup) {
    asx_runtime rt;
    asx_region_id rid;
    asx_region_slot *region = NULL;
    asx_cleanup_handle handle = ASX_CLEANUP_INVALID;
    int cleaned = 0;

    MUST_OK(asx_runtime_init_default(&rt));
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);
    region->state = ASX_REGION_CLOSED;
    ASSERT_EQ(asx_cleanup_push(&region->cleanup, cleanup_mark, &cleaned, &handle), ASX_OK);
    ASSERT_FALSE(asx_runtime_is_quiescent(&rt));
    ASSERT_EQ(cleaned, 0);
    asx_runtime_shutdown(&rt);
}

TEST(runtime_is_quiescent_false_with_active_io_registration) {
    asx_runtime rt;

    MUST_OK(asx_runtime_init_default(&rt));

#if ASX_HAS_NATIVE_IO_DRIVER
    if (asx_surface_available_active(ASX_SURFACE_IO_DRIVER)) {
        asx_waker w;
        asx_io_token tok;
        int fds[2];
        int fd = rt_io_fd_open(fds);

        ASSERT_TRUE(fd >= 0);
        ASSERT_TRUE(asx_runtime_is_quiescent(&rt));
        ASSERT_EQ(asx_waker_register(88, &w), ASX_OK);
        ASSERT_EQ(asx_io_register(fd, ASX_IO_READABLE, &w, &tok), ASX_OK);
        ASSERT_FALSE(asx_runtime_is_quiescent(&rt));
        asx_io_deregister(&tok);
        ASSERT_TRUE(asx_runtime_is_quiescent(&rt));
        rt_io_fd_close(fds);
    } else {
        ASSERT_TRUE(asx_runtime_is_quiescent(&rt));
    }
#else
    ASSERT_TRUE(asx_runtime_is_quiescent(&rt));
#endif

    asx_runtime_shutdown(&rt);
}

TEST(runtime_subsystem_queries_track_live_state) {
    asx_runtime rt;
#if ASX_HAS_NATIVE_IO_DRIVER
    asx_waker w;
    asx_io_token tok;
#endif
#if ASX_HAS_BLOCKING_SURFACE
    asx_blocking_handle blocking;
    uint64_t blocking_input = 5u;
#endif

    ASSERT_EQ(asx_runtime_init_default(&rt), ASX_OK);

#if ASX_HAS_NATIVE_IO_DRIVER
    if (asx_surface_available_active(ASX_SURFACE_IO_DRIVER)) {
        int fds[2];
        int fd = rt_io_fd_open(fds);
        ASSERT_TRUE(fd >= 0);
        ASSERT_TRUE(asx_runtime_io_driver_initialized(&rt));
        ASSERT_EQ(asx_runtime_io_registration_count(&rt), 0u);
        ASSERT_EQ(asx_waker_register(77, &w), ASX_OK);
        ASSERT_EQ(asx_io_register(fd, ASX_IO_READABLE, &w, &tok), ASX_OK);
        ASSERT_EQ(asx_runtime_io_registration_count(&rt), 1u);
        asx_io_deregister(&tok);
        ASSERT_EQ(asx_runtime_io_registration_count(&rt), 0u);
        rt_io_fd_close(fds);
    } else {
        ASSERT_FALSE(asx_runtime_io_driver_initialized(&rt));
        ASSERT_EQ(asx_runtime_io_registration_count(&rt), 0u);
    }
#else
    ASSERT_FALSE(asx_surface_available_active(ASX_SURFACE_IO_DRIVER));
    ASSERT_FALSE(asx_runtime_io_driver_initialized(&rt));
    ASSERT_EQ(asx_runtime_io_registration_count(&rt), 0u);
#endif

#if ASX_HAS_BLOCKING_SURFACE
    if (asx_surface_available_active(ASX_SURFACE_BLOCKING)) {
        uint64_t blocking_result = 0u;
        uint32_t spins = 0u;
        asx_status got;
        ASSERT_TRUE(asx_runtime_blocking_pool_initialized(&rt));
        ASSERT_EQ(asx_runtime_blocking_active_count(&rt), 0u);
        ASSERT_EQ(asx_spawn_blocking(add_one, &blocking_input, NULL, &blocking), ASX_OK);
        /* CORE completes inline; live POSIX builds complete on a pool
         * worker, so wait (bounded) for the result before counting. */
        do {
            got = asx_blocking_get_result(&blocking, &blocking_result);
        } while (got == ASX_E_PENDING && ++spins < 2000000000u);
        ASSERT_EQ(got, ASX_OK);
        ASSERT_EQ(blocking_result, (uint64_t)6);
        ASSERT_EQ(asx_runtime_blocking_active_count(&rt), 0u);
    } else {
        ASSERT_FALSE(asx_runtime_blocking_pool_initialized(&rt));
        ASSERT_EQ(asx_runtime_blocking_active_count(&rt), 0u);
    }
#else
    ASSERT_FALSE(asx_surface_available_active(ASX_SURFACE_BLOCKING));
    ASSERT_FALSE(asx_runtime_blocking_pool_initialized(&rt));
    ASSERT_EQ(asx_runtime_blocking_active_count(&rt), 0u);
#endif

    asx_runtime_shutdown(&rt);
    ASSERT_FALSE(asx_runtime_io_driver_initialized(&rt));
    ASSERT_FALSE(asx_runtime_blocking_pool_initialized(&rt));
    ASSERT_EQ(asx_runtime_io_registration_count(&rt), 0u);
    ASSERT_EQ(asx_runtime_blocking_active_count(&rt), 0u);
}

/* ------------------------------------------------------------------ */
/* Capacity query tests                                                */
/* ------------------------------------------------------------------ */

TEST(region_capacity_returns_max) {
    ASSERT_EQ(asx_runtime_region_capacity(), (uint32_t)ASX_MAX_REGIONS);
}

TEST(task_capacity_returns_max) { ASSERT_EQ(asx_runtime_task_capacity(), (uint32_t)ASX_MAX_TASKS); }

TEST(obligation_capacity_returns_max) {
    ASSERT_EQ(asx_runtime_obligation_capacity(), (uint32_t)ASX_MAX_OBLIGATIONS);
}

/* ------------------------------------------------------------------ */
/* Safety/containment query tests                                      */
/* ------------------------------------------------------------------ */

TEST(safety_profile_returns_active) {
    asx_runtime rt;
    asx_safety_profile sp;
    MUST_OK(asx_runtime_init_default(&rt));
    sp = asx_runtime_safety_profile(&rt);
    ASSERT_EQ(sp, asx_safety_profile_active());
    asx_runtime_shutdown(&rt);
}

TEST(containment_policy_returns_active) {
    asx_runtime rt;
    asx_containment_policy cp;
    MUST_OK(asx_runtime_init_default(&rt));
    cp = asx_runtime_containment_policy(&rt);
    ASSERT_EQ(cp, asx_containment_policy_active());
    asx_runtime_shutdown(&rt);
}

TEST(umbrella_runtime_lab_deadline_roundtrip) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_lab_scenario scenario;
    asx_lab_result result;
    asx_auto_deadline_tracker *deadline;
    asx_auto_audit_ring *audit;
    asx_region_id rid = ASX_INVALID_ID;

    asx_auto_instrument_reset();
    deadline = asx_auto_deadline_global();
    audit = asx_auto_audit_global();
    ASSERT_TRUE(deadline != NULL);
    ASSERT_TRUE(audit != NULL);
    ASSERT_EQ(asx_auto_audit_count(audit), 0u);

    asx_auto_record_deadline(1000u, 800u, 1u);
    asx_auto_record_deadline(1000u, 1200u, 2u);
    ASSERT_EQ(deadline->total_deadlines, 2u);
    ASSERT_EQ(deadline->deadline_hits, 1u);
    ASSERT_EQ(deadline->deadline_misses, 1u);
    ASSERT_EQ(asx_auto_deadline_miss_rate(deadline), 5000u);
    ASSERT_EQ(asx_auto_audit_count(audit), 1u);
    ASSERT_STR_EQ(asx_audit_kind_str(ASX_AUDIT_DEADLINE_MISS), "DEADLINE_MISS");

    asx_lab_config_init(&cfg);
    cfg.seed = 42u;
    MUST_OK(asx_lab_init(&lab, &cfg));
    ASSERT_EQ(asx_runtime_region_count(&lab.rt), 0u);
    ASSERT_EQ(asx_runtime_task_count(&lab.rt), 0u);
    ASSERT_EQ(asx_lab_now(&lab), (asx_time)0u);

    asx_lab_scenario_init(&scenario, "runtime-lab-deadline-roundtrip");
    MUST_OK(asx_lab_scenario_add_step(&scenario, lab_noop_step, &rid));
    asx_lab_advance_time(&lab, 300u);
    MUST_OK(asx_lab_run_scenario(&lab, &scenario, &result));

    ASSERT_TRUE(rid != ASX_INVALID_ID);
    ASSERT_EQ(result.steps_completed, 1u);
    ASSERT_EQ(result.steps_total, 1u);
    ASSERT_EQ(result.last_status, ASX_OK);
    ASSERT_EQ(result.elapsed_ns, (asx_time)0u);
    ASSERT_EQ(asx_lab_now(&lab), (asx_time)300u);
    ASSERT_EQ(asx_runtime_region_count(&lab.rt), 1u);
    ASSERT_EQ(asx_runtime_safety_profile(&lab.rt), asx_safety_profile_active());
    ASSERT_EQ(asx_runtime_containment_policy(&lab.rt), asx_containment_policy_active());

    asx_lab_shutdown(&lab);
    asx_auto_instrument_reset();
}

/* ------------------------------------------------------------------ */
/* Integration: init → use → shutdown round-trip                       */
/* ------------------------------------------------------------------ */

typedef struct {
    const asx_runtime *rt;
    asx_region_id region;
} roundtrip_ctx;

/* Reserve and commit an obligation, as a task's own poll. */
static void roundtrip_reserve_commit(void *data) {
    roundtrip_ctx *ctx = (roundtrip_ctx *)data;
    asx_obligation_id oid;

    ASSERT_EQ(asx_obligation_reserve(ctx->region, &oid), ASX_OK);
    ASSERT_EQ(asx_runtime_obligation_count(ctx->rt), 1u);
    ASSERT_EQ(asx_obligation_commit(oid), ASX_OK);
}

TEST(full_roundtrip) {
    asx_runtime rt;
    asx_region_id rid;
    roundtrip_ctx ctx;
    asx_runtime_config cfg_out;

    ASSERT_EQ(asx_runtime_init_default(&rt), ASX_OK);
    ASSERT_TRUE(asx_runtime_is_initialized(&rt));

    /* Open region */
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_runtime_region_count(&rt), 1u);

    /* Reserve and commit obligation */
    ctx.rt = &rt;
    ctx.region = rid;
    ASSERT_EQ(test_run_in_task(rid, roundtrip_reserve_commit, &ctx), ASX_OK);

    /* Query config */
    ASSERT_EQ(asx_runtime_get_config(&rt, &cfg_out), ASX_OK);
    ASSERT_EQ(cfg_out.finalizer_poll_budget, 100u);

    /* Shutdown resets everything */
    asx_runtime_shutdown(&rt);
    ASSERT_FALSE(asx_runtime_is_initialized(&rt));
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    RUN_TEST(conflicting_init_preserves_live_runtime_and_destination);
    RUN_TEST(runtime_storage_size_and_alignment_are_published);
    RUN_TEST(runtime_arenas_live_in_the_callers_storage_and_return_to_the_static_block);
    RUN_TEST(kernel_registry_names_only_the_kernels_runtimes_hold);
    RUN_TEST(kernel_clear_leaves_another_held_kernel_and_the_process_state_alone);
    RUN_TEST(kernel_clock_and_entropy_belong_to_the_kernel);
    RUN_TEST(kernel_trace_belongs_to_the_kernel);
    RUN_TEST(nested_poll_restores_the_current_task_and_the_ledger_binding);
    RUN_TEST(runtime_storage_refusals_bind_nothing_and_change_no_byte);
    RUN_TEST_IF(ASX_MAX_RUNTIMES == 1u,
                refused_storage_init_preserves_the_live_runtime_and_the_block,
                "with room for more runtimes the block is bound as the next tag");
    RUN_TEST(runtimes_in_turn_in_their_own_blocks_do_not_share_arenas);
    fprintf(stderr, "=== test_rt ===\n");

    /* Config validation */
    RUN_TEST(config_validate_null_fails);
    RUN_TEST(config_validate_bad_size_fails);
    RUN_TEST(config_validate_zero_cancel_depth_fails);
    RUN_TEST(config_validate_zero_finalizer_budget_fails);
    RUN_TEST(config_validate_invalid_wait_policy_fails);
    RUN_TEST(config_validate_invalid_io_backend_fails);
    RUN_TEST(config_validate_invalid_leak_response_fails);
    RUN_TEST(config_validate_invalid_finalizer_escalation_fails);
    RUN_TEST(config_validate_invalid_leak_escalation_fails);
    RUN_TEST(config_validate_defaults_ok);

    /* Init/shutdown lifecycle */
    RUN_TEST(init_null_rt_fails);
    RUN_TEST(mismatched_creation_preserves_live_work_and_all_objects);
    RUN_TEST(init_null_config_fails);
    RUN_TEST(init_null_hooks_fails);
    RUN_TEST(init_bad_config_fails);
    RUN_TEST(init_invalid_enum_config_fails);
    RUN_TEST(init_unsupported_io_backend_fails_closed);
    RUN_TEST(init_invalid_leak_escalation_config_fails);
    RUN_TEST(init_success);
    RUN_TEST(init_generation_increments);
    RUN_TEST(second_init_is_refused_while_a_runtime_is_live);
    RUN_TEST(stale_runtime_reports_uninitialized_after_new_init);
    RUN_TEST(explicit_shutdown_permits_runtime_restart);
    RUN_TEST(runtime_reset_releases_the_live_runtime);
    RUN_TEST(shutdown_clears_state);
    RUN_TEST(shutdown_null_safe);
    RUN_TEST(shutdown_double_safe);
    RUN_TEST(stale_shutdown_does_not_tear_down_active_runtime);
    RUN_TEST(shutdown_uninitialized_safe);

    /* Default init */
    RUN_TEST(init_default_null_fails);
    RUN_TEST(init_from_env_null_rt_fails);
    RUN_TEST(init_default_success);
    RUN_TEST(init_from_env_applies_default_prefix_overrides);
    RUN_TEST(init_from_env_applies_custom_prefix_overrides);
    RUN_TEST(init_from_env_rejects_invalid_env_without_initializing);
    RUN_TEST(init_from_env_rejects_unsupported_io_backend_without_initializing);
    RUN_TEST(init_default_wires_blocking_surface);
    RUN_TEST(init_default_wires_io_surface);
    RUN_TEST(init_default_io_poll_wakes_registered_task);

    /* is_initialized */
    RUN_TEST(is_initialized_null_returns_false);
    RUN_TEST(is_initialized_zeroed_returns_false);

    /* Config/hooks retrieval */
    RUN_TEST(get_config_null_rt_fails);
    RUN_TEST(get_config_null_out_fails);
    RUN_TEST(get_config_uninitialized_fails);
    RUN_TEST(get_config_returns_stored_config);
    RUN_TEST(runtime_io_backend_query_tracks_runtime_config);
    RUN_TEST(init_copies_leak_escalation_config);
    RUN_TEST(get_hooks_null_rt_fails);
    RUN_TEST(get_hooks_null_out_fails);
    RUN_TEST(get_hooks_uninitialized_fails);
    RUN_TEST(validate_reload_uninitialized_fails);
    RUN_TEST(validate_reload_reports_restart_required_field);
    RUN_TEST(reload_config_updates_reloadable_fields);
    RUN_TEST(reload_config_copies_leak_escalation_config);
    RUN_TEST(reload_config_rejects_restart_required_change_without_mutation);
    RUN_TEST(umbrella_exposes_runtime_support_surfaces);

    /* State queries */
    RUN_TEST(region_count_null_returns_zero);
    RUN_TEST(task_count_null_returns_zero);
    RUN_TEST(obligation_count_null_returns_zero);
    RUN_TEST(io_registration_count_null_returns_zero);
    RUN_TEST(blocking_active_count_null_returns_zero);
    RUN_TEST(io_driver_initialized_null_returns_false);
    RUN_TEST(blocking_pool_initialized_null_returns_false);
    RUN_TEST(runtime_is_quiescent_null_returns_false);
    RUN_TEST(counts_zero_after_init);
    RUN_TEST(counts_reflect_opened_region);
    RUN_TEST(counts_reflect_spawned_task);
    RUN_TEST(task_count_retains_completed_task_slot_until_reset);
    RUN_TEST(counts_uninitialized_returns_zero);
    RUN_TEST(runtime_is_quiescent_true_with_idle_open_region);
    RUN_TEST(runtime_is_quiescent_false_with_closing_region);
    RUN_TEST(runtime_is_quiescent_false_with_pending_lab_cancel_command);
    RUN_TEST(runtime_is_quiescent_false_with_live_task);
    RUN_TEST(runtime_is_quiescent_true_with_completed_task_in_closed_region);
    RUN_TEST(runtime_is_quiescent_false_with_pending_obligation);
    RUN_TEST(runtime_is_quiescent_false_with_pending_region_cleanup);
    RUN_TEST(runtime_is_quiescent_false_with_active_io_registration);
    RUN_TEST(runtime_subsystem_queries_track_live_state);

    /* Capacity queries */
    RUN_TEST(region_capacity_returns_max);
    RUN_TEST(task_capacity_returns_max);
    RUN_TEST(obligation_capacity_returns_max);

    /* Safety/containment */
    RUN_TEST(safety_profile_returns_active);
    RUN_TEST(containment_policy_returns_active);
    RUN_TEST(umbrella_runtime_lab_deadline_roundtrip);

    /* Integration */
    RUN_TEST(full_roundtrip);

    TEST_REPORT();
    return test_failures;
}
