/*
 * consumer_shim.c — ABI/API stability consumer shim test (bd-56t.4)
 *
 * Compiles against public headers only. Verifies that:
 * 1. All public types are accessible and have expected sizes
 * 2. ABI version macros are defined and consistent
 * 3. Key enum values have their frozen assignments
 * 4. Config-struct size-field pattern works
 * 5. Function symbols resolve at link time
 *
 * This test MUST NOT include any private/internal headers.
 * It simulates an external consumer linking against libasx.
 *
 * Build: cc -std=c99 -I include -o consumer_shim tests/abi/consumer_shim.c -L build/lib -lasx
 * Run:   ./consumer_shim
 *
 * SPDX-License-Identifier: MIT
 */

/* ASX_CHECKPOINT_WAIVER_FILE("Consumer shim — no runtime loops") */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Only public headers — simulates an external consumer */
#include <asx/asx.h>

/* Mismatch consumers may expose more headers than the linked library.
 * Test only factories public in both builds; unavailable library surfaces
 * cannot be linked and are not runtime comparisons. */
#ifndef ASX_ABI_LIBRARY_HAS_APP
#define ASX_ABI_LIBRARY_HAS_APP ASX_HAS_NATIVE_RUNTIME_SURFACES
#endif
#ifndef ASX_ABI_LIBRARY_HAS_LAB
#define ASX_ABI_LIBRARY_HAS_LAB ASX_HAS_BROWSER_TRACE
#endif

asx_status asx_abi_host_start(void);
asx_status asx_abi_host_finish(void);

#ifdef ASX_ABI_MATCHED_HOST

/* This translation unit is compiled with the library's options, while the
 * consumer below is compiled independently with a deliberate mismatch. */
static asx_runtime host_runtime;
static asx_region_id host_region;
static asx_task_id host_task;
static asx_obligation_id host_obligation;
static unsigned int host_polls;

static asx_status host_poll(void *user_data, asx_task_id self) {
    (void)user_data;
    (void)self;
    host_polls++;
    return asx_obligation_abort(host_obligation);
}

asx_status asx_abi_host_start(void) {
    asx_status st;
    host_polls = 0u;
    st = asx_runtime_init_default(&host_runtime);
    if (st != ASX_OK) return st;
    st = asx_region_open(&host_region);
    if (st != ASX_OK) return st;
    st = asx_task_spawn(host_region, host_poll, NULL, &host_task);
    if (st != ASX_OK) return st;
    return asx_obligation_reserve(host_region, &host_obligation);
}

asx_status asx_abi_host_finish(void) {
    asx_task_state task_state;
    asx_obligation_state obligation_state;
    asx_budget budget;
    asx_status st;

    if (!asx_runtime_is_initialized(&host_runtime) || host_polls != 0u) return ASX_E_INVALID_STATE;
    st = asx_task_get_state(host_task, &task_state);
    if (st != ASX_OK || task_state != ASX_TASK_CREATED) return ASX_E_INVALID_STATE;
    st = asx_obligation_get_state(host_obligation, &obligation_state);
    if (st != ASX_OK || obligation_state != ASX_OBLIGATION_RESERVED) return ASX_E_INVALID_STATE;
    budget = asx_budget_from_polls(64u);
    st = asx_scheduler_run(host_region, &budget);
    if (st != ASX_OK || host_polls != 1u) return ASX_E_INVALID_STATE;
    st = asx_task_get_state(host_task, &task_state);
    if (st != ASX_OK || task_state != ASX_TASK_COMPLETED) return ASX_E_INVALID_STATE;
    st = asx_obligation_get_state(host_obligation, &obligation_state);
    if (st != ASX_OK || obligation_state != ASX_OBLIGATION_ABORTED) return ASX_E_INVALID_STATE;
    st = asx_region_close(host_region);
    if (st != ASX_OK) return st;
    budget = asx_budget_from_polls(64u);
    st = asx_region_drain(host_region, &budget);
    if (st != ASX_OK || !asx_runtime_is_quiescent(&host_runtime)) return ASX_E_INVALID_STATE;
    asx_runtime_shutdown(&host_runtime);
    return ASX_OK;
}

#else

static int failures = 0;
static int passes = 0;

#define SHIM_CHECK(cond, msg)                                                                      \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "  FAIL: %s\n", msg);                                                  \
            failures++;                                                                            \
        } else {                                                                                   \
            passes++;                                                                              \
        }                                                                                          \
    } while (0)

/* --- ABI version checks --- */

static void test_abi_version(void) {
    fprintf(stderr, "--- ABI version ---\n");

    /* Compile-time version macros exist */
    SHIM_CHECK(ASX_ABI_VERSION_MAJOR >= 1, "ABI major >= 1");
    SHIM_CHECK(ASX_ABI_VERSION_MINOR >= 0, "ABI minor >= 0");
    SHIM_CHECK(ASX_ABI_VERSION_PATCH >= 0, "ABI patch >= 0");

    /* Composite version */
    SHIM_CHECK(ASX_ABI_VERSION >= 1000000, "ABI composite version >= 1.0.0");

    /* Runtime version matches compile-time */
    SHIM_CHECK(asx_abi_version_major() == ASX_ABI_VERSION_MAJOR,
               "runtime ABI major matches header");
    SHIM_CHECK(asx_abi_version_minor() == ASX_ABI_VERSION_MINOR,
               "runtime ABI minor matches header");
    SHIM_CHECK(asx_abi_version_patch() == ASX_ABI_VERSION_PATCH,
               "runtime ABI patch matches header");

    /* API version macros exist */
    SHIM_CHECK(ASX_API_VERSION_MAJOR >= 0, "API major defined");
    SHIM_CHECK(ASX_API_VERSION_MINOR >= 0, "API minor defined");
    SHIM_CHECK(ASX_API_VERSION_PATCH >= 0, "API patch defined");

    /* ABI check macro compiles */
    ASX_ABI_CHECK(2);
}

/* --- Handle type sizes --- */

static void test_handle_sizes(void) {
    fprintf(stderr, "--- Handle type sizes ---\n");

    SHIM_CHECK(sizeof(asx_region_id) == ASX_ABI_SIZEOF_REGION_ID, "asx_region_id size matches ABI");
    SHIM_CHECK(sizeof(asx_task_id) == ASX_ABI_SIZEOF_TASK_ID, "asx_task_id size matches ABI");
    SHIM_CHECK(sizeof(asx_obligation_id) == ASX_ABI_SIZEOF_OBLIGATION_ID,
               "asx_obligation_id size matches ABI");
    SHIM_CHECK(sizeof(asx_timer_id) == ASX_ABI_SIZEOF_TIMER_ID, "asx_timer_id size matches ABI");
    SHIM_CHECK(sizeof(asx_channel_id) == ASX_ABI_SIZEOF_CHANNEL_ID,
               "asx_channel_id size matches ABI");
    SHIM_CHECK(sizeof(asx_status) == ASX_ABI_SIZEOF_STATUS, "asx_status size matches ABI");
    SHIM_CHECK(sizeof(asx_time) == ASX_ABI_SIZEOF_TIME, "asx_time size matches ABI");
}

/* --- Frozen enum values --- */

static void test_frozen_enums(void) {
    fprintf(stderr, "--- Frozen enum values ---\n");

    /* Region states */
    SHIM_CHECK(ASX_REGION_OPEN == 0, "ASX_REGION_OPEN == 0");
    SHIM_CHECK(ASX_REGION_CLOSED == 4, "ASX_REGION_CLOSED == 4");

    /* Task states */
    SHIM_CHECK(ASX_TASK_CREATED == 0, "ASX_TASK_CREATED == 0");
    SHIM_CHECK(ASX_TASK_COMPLETED == 5, "ASX_TASK_COMPLETED == 5");

    /* Outcome severity */
    SHIM_CHECK(ASX_OUTCOME_OK == 0, "ASX_OUTCOME_OK == 0");
    SHIM_CHECK(ASX_OUTCOME_PANICKED == 3, "ASX_OUTCOME_PANICKED == 3");

    /* Cancel kinds */
    SHIM_CHECK(ASX_CANCEL_USER == 0, "ASX_CANCEL_USER == 0");

    /* Cancel phases */
    SHIM_CHECK(ASX_CANCEL_PHASE_REQUESTED == 0, "ASX_CANCEL_PHASE_REQUESTED == 0");
    SHIM_CHECK(ASX_CANCEL_PHASE_COMPLETED == 3, "ASX_CANCEL_PHASE_COMPLETED == 3");

    /* Status codes — family structure */
    SHIM_CHECK(ASX_OK == 0, "ASX_OK == 0");
    SHIM_CHECK(ASX_E_PENDING == 1, "ASX_E_PENDING == 1");
    SHIM_CHECK(ASX_E_INVALID_ARGUMENT == 100, "ASX_E_INVALID_ARGUMENT == 100");
    SHIM_CHECK(ASX_E_INVALID_TRANSITION == 200, "ASX_E_INVALID_TRANSITION == 200");
    SHIM_CHECK(ASX_E_REGION_NOT_FOUND == 300, "ASX_E_REGION_NOT_FOUND == 300");
    SHIM_CHECK(ASX_E_TASK_NOT_FOUND == 400, "ASX_E_TASK_NOT_FOUND == 400");
    SHIM_CHECK(ASX_E_CANCELLED == 600, "ASX_E_CANCELLED == 600");
    SHIM_CHECK(ASX_E_DISCONNECTED == 700, "ASX_E_DISCONNECTED == 700");
    SHIM_CHECK(ASX_E_TIMER_NOT_FOUND == 800, "ASX_E_TIMER_NOT_FOUND == 800");
    SHIM_CHECK(ASX_E_RESOURCE_EXHAUSTED == 1000, "ASX_E_RESOURCE_EXHAUSTED == 1000");
    SHIM_CHECK(ASX_E_STALE_HANDLE == 1100, "ASX_E_STALE_HANDLE == 1100");
    SHIM_CHECK(ASX_E_HOOK_MISSING == 1200, "ASX_E_HOOK_MISSING == 1200");

    /* Invalid handle sentinel */
    SHIM_CHECK(ASX_INVALID_ID == 0, "ASX_INVALID_ID == 0");
}

/* --- Config-struct size-field pattern --- */

static void test_config_pattern(void) {
    asx_runtime_config cfg;

    fprintf(stderr, "--- Config size-field pattern ---\n");

    memset(&cfg, 0, sizeof(cfg));
    cfg.size = (uint32_t)sizeof(cfg);

    SHIM_CHECK(cfg.size == sizeof(asx_runtime_config), "config size-field roundtrip");
    SHIM_CHECK(cfg.size > 0, "config size nonzero");

    asx_runtime_config_init(&cfg);
    SHIM_CHECK(cfg.size == sizeof(asx_runtime_config), "config init sets size field");
    SHIM_CHECK(cfg.finalizer_poll_budget > 0u, "config init seeds finalizer budget");
}

/* --- Function symbol resolution --- */

static void test_symbol_resolution(void) {
    fprintf(stderr, "--- Symbol resolution ---\n");

    /* Core lifecycle functions resolve */
    SHIM_CHECK(asx_region_open != NULL, "asx_region_open resolves");
    SHIM_CHECK(asx_task_spawn != NULL, "asx_task_spawn resolves");
    SHIM_CHECK(asx_scheduler_run != NULL, "asx_scheduler_run resolves");
    SHIM_CHECK(asx_runtime_reset != NULL, "asx_runtime_reset resolves");

    /* Status helpers */
    SHIM_CHECK(asx_status_str != NULL, "asx_status_str resolves");

    /* ABI version */
    SHIM_CHECK(asx_abi_version_major != NULL, "asx_abi_version_major resolves");

    /* Handle helpers */
    SHIM_CHECK(asx_handle_is_valid(ASX_INVALID_ID) == 0, "invalid handle not valid");

    /* Runtime object surface */
    SHIM_CHECK(asx_runtime_init_checked != NULL, "asx_runtime_init_checked resolves");
    SHIM_CHECK(asx_runtime_init_default_checked != NULL,
               "asx_runtime_init_default_checked resolves");
    SHIM_CHECK(asx_runtime_shutdown != NULL, "asx_runtime_shutdown resolves");
    SHIM_CHECK(asx_runtime_config_init_checked != NULL, "asx_runtime_config_init_checked resolves");

    /* Actor + supervision surface */
    SHIM_CHECK(asx_actor_spawn != NULL, "asx_actor_spawn resolves");
    SHIM_CHECK(asx_actor_cast != NULL, "asx_actor_cast resolves");
    SHIM_CHECK(asx_actor_try_cast != NULL, "asx_actor_try_cast resolves");
    SHIM_CHECK(asx_actor_call != NULL, "asx_actor_call resolves");
    SHIM_CHECK(asx_actor_stop != NULL, "asx_actor_stop resolves");
    SHIM_CHECK(asx_actor_join != NULL, "asx_actor_join resolves");
    SHIM_CHECK(asx_supervisor_spawn != NULL, "asx_supervisor_spawn resolves");
    SHIM_CHECK(asx_supervisor_join != NULL, "asx_supervisor_join resolves");
    SHIM_CHECK(asx_supervisor_abort != NULL, "asx_supervisor_abort resolves");

    /* Typed symbol surface */
    SHIM_CHECK(asx_typed_symbol_register != NULL, "asx_typed_symbol_register resolves");
    SHIM_CHECK(asx_typed_symbol_lookup != NULL, "asx_typed_symbol_lookup resolves");
    SHIM_CHECK(asx_type_kind_str != NULL, "asx_type_kind_str resolves");
}

/* --- Public typed-symbol surface works via umbrella header --- */

static void test_typed_symbol_surface(void) {
    asx_typed_symbol symbol;
    asx_typed_symbol lookup;
    asx_symbol_set exported;
    asx_status st;

    fprintf(stderr, "--- Typed symbol surface ---\n");

    asx_symbol_registry_reset();
    asx_symbol_set_init(&exported);

    st = asx_typed_symbol_register("consumer.latency_ns", ASX_TYPE_KIND_U64, 8u, 8u, &symbol);
    SHIM_CHECK(st == ASX_OK, "typed symbol register succeeds");
    SHIM_CHECK(symbol.symbol != ASX_SYMBOL_INVALID, "typed symbol id valid");
    SHIM_CHECK(symbol.kind == ASX_TYPE_KIND_U64, "typed symbol kind preserved");

    st = asx_typed_symbol_lookup("consumer.latency_ns", &lookup);
    SHIM_CHECK(st == ASX_OK, "typed symbol lookup succeeds");
    SHIM_CHECK(lookup.symbol == symbol.symbol, "typed symbol lookup roundtrip");
    SHIM_CHECK(strcmp(asx_symbol_name(symbol.symbol), "consumer.latency_ns") == 0,
               "typed symbol name roundtrip");

    asx_symbol_set_insert(&exported, symbol.symbol);
    SHIM_CHECK(asx_symbol_set_contains(&exported, symbol.symbol), "symbol set contains symbol");
    SHIM_CHECK(asx_symbol_set_count(&exported) == 1u, "symbol set count tracks insert");
    SHIM_CHECK(strcmp(asx_type_kind_str(ASX_TYPE_KIND_COUNT), "count") == 0,
               "type kind count string exported");
}

/* --- Smoke: basic lifecycle works --- */

static asx_status noop_poll(void *user_data, asx_task_id self) {
    (void)user_data;
    (void)self;
    return ASX_OK;
}

static int shim_is_progress_status(asx_status st) {
    return st == ASX_OK || st == ASX_E_PENDING || st == ASX_E_POLL_BUDGET_EXHAUSTED;
}

typedef struct {
    uint64_t last_cast;
} shim_actor_state;

static asx_status shim_actor_cast(void *state, uint64_t msg, asx_actor_handle self) {
    shim_actor_state *actor = (shim_actor_state *)state;
    (void)self;
    actor->last_cast = msg;
    return ASX_OK;
}

static asx_status shim_actor_call(void *state, uint64_t request, uint64_t *reply,
                                  asx_actor_handle self) {
    shim_actor_state *actor = (shim_actor_state *)state;
    (void)self;
    actor->last_cast = request;
    *reply = request + 1u;
    return ASX_OK;
}

/* A supervised child: each generation runs noop_poll. */
static asx_status shim_child_start(void *user_data, const asx_supervisor_generation *gen,
                                   asx_task_poll_fn *out_poll, void **out_data) {
    (void)gen;
    *out_poll = noop_poll;
    *out_data = user_data;
    return ASX_OK;
}

/* A task in a child region calling the server once. */
typedef struct {
    asx_actor_handle server;
    asx_cx cx;
    asx_actor_op op;
    asx_status status;
    uint64_t reply;
} shim_caller;

static asx_status shim_caller_poll(void *user_data, asx_task_id self) {
    shim_caller *c = (shim_caller *)user_data;
    asx_status st;
    (void)self;
    st = asx_actor_call(c->server, &c->cx, 41u, &c->op, &c->reply);
    if (st == ASX_E_PENDING) return st;
    c->status = st;
    return ASX_OK;
}

static void test_lifecycle_smoke(void) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    asx_status st;

    fprintf(stderr, "--- Lifecycle smoke ---\n");

    asx_runtime_reset();

    st = asx_region_open(&rid);
    SHIM_CHECK(st == ASX_OK, "region_open succeeds");

    st = asx_task_spawn(rid, noop_poll, NULL, &tid);
    SHIM_CHECK(st == ASX_OK, "task_spawn succeeds");

    budget = asx_budget_from_polls(64);
    st = asx_scheduler_run(rid, &budget);
    SHIM_CHECK(st == ASX_OK, "scheduler_run succeeds");
}

static void test_actor_surface_smoke(void) {
    asx_region_id rid;
    asx_actor_handle actor;
    asx_supervisor_handle supervisor;
    asx_region_id callers;
    asx_task_id caller_id;
    shim_caller caller;
    shim_actor_state state;
    asx_supervisor_config cfg;
    asx_child_spec spec;
    asx_budget budget;
    asx_status st;

    fprintf(stderr, "--- Actor surface smoke ---\n");

    memset(&state, 0, sizeof(state));
    asx_runtime_reset();

    st = asx_region_open(&rid);
    SHIM_CHECK(st == ASX_OK, "actor smoke region_open succeeds");

    {
        asx_actor_behavior behavior;
        behavior.init = NULL;
        behavior.handle_cast = shim_actor_cast;
        behavior.handle_call = shim_actor_call;
        behavior.terminate = NULL;
        behavior.on_start_budget = NULL;
        behavior.on_stop_budget = NULL;
        behavior.cast_overflow_policy = ASX_ACTOR_OVERFLOW_REJECT;
        st = asx_actor_spawn(&actor, rid, &behavior, &state, 4u);
    }
    SHIM_CHECK(st == ASX_OK, "actor_spawn succeeds through umbrella header");

    /* An actor with an empty mailbox parks, and a run whose every live task
     * is parked with no wake source returns ASX_E_WOULD_BLOCK. */
    budget = asx_budget_from_polls(4u);
    st = asx_scheduler_run(rid, &budget);
    SHIM_CHECK(st == ASX_E_WOULD_BLOCK, "actor init run parks the idle actor");

    st = asx_actor_try_cast(actor, 9u);
    SHIM_CHECK(st == ASX_OK, "actor_try_cast succeeds");
    memset(&caller, 0, sizeof(caller));
    caller.server = actor;
    caller.status = ASX_E_PENDING;
    st = asx_region_open_child(rid, &callers);
    if (st == ASX_OK) st = asx_task_spawn(callers, shim_caller_poll, &caller, &caller_id);
    if (st == ASX_OK) st = asx_cx_init(&caller.cx, callers, caller_id, ASX_CAP_NONE);
    SHIM_CHECK(st == ASX_OK, "caller task spawns in a child region");

    budget = asx_budget_from_polls(16u);
    st = asx_scheduler_run(rid, &budget);
    SHIM_CHECK(st == ASX_E_WOULD_BLOCK, "actor message run drains the mailbox and parks");

    SHIM_CHECK(caller.status == ASX_OK && caller.reply == 42u, "actor call reply roundtrip");
    SHIM_CHECK(state.last_cast == 41u, "actor state updated through reply handler");

    st = asx_actor_stop(actor);
    SHIM_CHECK(st == ASX_OK, "actor_stop succeeds");

    budget = asx_budget_from_polls(4u);
    st = asx_scheduler_run(rid, &budget);
    SHIM_CHECK(shim_is_progress_status(st), "actor stop run succeeds");

    asx_supervisor_config_init(&cfg, "shim", 1u, 1000000000u);
    asx_child_spec_init(&spec, "child", ASX_CHILD_TEMPORARY, shim_child_start, NULL);

    st = asx_supervisor_spawn(&supervisor, rid, &cfg, &spec, 1u);
    SHIM_CHECK(st == ASX_OK, "supervisor_spawn succeeds");
    SHIM_CHECK(asx_supervisor_task(supervisor) != ASX_INVALID_ID, "supervisor controller visible");

    /* The temporary child's one generation ends; nothing is left to
     * supervise, so the controller closes its region and finishes. */
    budget = asx_budget_from_polls(64u);
    st = asx_scheduler_run(rid, &budget);
    SHIM_CHECK(shim_is_progress_status(st), "supervisor run succeeds");
    SHIM_CHECK(asx_supervisor_finished(supervisor), "supervisor finished");
    SHIM_CHECK(!asx_supervisor_is_alive(supervisor), "supervisor no longer alive");

    st = asx_supervisor_abort(supervisor);
    SHIM_CHECK(st == ASX_OK, "supervisor_abort of a finished supervisor is harmless");
}

/* --- Main --- */

static void test_runtime_creation_fingerprint(void) {
    asx_runtime rt;
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    asx_runtime_builder builder;
    asx_region_id region;
    asx_task_id task;
    asx_task_state state;
    asx_budget budget;
    asx_status st;
    unsigned int path;

    SHIM_CHECK(ASX_CONFIG_FINGERPRINT == asx_runtime_config_fingerprint(),
               "application and library fingerprints match");
    SHIM_CHECK(asx_runtime_config_init(&cfg) == ASX_OK, "matching config initializes");
    SHIM_CHECK(asx_runtime_hooks_init(&hooks) == ASX_OK, "matching hooks initialize");
    SHIM_CHECK(asx_runtime_builder_init(&builder) == ASX_OK, "matching builder initializes");
    for (path = 0u; path < 4u; path++) {
        if (path == 0u)
            st = asx_runtime_init(&rt, &cfg, &hooks);
        else if (path == 1u)
            st = asx_runtime_init_default(&rt);
        else if (path == 2u)
            st = asx_runtime_init_from_env(&rt, "ASX_ABI_CONSUMER_");
        else
            st = asx_runtime_builder_build(&builder, &rt);
        SHIM_CHECK(st == ASX_OK, "matching creation path starts runtime");
        if (st != ASX_OK) continue;
        SHIM_CHECK(asx_region_open(&region) == ASX_OK, "creation path opens region");
        SHIM_CHECK(asx_task_spawn(region, noop_poll, NULL, &task) == ASX_OK,
                   "creation path spawns real task");
        budget = asx_budget_from_polls(64u);
        SHIM_CHECK(asx_scheduler_run(region, &budget) == ASX_OK,
                   "creation path executes scheduler");
        SHIM_CHECK(asx_task_get_state(task, &state) == ASX_OK && state == ASX_TASK_COMPLETED,
                   "creation path completes task");
        SHIM_CHECK(asx_region_close(region) == ASX_OK, "creation path closes region");
        budget = asx_budget_from_polls(64u);
        SHIM_CHECK(asx_region_drain(region, &budget) == ASX_OK, "creation path drains region");
        SHIM_CHECK(asx_runtime_is_quiescent(&rt), "creation path reaches quiescence");
        asx_runtime_shutdown(&rt);
    }
}

static void test_mismatched_creation(void) {
    /* Surround every object with canaries. In particular, run the ordinary
     * preparatory constructors: a packed consumer must be refused before
     * any library-sized memset or field access, not only at final init. */
    struct {
        unsigned char before[32];
        asx_runtime rt;
        asx_runtime_config cfg;
        asx_runtime_hooks hooks;
        asx_runtime_builder builder;
#if ASX_HAS_NATIVE_RUNTIME_SURFACES && ASX_ABI_LIBRARY_HAS_APP
        asx_app app;
        asx_app_config app_config;
#endif
#if defined(ASX_RUNTIME_LAB_H) && ASX_ABI_LIBRARY_HAS_LAB
        asx_lab lab;
        asx_lab_config lab_config;
#endif
        unsigned char after[32];
    } objects;
    unsigned char saved[sizeof(objects)];

    memset(&objects, 0xa5, sizeof(objects));
    memcpy(saved, &objects, sizeof(objects));
    SHIM_CHECK(asx_abi_host_start() == ASX_OK,
               "matching host starts a runtime with a live task and reserved obligation");
    SHIM_CHECK(ASX_CONFIG_FINGERPRINT != asx_runtime_config_fingerprint(),
               "separately compiled consumer fingerprint differs");
    SHIM_CHECK(asx_runtime_config_init(&objects.cfg) == ASX_E_CONFIG_MISMATCH,
               "mismatched config constructor refuses");
    SHIM_CHECK(asx_runtime_hooks_init(&objects.hooks) == ASX_E_CONFIG_MISMATCH,
               "mismatched hook constructor refuses");
    SHIM_CHECK(asx_runtime_builder_init(&objects.builder) == ASX_E_CONFIG_MISMATCH,
               "mismatched default builder constructor refuses");
    SHIM_CHECK(asx_runtime_builder_init_current_thread(&objects.builder) == ASX_E_CONFIG_MISMATCH,
               "mismatched current-thread builder constructor refuses");
    SHIM_CHECK(asx_runtime_builder_init_low_latency(&objects.builder) == ASX_E_CONFIG_MISMATCH,
               "mismatched low-latency builder constructor refuses");
    SHIM_CHECK(asx_runtime_builder_init_high_throughput(&objects.builder) == ASX_E_CONFIG_MISMATCH,
               "mismatched high-throughput builder constructor refuses");
    SHIM_CHECK(asx_runtime_init(&objects.rt, &objects.cfg, &objects.hooks) == ASX_E_CONFIG_MISMATCH,
               "mismatched explicit creation refuses");
    SHIM_CHECK(asx_runtime_init_default(&objects.rt) == ASX_E_CONFIG_MISMATCH,
               "mismatched default creation refuses");
    SHIM_CHECK(asx_runtime_init_from_env(&objects.rt, "ASX_ABI_CONSUMER_") == ASX_E_CONFIG_MISMATCH,
               "mismatched environment creation refuses");
    SHIM_CHECK(asx_runtime_builder_build(&objects.builder, &objects.rt) == ASX_E_CONFIG_MISMATCH,
               "mismatched builder creation refuses");
#if ASX_HAS_NATIVE_RUNTIME_SURFACES && ASX_ABI_LIBRARY_HAS_APP
    SHIM_CHECK(asx_app_init(&objects.app, &objects.app_config) == ASX_E_CONFIG_MISMATCH,
               "mismatched application creation refuses");
#endif
#if defined(ASX_RUNTIME_LAB_H) && ASX_ABI_LIBRARY_HAS_LAB
    SHIM_CHECK(asx_lab_config_init(&objects.lab_config) == ASX_E_CONFIG_MISMATCH,
               "mismatched lab config constructor refuses");
    SHIM_CHECK(asx_lab_init(&objects.lab, &objects.lab_config) == ASX_E_CONFIG_MISMATCH,
               "mismatched lab creation refuses");
    SHIM_CHECK(asx_lab_reset(&objects.lab) == ASX_E_CONFIG_MISMATCH,
               "mismatched lab reconstruction refuses");
    {
        asx_snapshot_id invalid_snapshot = {UINT32_MAX};
        SHIM_CHECK(asx_snapshot_restore(&objects.lab, invalid_snapshot) == ASX_E_CONFIG_MISMATCH,
                   "mismatched snapshot reconstruction refuses before inspecting lab or ID");
    }
#endif
    SHIM_CHECK(memcmp(saved, &objects, sizeof(objects)) == 0,
               "mismatched constructors preserve every object and both canaries");
    SHIM_CHECK(asx_abi_host_finish() == ASX_OK,
               "matching host preserves its live work, completes task and resolves obligation");
}

static void test_higher_level_creation(void) {
#if ASX_HAS_NATIVE_RUNTIME_SURFACES && ASX_ABI_LIBRARY_HAS_APP
    asx_app app;
    asx_app_config app_config = {"fingerprint", 64u};
    SHIM_CHECK(asx_app_init(&app, &app_config) == ASX_OK, "matching application starts");
    SHIM_CHECK(asx_app_run(&app, noop_poll, NULL) == ASX_EXIT_OK,
               "matching application executes main task");
    asx_app_shutdown(&app);
#endif
#if defined(ASX_RUNTIME_LAB_H) && ASX_ABI_LIBRARY_HAS_LAB
    {
        asx_lab lab;
        asx_lab_config config;
        asx_snapshot_id snapshot;
        asx_region_id region;
        asx_task_id task;
        asx_task_state state;
        asx_budget budget;
        SHIM_CHECK(asx_lab_config_init(&config) == ASX_OK, "matching lab config initializes");
        SHIM_CHECK(asx_lab_init(&lab, &config) == ASX_OK, "matching lab starts");
        SHIM_CHECK(asx_snapshot_take(&lab, &snapshot) == ASX_OK, "matching lab saves snapshot");
        SHIM_CHECK(asx_snapshot_restore(&lab, snapshot) == ASX_OK,
                   "matching snapshot reconstructs lab");
        SHIM_CHECK(asx_lab_reset(&lab) == ASX_OK, "matching lab resets");
        SHIM_CHECK(asx_region_open(&region) == ASX_OK, "reconstructed lab opens region");
        SHIM_CHECK(asx_task_spawn(region, noop_poll, NULL, &task) == ASX_OK,
                   "reconstructed lab spawns task");
        budget = asx_budget_from_polls(64u);
        SHIM_CHECK(asx_lab_run(&lab, region, &budget) == ASX_OK, "reconstructed lab executes task");
        SHIM_CHECK(asx_task_get_state(task, &state) == ASX_OK && state == ASX_TASK_COMPLETED,
                   "reconstructed lab completes task");
        SHIM_CHECK(asx_region_close(region) == ASX_OK, "reconstructed lab closes region");
        budget = asx_budget_from_polls(64u);
        SHIM_CHECK(asx_region_drain(region, &budget) == ASX_OK && asx_runtime_is_quiescent(&lab.rt),
                   "reconstructed lab drains to quiescence");
        SHIM_CHECK(asx_snapshot_discard(snapshot) == ASX_OK, "snapshot is released");
        asx_lab_shutdown(&lab);
    }
#endif
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--app-available") == 0) {
        printf("%d\n", ASX_HAS_NATIVE_RUNTIME_SURFACES);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--lab-available") == 0) {
        printf("%d\n", ASX_HAS_BROWSER_TRACE);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--ghost-enabled") == 0) {
#if defined(ASX_DEBUG_GHOST) && ASX_DEBUG_GHOST
        puts("1");
#else
        puts("0");
#endif
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--browser-io-enabled") == 0) {
        printf("%d\n", ASX_HAS_BROWSER_IO);
        return 0;
    }
    fprintf(stderr, "[asx-consumer-shim] ABI/API stability consumer shim test (bd-56t.4)\n\n");

    if (argc == 2 && strcmp(argv[1], "--expect-mismatch") == 0) {
        test_mismatched_creation();
        fprintf(stderr, "[asx-consumer-shim] mismatch: %d passed, %d failed\n", passes, failures);
        return failures > 0 ? 1 : 0;
    }
    if (argc != 1) return 2;
    test_abi_version();
    test_handle_sizes();
    test_frozen_enums();
    test_config_pattern();
    test_symbol_resolution();
    test_typed_symbol_surface();
    test_lifecycle_smoke();
    test_actor_surface_smoke();
    asx_runtime_reset();
    test_runtime_creation_fingerprint();
    test_higher_level_creation();

    fprintf(stderr, "\n[asx-consumer-shim] %d passed, %d failed\n", passes, failures);

    return failures > 0 ? 1 : 0;
}

#endif /* ASX_ABI_MATCHED_HOST */
