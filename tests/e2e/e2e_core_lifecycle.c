/*
 * e2e_core_lifecycle.c — end-to-end scenarios for core runtime semantics
 *
 * Exercises: region lifecycle, task spawn/complete, obligation protocol,
 * cancellation propagation, quiescence close paths, timer wheel, and
 * bounded MPSC channel with two-phase send.
 *
 * Output: one line per scenario in the format:
 *   SCENARIO <id> <pass|fail> [diagnostic]
 *
 * SPDX-License-Identifier: MIT
 */

#include "../test_runtime.h"
#include <asx/asx.h>
#include <asx/runtime/lab.h>
#include <asx/time/timer_wheel.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Suppress warn_unused_result for intentionally-ignored calls */
#define IGNORE_RC(expr)                                                                            \
    do {                                                                                           \
        asx_status ignore_rc_ = (expr);                                                            \
        (void)ignore_rc_;                                                                          \
    } while (0)

/* -------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------- */

static int g_pass = 0;
static int g_fail = 0;
static uint64_t g_seed = 42u;
static uint64_t g_entropy_sample = 0u;
static uint32_t g_source_yields = 0u;
static const char *g_run_id = "standalone-draft";
static const char *g_scenario_id;
static asx_lab g_lab;

#define E2E_STRINGIFY_IMPL(value) #value
#define E2E_STRINGIFY(value) E2E_STRINGIFY_IMPL(value)
#ifndef ASX_E2E_BUILD_SHA
#define ASX_E2E_BUILD_SHA unknown_dirty_draft
#endif
#ifndef ASX_E2E_RUST_REV
#define ASX_E2E_RUST_REV unknown_reference
#endif

typedef struct {
    asx_task_id caller;
    asx_obligation_id obligation;
    asx_obligation_info before;
    asx_obligation_info after;
    asx_status observed;
    asx_status expected;
    const char *first_changed_state;
    int observed_set;
} lifecycle_evidence;

static lifecycle_evidence g_evidence;

static const char *compiled_profile(void) {
#if defined(ASX_PROFILE_CORE)
    return "CORE";
#elif defined(ASX_PROFILE_POSIX)
    return "POSIX";
#elif defined(ASX_PROFILE_WIN32)
    return "WIN32";
#elif defined(ASX_PROFILE_FREESTANDING)
    return "FREESTANDING";
#elif defined(ASX_PROFILE_EMBEDDED_ROUTER)
    return "EMBEDDED_ROUTER";
#elif defined(ASX_PROFILE_HFT)
    return "HFT";
#elif defined(ASX_PROFILE_AUTOMOTIVE)
    return "AUTOMOTIVE";
#elif defined(ASX_PROFILE_PARALLEL)
    return "PARALLEL";
#elif defined(ASX_PROFILE_BROWSER)
    return "BROWSER";
#else
    return "unknown";
#endif
}

static const char *compiled_codec(void) {
#if defined(ASX_CODEC_BIN)
    return "bin";
#elif defined(ASX_CODEC_JSON)
    return "json";
#else
    return "unspecified";
#endif
}

static const char *compiled_resource_class(void) {
#if defined(ASX_RESOURCE_CLASS) && ASX_RESOURCE_CLASS == 1
    return "R1";
#elif defined(ASX_RESOURCE_CLASS) && ASX_RESOURCE_CLASS == 2
    return "R2";
#elif defined(ASX_RESOURCE_CLASS) && ASX_RESOURCE_CLASS == 3
    return "R3";
#else
    return "unclassed";
#endif
}

static void json_string(const char *value) {
    const unsigned char *p = (const unsigned char *)value;
    putchar('"');
    while (*p != 0u) {
        if (*p == '"' || *p == '\\') {
            putchar('\\');
            putchar((int)*p);
        } else if (*p < 0x20u) {
            printf("\\u%04x", (unsigned)*p);
        } else {
            putchar((int)*p);
        }
        p++;
    }
    putchar('"');
}

static uint64_t fingerprint_u64(uint64_t hash, uint64_t value) {
    unsigned i;
    for (i = 0u; i < 8u; i++) {
        hash ^= value & UINT64_C(0xff);
        hash *= UINT64_C(1099511628211);
        value >>= 8u;
    }
    return hash;
}

static uint64_t fingerprint_text(uint64_t hash, const char *text) {
    while (*text != '\0') {
        hash ^= (uint64_t)(unsigned char)*text++;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t config_fingerprint(void) {
    asx_runtime_config cfg;
    uint64_t hash = UINT64_C(14695981039346656037);
    asx_runtime_config_init(&cfg);
    if (g_lab.initialized) cfg = g_lab.rt.config;
    hash = fingerprint_text(hash, compiled_profile());
    hash = fingerprint_text(hash, compiled_codec());
    hash = fingerprint_text(hash, compiled_resource_class());
    hash = fingerprint_u64(hash, g_seed);
    hash = fingerprint_u64(hash, (uint64_t)ASX_DETERMINISTIC);
    hash = fingerprint_u64(hash, asx_runtime_region_capacity());
    hash = fingerprint_u64(hash, asx_runtime_task_capacity());
    hash = fingerprint_u64(hash, asx_runtime_obligation_capacity());
    hash = fingerprint_u64(hash, (uint64_t)cfg.wait_policy);
    hash = fingerprint_u64(hash, (uint64_t)cfg.io_backend);
    hash = fingerprint_u64(hash, (uint64_t)cfg.leak_response);
    hash = fingerprint_u64(hash, cfg.finalizer_poll_budget);
    hash = fingerprint_u64(hash, cfg.finalizer_time_budget_ns);
    hash = fingerprint_u64(hash, (uint64_t)cfg.finalizer_escalation);
    hash = fingerprint_u64(hash, cfg.max_cancel_chain_depth);
    hash = fingerprint_u64(hash, cfg.max_cancel_chain_memory);
    hash = fingerprint_u64(hash, cfg.cleanup_hard_bound);
    hash = fingerprint_u64(hash, cfg.leak_escalation != NULL ? 1u : 0u);
    hash = fingerprint_u64(hash, g_lab.initialized ? g_lab.config.max_polls : 0u);
    hash = fingerprint_u64(hash, g_lab.initialized ? g_lab.config.auto_advance : 0u);
    return hash;
}

static void emit_evidence(const char *test, int pass, const char *diagnostic) {
    printf("DETAIL {\"run_id\":");
    json_string(g_run_id);
    printf(",\"suite\":\"core-lifecycle\",\"test\":");
    json_string(test);
    printf(",\"status\":\"%s\",\"seed\":%" PRIu64, pass ? "pass" : "fail", g_seed);
    printf(",\"profile\":");
    json_string(compiled_profile());
    printf(",\"codec\":");
    json_string(compiled_codec());
    printf(",\"resource_class\":");
    json_string(compiled_resource_class());
    printf(",\"rust_rev\":");
    json_string(E2E_STRINGIFY(ASX_E2E_RUST_REV));
    printf(",\"c_git_sha\":");
    json_string(E2E_STRINGIFY(ASX_E2E_BUILD_SHA));
    printf(",\"config_fingerprint\":\"fnv1a64:%016" PRIx64 "\"", config_fingerprint());
    printf(",\"caller\":\"0x%016" PRIx64 "\",\"holder\":\"0x%016" PRIx64
           "\",\"region\":\"0x%016" PRIx64 "\",\"obligation\":\"0x%016" PRIx64 "\"",
           g_evidence.caller, g_evidence.before.holder, g_evidence.before.region,
           g_evidence.obligation);
    printf(",\"generation\":%u", (unsigned)asx_handle_generation(g_evidence.obligation));
    printf(",\"observed_status\":");
    if (g_evidence.observed_set)
        json_string(asx_status_str(g_evidence.observed));
    else
        printf("null");
    printf(",\"expected_status\":");
    if (g_evidence.observed_set)
        json_string(asx_status_str(g_evidence.expected));
    else
        printf("null");
    printf(",\"first_changed_state\":");
    json_string(g_evidence.first_changed_state);
    printf(",\"first_divergence\":");
    if (pass)
        printf("null");
    else
        json_string(diagnostic);
    printf(",\"before_state\":");
    if (g_evidence.observed_set)
        printf("%d", (int)g_evidence.before.state);
    else
        printf("null");
    printf(",\"after_state\":");
    if (g_evidence.observed_set)
        printf("%d", (int)g_evidence.after.state);
    else
        printf("null");
    printf(",\"after_holder\":\"0x%016" PRIx64 "\",\"after_region\":\"0x%016" PRIx64
           "\",\"entropy_sample\":%" PRIu64 ",\"source_yields\":%u,\"lab_seed_applied\":%s",
           g_evidence.after.holder, g_evidence.after.region, g_entropy_sample,
           (unsigned)g_source_yields, g_lab.initialized ? "true" : "false");
    printf(",\"arena_capacities\":{\"regions\":%u,\"tasks\":%u,\"obligations\":%u},"
           "\"diagnostic\":",
           (unsigned)asx_runtime_region_capacity(), (unsigned)asx_runtime_task_capacity(),
           (unsigned)asx_runtime_obligation_capacity());
    json_string(diagnostic);
    printf("}\n");
}

static void reset_evidence(const char *scenario_id) {
    g_scenario_id = scenario_id;
    g_entropy_sample = 0u;
    g_source_yields = 0u;
    memset(&g_evidence, 0, sizeof(g_evidence));
    g_evidence.first_changed_state = "none";
}

#define SCENARIO_BEGIN(id)                                                                         \
    do {                                                                                           \
        const char *_scenario_id = (id);                                                           \
        int _scenario_ok = 1;                                                                      \
        reset_evidence(_scenario_id);                                                              \
    (void)0

#define SCENARIO_CHECK(cond, msg)                                                                  \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("SCENARIO %s fail %s\n", _scenario_id, (msg));                                  \
            emit_evidence(_scenario_id, 0, (msg));                                                 \
            _scenario_ok = 0;                                                                      \
            g_fail++;                                                                              \
            goto _scenario_end;                                                                    \
        }                                                                                          \
    } while (0)

#define SCENARIO_END()                                                                             \
    _scenario_end:                                                                                 \
    if (_scenario_ok) {                                                                            \
        printf("SCENARIO %s pass\n", _scenario_id);                                                \
        emit_evidence(_scenario_id, 1, "");                                                        \
        g_pass++;                                                                                  \
    }                                                                                              \
    }                                                                                              \
    while (0)

/* Simple poll function: completes immediately */
static asx_status poll_complete(void *ud, asx_task_id self) {
    (void)ud;
    (void)self;
    return ASX_OK;
}

/* Countdown poll: yields N times then completes */
typedef struct {
    asx_co_state co;
    int remaining;
} countdown_state;

static asx_status poll_countdown(void *ud, asx_task_id self) {
    countdown_state *s = (countdown_state *)ud;
    (void)self;
    ASX_CO_BEGIN(&s->co);
    while (s->remaining > 0) {
        s->remaining--;
        ASX_CO_YIELD(&s->co);
    }
    ASX_CO_END(&s->co);
}

/* Cancel-aware poll: checkpoints and finalizes on cancel */
static asx_status poll_cancel_aware(void *ud, asx_task_id self) {
    countdown_state *s = (countdown_state *)ud;
    asx_checkpoint_result cp;

    ASX_CO_BEGIN(&s->co);
    for (;;) {
        if (asx_checkpoint(self, &cp) == ASX_OK && cp.cancelled) {
            IGNORE_RC(asx_task_finalize(self));
            return ASX_OK;
        }
        s->remaining--;
        if (s->remaining <= 0) break;
        ASX_CO_YIELD(&s->co);
    }
    ASX_CO_END(&s->co);
}

/* Obligation probe: an obligation has a holder task, so it is reserved inside
 * a poll and only that task's poll settles it. This task reserves one
 * obligation, reads its state, then commits or aborts it. */
typedef struct {
    asx_region_id region;
    int use_abort; /* settle by abort (else by commit) */
    asx_obligation_id oid;
    asx_status reserve_rc;
    asx_status state_rc;
    asx_obligation_state reserved_state;
    asx_status settle_rc;
} obligation_probe;

static asx_status poll_obligation_probe(void *ud, asx_task_id self) {
    obligation_probe *p = (obligation_probe *)ud;
    (void)self;

    p->reserve_rc = asx_obligation_reserve(p->region, &p->oid);
    if (p->reserve_rc != ASX_OK) return ASX_OK;
    p->state_rc = asx_obligation_get_state(p->oid, &p->reserved_state);
    p->settle_rc = p->use_abort ? asx_obligation_abort(p->oid) : asx_obligation_commit(p->oid);
    return ASX_OK;
}

/* Commit holder: commits, in its own poll, the obligation reserved on its
 * behalf (asx_obligation_reserve_ex names it as the holder). */
typedef struct {
    asx_obligation_id oid;
    asx_status commit_rc;
} commit_holder_state;

static asx_status poll_commit_holder(void *ud, asx_task_id self) {
    commit_holder_state *h = (commit_holder_state *)ud;
    (void)self;
    h->commit_rc = asx_obligation_commit(h->oid);
    return ASX_OK;
}

/* -------------------------------------------------------------------
 * Scenarios
 * ------------------------------------------------------------------- */

static void scenario_region_open_close(void) {
    SCENARIO_BEGIN("region.open_close");
    asx_runtime_reset(test_runtime());

    asx_region_id rid;
    asx_region_state st;
    asx_status rc;

    rc = asx_region_open(test_runtime(), &rid);
    SCENARIO_CHECK(rc == ASX_OK, "region_open failed");

    rc = asx_region_get_state(rid, &st);
    SCENARIO_CHECK(rc == ASX_OK && st == ASX_REGION_OPEN, "region not OPEN");

    rc = asx_region_close(rid);
    SCENARIO_CHECK(rc == ASX_OK, "region_close failed");

    SCENARIO_END();
}

static void scenario_task_spawn_complete(void) {
    SCENARIO_BEGIN("task.spawn_complete");
    asx_runtime_reset(test_runtime());

    asx_region_id rid;
    asx_task_id tid;
    asx_task_state ts;
    asx_outcome out;
    asx_budget budget = asx_budget_infinite();

    SCENARIO_CHECK(asx_region_open(test_runtime(), &rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_task_spawn(rid, poll_complete, NULL, &tid) == ASX_OK, "task_spawn");

    SCENARIO_CHECK(asx_task_get_state(tid, &ts) == ASX_OK && ts == ASX_TASK_CREATED,
                   "task not CREATED");

    SCENARIO_CHECK(asx_scheduler_run(rid, &budget) == ASX_OK, "scheduler_run");

    SCENARIO_CHECK(asx_task_get_state(tid, &ts) == ASX_OK && ts == ASX_TASK_COMPLETED,
                   "task not COMPLETED");

    SCENARIO_CHECK(asx_task_get_outcome(tid, &out) == ASX_OK && out.severity == ASX_OUTCOME_OK,
                   "outcome not OK");

    SCENARIO_END();
}

static void scenario_task_coroutine_yields(void) {
    SCENARIO_BEGIN("task.coroutine_yields");
    asx_runtime_reset(test_runtime());

    asx_region_id rid;
    asx_task_id tid;
    void *state;
    countdown_state *cs;
    asx_budget budget;

    SCENARIO_CHECK(asx_region_open(test_runtime(), &rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_task_spawn_captured(rid, poll_countdown, (uint32_t)sizeof(countdown_state),
                                           NULL, &tid, &state) == ASX_OK,
                   "task_spawn_captured");

    cs = (countdown_state *)state;
    cs->co.line = 0;
    cs->remaining = 3;

    budget = asx_budget_from_polls(10);
    SCENARIO_CHECK(asx_scheduler_run(rid, &budget) == ASX_OK, "scheduler_run");

    asx_task_state ts;
    SCENARIO_CHECK(asx_task_get_state(tid, &ts) == ASX_OK && ts == ASX_TASK_COMPLETED,
                   "task not COMPLETED after 3 yields");

    SCENARIO_END();
}

static void scenario_obligation_reserve_commit(void) {
    SCENARIO_BEGIN("obligation.reserve_commit");
    asx_runtime_reset(test_runtime());

    asx_region_id rid;
    asx_task_id tid;
    asx_obligation_state os;
    obligation_probe probe;
    asx_budget budget = asx_budget_from_polls(10);

    memset(&probe, 0, sizeof(probe));
    probe.reserve_rc = ASX_E_INVALID_STATE; /* until the holder's poll runs */
    probe.state_rc = ASX_E_INVALID_STATE;
    probe.settle_rc = ASX_E_INVALID_STATE;

    SCENARIO_CHECK(asx_region_open(test_runtime(), &rid) == ASX_OK, "region_open");
    probe.region = rid;
    SCENARIO_CHECK(asx_task_spawn(rid, poll_obligation_probe, &probe, &tid) == ASX_OK,
                   "task_spawn");
    SCENARIO_CHECK(asx_scheduler_run(rid, &budget) == ASX_OK, "scheduler_run");

    SCENARIO_CHECK(probe.reserve_rc == ASX_OK, "obligation_reserve");

    SCENARIO_CHECK(probe.state_rc == ASX_OK && probe.reserved_state == ASX_OBLIGATION_RESERVED,
                   "not RESERVED");

    SCENARIO_CHECK(probe.settle_rc == ASX_OK, "obligation_commit");

    SCENARIO_CHECK(asx_obligation_get_state(probe.oid, &os) == ASX_OK &&
                       os == ASX_OBLIGATION_COMMITTED,
                   "not COMMITTED");

    SCENARIO_END();
}

static void scenario_obligation_reserve_abort(void) {
    SCENARIO_BEGIN("obligation.reserve_abort");
    asx_runtime_reset(test_runtime());

    asx_region_id rid;
    asx_task_id tid;
    asx_obligation_state os;
    obligation_probe probe;
    asx_budget budget = asx_budget_from_polls(10);

    memset(&probe, 0, sizeof(probe));
    probe.use_abort = 1;
    probe.reserve_rc = ASX_E_INVALID_STATE; /* until the holder's poll runs */
    probe.state_rc = ASX_E_INVALID_STATE;
    probe.settle_rc = ASX_E_INVALID_STATE;

    SCENARIO_CHECK(asx_region_open(test_runtime(), &rid) == ASX_OK, "region_open");
    probe.region = rid;
    SCENARIO_CHECK(asx_task_spawn(rid, poll_obligation_probe, &probe, &tid) == ASX_OK,
                   "task_spawn");
    SCENARIO_CHECK(asx_scheduler_run(rid, &budget) == ASX_OK, "scheduler_run");

    SCENARIO_CHECK(probe.reserve_rc == ASX_OK, "obligation_reserve");

    SCENARIO_CHECK(probe.settle_rc == ASX_OK, "obligation_abort");

    SCENARIO_CHECK(asx_obligation_get_state(probe.oid, &os) == ASX_OK &&
                       os == ASX_OBLIGATION_ABORTED,
                   "not ABORTED");

    SCENARIO_END();
}

static void scenario_cancel_checkpoint_finalize(void) {
    SCENARIO_BEGIN("cancel.checkpoint_finalize");
    asx_runtime_reset(test_runtime());

    asx_region_id rid;
    asx_task_id tid;
    void *state;
    countdown_state *cs;
    asx_budget budget;

    SCENARIO_CHECK(asx_region_open(test_runtime(), &rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_task_spawn_captured(rid, poll_cancel_aware,
                                           (uint32_t)sizeof(countdown_state), NULL, &tid,
                                           &state) == ASX_OK,
                   "task_spawn_captured");

    cs = (countdown_state *)state;
    cs->co.line = 0;
    cs->remaining = 100; /* would run forever without cancel */

    /* Cancel the task */
    SCENARIO_CHECK(asx_task_cancel(tid, ASX_CANCEL_USER) == ASX_OK, "task_cancel");

    /* Run scheduler — task should observe cancel and finalize */
    budget = asx_budget_from_polls(20);
    IGNORE_RC(asx_scheduler_run(rid, &budget));

    asx_task_state ts;
    SCENARIO_CHECK(asx_task_get_state(tid, &ts) == ASX_OK && ts == ASX_TASK_COMPLETED,
                   "task not COMPLETED after cancel");

    asx_outcome out;
    SCENARIO_CHECK(asx_task_get_outcome(tid, &out) == ASX_OK, "get_outcome");

    SCENARIO_END();
}

static void scenario_cancel_propagate_region(void) {
    SCENARIO_BEGIN("cancel.propagate_region");
    asx_runtime_reset(test_runtime());

    asx_region_id rid;
    asx_task_id t1, t2, t3;
    void *s1, *s2, *s3;
    countdown_state *cs;

    SCENARIO_CHECK(asx_region_open(test_runtime(), &rid) == ASX_OK, "region_open");

    /* Spawn 3 long-running tasks */
    SCENARIO_CHECK(asx_task_spawn_captured(rid, poll_cancel_aware,
                                           (uint32_t)sizeof(countdown_state), NULL, &t1,
                                           &s1) == ASX_OK,
                   "spawn_t1");
    cs = (countdown_state *)s1;
    cs->co.line = 0;
    cs->remaining = 100;

    SCENARIO_CHECK(asx_task_spawn_captured(rid, poll_cancel_aware,
                                           (uint32_t)sizeof(countdown_state), NULL, &t2,
                                           &s2) == ASX_OK,
                   "spawn_t2");
    cs = (countdown_state *)s2;
    cs->co.line = 0;
    cs->remaining = 100;

    SCENARIO_CHECK(asx_task_spawn_captured(rid, poll_cancel_aware,
                                           (uint32_t)sizeof(countdown_state), NULL, &t3,
                                           &s3) == ASX_OK,
                   "spawn_t3");
    cs = (countdown_state *)s3;
    cs->co.line = 0;
    cs->remaining = 100;

    /* Propagate cancel to all tasks */
    uint32_t cancelled = asx_cancel_propagate(rid, ASX_CANCEL_SHUTDOWN);
    SCENARIO_CHECK(cancelled == 3, "propagate should cancel 3 tasks");

    /* Run scheduler to completion */
    asx_budget budget = asx_budget_from_polls(60);
    IGNORE_RC(asx_scheduler_run(rid, &budget));

    asx_task_state ts;
    SCENARIO_CHECK(asx_task_get_state(t1, &ts) == ASX_OK && ts == ASX_TASK_COMPLETED,
                   "t1 not completed");
    SCENARIO_CHECK(asx_task_get_state(t2, &ts) == ASX_OK && ts == ASX_TASK_COMPLETED,
                   "t2 not completed");
    SCENARIO_CHECK(asx_task_get_state(t3, &ts) == ASX_OK && ts == ASX_TASK_COMPLETED,
                   "t3 not completed");

    SCENARIO_END();
}

static void scenario_quiescence_empty_region(void) {
    SCENARIO_BEGIN("quiescence.empty_region");
    asx_runtime_reset(test_runtime());

    asx_region_id rid;
    asx_budget budget = asx_budget_from_polls(10);

    SCENARIO_CHECK(asx_region_open(test_runtime(), &rid) == ASX_OK, "region_open");

    /* Drain drives region through Close → Finalize → Closed */
    asx_status rc = asx_region_drain(rid, &budget);
    SCENARIO_CHECK(rc == ASX_OK, "region_drain");

    /* Empty closed region should be quiescent */
    rc = asx_quiescence_check(rid);
    SCENARIO_CHECK(rc == ASX_OK, "quiescence_check failed on empty region");

    SCENARIO_END();
}

static void scenario_quiescence_after_drain(void) {
    SCENARIO_BEGIN("quiescence.after_drain");
    asx_runtime_reset(test_runtime());

    asx_region_id rid;
    asx_task_id tid;
    void *state;
    countdown_state *cs;

    SCENARIO_CHECK(asx_region_open(test_runtime(), &rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_task_spawn_captured(rid, poll_countdown, (uint32_t)sizeof(countdown_state),
                                           NULL, &tid, &state) == ASX_OK,
                   "spawn");

    cs = (countdown_state *)state;
    cs->co.line = 0;
    cs->remaining = 2;

    asx_budget budget = asx_budget_from_polls(20);
    asx_status rc = asx_region_drain(rid, &budget);
    SCENARIO_CHECK(rc == ASX_OK, "region_drain");

    rc = asx_quiescence_check(rid);
    SCENARIO_CHECK(rc == ASX_OK, "quiescence_check after drain");

    SCENARIO_END();
}

static void scenario_timer_register_expire(void) {
    SCENARIO_BEGIN("timer.register_expire");

    asx_timer_wheel *wheel = asx_timer_wheel_global();
    asx_timer_wheel_reset(wheel);

    asx_timer_handle h;
    int marker = 42;
    void *wakers[4];

    SCENARIO_CHECK(asx_timer_register(wheel, 100, &marker, &h) == ASX_OK, "timer_register");
    SCENARIO_CHECK(asx_timer_active_count(wheel) == 1, "active_count != 1");

    /* Advance past deadline */
    uint32_t fired = asx_timer_collect_expired(wheel, 101, wakers, 4);
    SCENARIO_CHECK(fired == 1, "expected 1 timer to fire");
    SCENARIO_CHECK(wakers[0] == &marker, "waker_data mismatch");
    SCENARIO_CHECK(asx_timer_active_count(wheel) == 0, "active_count != 0 after fire");

    SCENARIO_END();
}

static void scenario_timer_cancel_before_expire(void) {
    SCENARIO_BEGIN("timer.cancel_before_expire");

    asx_timer_wheel *wheel = asx_timer_wheel_global();
    asx_timer_wheel_reset(wheel);

    asx_timer_handle h;
    int marker = 99;
    void *wakers[4];

    SCENARIO_CHECK(asx_timer_register(wheel, 200, &marker, &h) == ASX_OK, "timer_register");

    int cancelled = asx_timer_cancel(wheel, &h);
    SCENARIO_CHECK(cancelled == 1, "cancel should return 1");
    SCENARIO_CHECK(asx_timer_active_count(wheel) == 0, "active after cancel");

    uint32_t fired = asx_timer_collect_expired(wheel, 300, wakers, 4);
    SCENARIO_CHECK(fired == 0, "cancelled timer should not fire");

    SCENARIO_END();
}

static void scenario_timer_deterministic_order(void) {
    SCENARIO_BEGIN("timer.deterministic_order");

    asx_timer_wheel *wheel = asx_timer_wheel_global();
    asx_timer_wheel_reset(wheel);

    int a = 1, b = 2, c = 3;
    asx_timer_handle ha, hb, hc;
    void *wakers[8];

    /* Same deadline — should fire in insertion order */
    SCENARIO_CHECK(asx_timer_register(wheel, 50, &a, &ha) == ASX_OK, "reg_a");
    SCENARIO_CHECK(asx_timer_register(wheel, 50, &b, &hb) == ASX_OK, "reg_b");
    SCENARIO_CHECK(asx_timer_register(wheel, 50, &c, &hc) == ASX_OK, "reg_c");

    uint32_t fired = asx_timer_collect_expired(wheel, 51, wakers, 8);
    SCENARIO_CHECK(fired == 3, "expected 3 timers");
    SCENARIO_CHECK(wakers[0] == &a && wakers[1] == &b && wakers[2] == &c,
                   "insertion order violated");

    SCENARIO_END();
}

static void scenario_channel_reserve_send_recv(void) {
    SCENARIO_BEGIN("channel.reserve_send_recv");
    asx_runtime_reset(test_runtime());
    asx_channel_reset();

    asx_region_id rid;
    asx_channel_id cid;
    asx_send_permit permit;
    uint64_t value;

    SCENARIO_CHECK(asx_region_open(test_runtime(), &rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_channel_create(rid, 4, &cid) == ASX_OK, "channel_create");

    /* Reserve and send */
    SCENARIO_CHECK(asx_channel_try_reserve(cid, &permit) == ASX_OK, "try_reserve");
    SCENARIO_CHECK(asx_send_permit_send(&permit, 0xDEADBEEF) == ASX_OK, "permit_send");

    /* Receive */
    SCENARIO_CHECK(asx_channel_try_recv(cid, &value) == ASX_OK, "try_recv");
    SCENARIO_CHECK(value == 0xDEADBEEF, "value mismatch");

    /* Queue should be empty now */
    uint32_t qlen;
    SCENARIO_CHECK(asx_channel_queue_len(cid, &qlen) == ASX_OK && qlen == 0,
                   "queue not empty after recv");

    SCENARIO_END();
}

static void scenario_channel_reserve_abort(void) {
    SCENARIO_BEGIN("channel.reserve_abort");
    asx_runtime_reset(test_runtime());
    asx_channel_reset();

    asx_region_id rid;
    asx_channel_id cid;
    asx_send_permit permit;

    SCENARIO_CHECK(asx_region_open(test_runtime(), &rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_channel_create(rid, 2, &cid) == ASX_OK, "channel_create");

    SCENARIO_CHECK(asx_channel_try_reserve(cid, &permit) == ASX_OK, "try_reserve");

    /* Abort — capacity should return */
    asx_send_permit_abort(&permit);

    uint32_t reserved;
    SCENARIO_CHECK(asx_channel_reserved_count(cid, &reserved) == ASX_OK && reserved == 0,
                   "reserved count not 0 after abort");

    SCENARIO_END();
}

static void scenario_channel_fifo_order(void) {
    SCENARIO_BEGIN("channel.fifo_order");
    asx_runtime_reset(test_runtime());
    asx_channel_reset();

    asx_region_id rid;
    asx_channel_id cid;
    asx_send_permit p;
    uint64_t v;

    SCENARIO_CHECK(asx_region_open(test_runtime(), &rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_channel_create(rid, 8, &cid) == ASX_OK, "channel_create");

    /* Send 3 values */
    SCENARIO_CHECK(asx_channel_try_reserve(cid, &p) == ASX_OK, "reserve_1");
    SCENARIO_CHECK(asx_send_permit_send(&p, 10) == ASX_OK, "send_1");
    SCENARIO_CHECK(asx_channel_try_reserve(cid, &p) == ASX_OK, "reserve_2");
    SCENARIO_CHECK(asx_send_permit_send(&p, 20) == ASX_OK, "send_2");
    SCENARIO_CHECK(asx_channel_try_reserve(cid, &p) == ASX_OK, "reserve_3");
    SCENARIO_CHECK(asx_send_permit_send(&p, 30) == ASX_OK, "send_3");

    /* Receive in FIFO order */
    SCENARIO_CHECK(asx_channel_try_recv(cid, &v) == ASX_OK && v == 10, "fifo_1");
    SCENARIO_CHECK(asx_channel_try_recv(cid, &v) == ASX_OK && v == 20, "fifo_2");
    SCENARIO_CHECK(asx_channel_try_recv(cid, &v) == ASX_OK && v == 30, "fifo_3");

    SCENARIO_END();
}

static void scenario_channel_close_sender(void) {
    SCENARIO_BEGIN("channel.close_sender");
    asx_runtime_reset(test_runtime());
    asx_channel_reset();

    asx_region_id rid;
    asx_channel_id cid;
    asx_send_permit p;
    uint64_t v;
    asx_channel_state cs;

    SCENARIO_CHECK(asx_region_open(test_runtime(), &rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_channel_create(rid, 4, &cid) == ASX_OK, "channel_create");

    /* Send one value, then close sender */
    SCENARIO_CHECK(asx_channel_try_reserve(cid, &p) == ASX_OK, "reserve");
    SCENARIO_CHECK(asx_send_permit_send(&p, 42) == ASX_OK, "send");
    SCENARIO_CHECK(asx_channel_close_sender(cid) == ASX_OK, "close_sender");

    SCENARIO_CHECK(asx_channel_get_state(cid, &cs) == ASX_OK && cs == ASX_CHANNEL_SENDER_CLOSED,
                   "not SENDER_CLOSED");

    /* Can still receive pending messages */
    SCENARIO_CHECK(asx_channel_try_recv(cid, &v) == ASX_OK && v == 42, "recv after sender close");

    /* Next recv should report disconnected */
    asx_status rc = asx_channel_try_recv(cid, &v);
    SCENARIO_CHECK(rc == ASX_E_DISCONNECTED, "expected DISCONNECTED");

    SCENARIO_END();
}

static void scenario_trace_digest_deterministic(void) {
    SCENARIO_BEGIN("trace.digest_deterministic");
    asx_runtime_reset(test_runtime());
    asx_trace_reset(test_runtime());

    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget = asx_budget_infinite();

    SCENARIO_CHECK(asx_region_open(test_runtime(), &rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_task_spawn(rid, poll_complete, NULL, &tid) == ASX_OK, "task_spawn");
    SCENARIO_CHECK(asx_scheduler_run(rid, &budget) == ASX_OK, "scheduler_run");

    uint64_t digest1 = asx_trace_digest(test_runtime());

    /* Reset and replay identical scenario */
    asx_runtime_reset(test_runtime());
    asx_trace_reset(test_runtime());

    SCENARIO_CHECK(asx_region_open(test_runtime(), &rid) == ASX_OK, "region_open_2");
    SCENARIO_CHECK(asx_task_spawn(rid, poll_complete, NULL, &tid) == ASX_OK, "task_spawn_2");
    SCENARIO_CHECK(asx_scheduler_run(rid, &budget) == ASX_OK, "scheduler_run_2");

    uint64_t digest2 = asx_trace_digest(test_runtime());

    SCENARIO_CHECK(digest1 == digest2, "trace digest not deterministic");
    SCENARIO_CHECK(digest1 != 0, "trace digest is zero");

    SCENARIO_END();
}

static void scenario_region_poison_containment(void) {
    SCENARIO_BEGIN("region.poison_containment");
    asx_runtime_reset(test_runtime());

    asx_region_id rid;
    int poisoned;

    SCENARIO_CHECK(asx_region_open(test_runtime(), &rid) == ASX_OK, "region_open");

    SCENARIO_CHECK(asx_region_is_poisoned(rid, &poisoned) == ASX_OK && poisoned == 0,
                   "should not be poisoned initially");

    SCENARIO_CHECK(asx_region_poison(rid) == ASX_OK, "region_poison");

    SCENARIO_CHECK(asx_region_is_poisoned(rid, &poisoned) == ASX_OK && poisoned == 1,
                   "should be poisoned");

    /* Spawn should fail on poisoned region */
    asx_task_id tid;
    asx_status rc = asx_task_spawn(rid, poll_complete, NULL, &tid);
    SCENARIO_CHECK(rc == ASX_E_REGION_POISONED, "spawn should fail on poisoned");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario: drain progress snapshot (bd-1eqo.5.2)
 *
 * Verifies that asx_region_drain_progress reports accurate task,
 * obligation, and cleanup counters during a region drain lifecycle.
 * ------------------------------------------------------------------- */

static void scenario_drain_progress_snapshot(void) {
    SCENARIO_BEGIN("drain_progress.snapshot");

    asx_runtime_reset(test_runtime());
    asx_region_id rid;
    SCENARIO_CHECK(asx_region_open(test_runtime(), &rid) == ASX_OK, "region_open");

    /* Spawn two tasks and create one obligation: t1 is its holder and
     * commits it in its own poll, which the drain below runs. */
    asx_task_id t1, t2;
    commit_holder_state holder;
    holder.oid = ASX_INVALID_ID;
    holder.commit_rc = ASX_E_INVALID_STATE; /* until t1's poll runs */
    SCENARIO_CHECK(asx_task_spawn(rid, poll_commit_holder, &holder, &t1) == ASX_OK, "spawn_t1");
    SCENARIO_CHECK(asx_task_spawn(rid, poll_complete, NULL, &t2) == ASX_OK, "spawn_t2");
    SCENARIO_CHECK(asx_obligation_reserve_ex(rid, ASX_OBLIGATION_KIND_GENERIC, t1, &holder.oid) ==
                       ASX_OK,
                   "reserve_obligation");

    /* Snapshot before drain: 2 live tasks, 1 reserved obligation */
    asx_drain_progress prog;
    SCENARIO_CHECK(asx_region_drain_progress(rid, &prog) == ASX_OK, "drain_progress_before");
    SCENARIO_CHECK(prog.region_state == ASX_REGION_OPEN, "state_open");
    SCENARIO_CHECK(prog.tasks_live == 2, "tasks_live_2");
    SCENARIO_CHECK(prog.obligations_total == 1, "obligations_total_1");
    SCENARIO_CHECK(prog.obligations_reserved == 1, "obligations_reserved_1");

    /* Drain: t1 resolves its obligation when the drain polls it */
    asx_budget budget = asx_budget_from_polls(128);
    IGNORE_RC(asx_region_drain(rid, &budget));
    SCENARIO_CHECK(holder.commit_rc == ASX_OK, "commit_obligation");

    /* Snapshot after drain */
    SCENARIO_CHECK(asx_region_drain_progress(rid, &prog) == ASX_OK, "drain_progress_after");
    SCENARIO_CHECK(prog.tasks_live == 0, "tasks_drained");
    SCENARIO_CHECK(prog.obligations_reserved == 0, "obligations_resolved");
    SCENARIO_CHECK(prog.cleanup_drained == 1, "cleanup_drained");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario: quiescence evidence decomposition (bd-1eqo.5.2)
 *
 * Verifies that asx_quiescence_check_detailed decomposes quiescence
 * into Q1-Q4 conjuncts and reports correct per-condition status.
 * ------------------------------------------------------------------- */

static void scenario_quiescence_evidence(void) {
    SCENARIO_BEGIN("quiescence.evidence");

    asx_runtime_reset(test_runtime());
    asx_region_id rid;
    SCENARIO_CHECK(asx_region_open(test_runtime(), &rid) == ASX_OK, "region_open");

    /* Spawn a task (still READY, not yet polled) */
    asx_task_id tid;
    SCENARIO_CHECK(asx_task_spawn(rid, poll_complete, NULL, &tid) == ASX_OK, "spawn");

    /* Before drain: not quiescent (task still live, region open) */
    asx_quiescence_report report;
    SCENARIO_CHECK(asx_quiescence_check_detailed(rid, &report) == ASX_OK, "detailed_before");
    SCENARIO_CHECK(report.quiescent == 0, "not_quiescent_before");
    SCENARIO_CHECK(report.q1_tasks_complete == 0, "q1_false_before");

    /* Full drain — will run scheduler, cancel, finalize, close */
    asx_budget budget = asx_budget_from_polls(64);
    SCENARIO_CHECK(asx_region_drain(rid, &budget) == ASX_OK, "drain_ok");

    /* After drain: fully quiescent */
    SCENARIO_CHECK(asx_quiescence_check_detailed(rid, &report) == ASX_OK, "detailed_after");
    SCENARIO_CHECK(report.quiescent == 1, "quiescent_after");
    SCENARIO_CHECK(report.q1_tasks_complete == 1, "q1_true");
    SCENARIO_CHECK(report.q2_children_closed == 1, "q2_true");
    SCENARIO_CHECK(report.q3_obligations_resolved == 1, "q3_true");
    SCENARIO_CHECK(report.q4_cleanup_drained == 1, "q4_true");
    SCENARIO_CHECK(report.region_state == ASX_REGION_CLOSED, "state_closed");

    /* Cross-validate with basic quiescence check */
    SCENARIO_CHECK(asx_quiescence_check(rid) == ASX_OK, "basic_quiescence_agrees");
    SCENARIO_CHECK(asx_region_is_quiescent(rid) == 1, "bool_quiescence_agrees");

    SCENARIO_END();
}

/* The source and destination execute inside actual cooperative polls.
 * The Lab seed changes their spawn order and the source's initial yields. */
typedef enum {
    HOLDER_RAW_GUARD,
    HOLDER_HANDOFF_SAME,
    HOLDER_HANDOFF_CROSS,
    HOLDER_HANDOFF_FULL,
    HOLDER_HANDOFF_CANCEL
} holder_case_kind;

typedef enum {
    HOLDER_COMMIT,
    HOLDER_ABORT,
    HOLDER_DROP,
    HOLDER_HANDOFF,
    HOLDER_ABORT_CANCEL
} holder_operation;

typedef struct {
    holder_case_kind kind;
    asx_region_id root;
    asx_region_id source_region;
    asx_region_id destination_region;
    asx_task_id source;
    asx_task_id destination;
    asx_obligation_id obligation;
    /* HOLDER_HANDOFF_FULL: an independent task of the destination region
     * holds the region's one obligation credit until it is released. */
    asx_task_id blocker_task;
    asx_obligation_id blocker;
    int blocker_release;
    asx_status blocker_status;
    uint32_t initial_yields;
    int ready;
    int source_action;
    int destination_action;
    int failed;
} holder_case;

static uint32_t obligation_events(asx_obligation_id id, asx_trace_event_kind kind) {
    asx_trace_event event;
    uint32_t i;
    uint32_t n = 0u;
    for (i = 0u; i < asx_trace_event_count(test_runtime()); i++) {
        if (asx_trace_event_get(test_runtime(), i, &event) && event.entity_id == id &&
            event.kind == kind)
            n++;
    }
    return n;
}

static const char *first_obligation_change(const asx_obligation_info *before,
                                           const asx_obligation_info *after) {
    if (before->state != after->state) return "state";
    if (before->kind != after->kind) return "kind";
    if (before->holder != after->holder) return "holder";
    if (before->region != after->region) return "region";
    if (before->abort_reason != after->abort_reason) return "abort_reason";
    return "none";
}

static int holder_operate(holder_case *c, asx_task_id caller, holder_operation operation,
                          asx_status expected, const char *step) {
    uint32_t commits = obligation_events(c->obligation, ASX_TRACE_OBLIGATION_COMMIT);
    uint32_t aborts = obligation_events(c->obligation, ASX_TRACE_OBLIGATION_ABORT);
    uint32_t leaks = obligation_events(c->obligation, ASX_TRACE_OBLIGATION_LEAK);
    uint32_t handoffs = obligation_events(c->obligation, ASX_TRACE_OBLIGATION_HANDOFF);
    char test[160];
    int pass;
    asx_status st;

    memset(&g_evidence, 0, sizeof(g_evidence));
    g_evidence.caller = asx_task_current();
    g_evidence.obligation = c->obligation;
    g_evidence.expected = expected;
    g_evidence.observed_set = 1;
    g_evidence.first_changed_state = "lookup";
    st = asx_obligation_get_info(c->obligation, &g_evidence.before);
    if (st != ASX_OK) {
        c->failed = 1;
        g_evidence.observed = st;
        emit_evidence(g_scenario_id, 0, "credit missing before actual operation");
        return 0;
    }
    switch (operation) {
    case HOLDER_COMMIT: st = asx_obligation_commit(c->obligation); break;
    case HOLDER_ABORT: st = asx_obligation_abort(c->obligation); break;
    case HOLDER_DROP: st = asx_obligation_drop(c->obligation); break;
    case HOLDER_HANDOFF: st = asx_obligation_handoff(c->obligation, c->destination); break;
    case HOLDER_ABORT_CANCEL:
        st = asx_obligation_abort_with_reason(c->obligation, ASX_OBLIGATION_ABORT_CANCEL);
        break;
    }
    g_evidence.observed = st;
    pass = st == expected && g_evidence.caller != ASX_INVALID_ID &&
           asx_handle_index(g_evidence.caller) == asx_handle_index(caller);
    if (asx_obligation_get_info(c->obligation, &g_evidence.after) != ASX_OK) {
        pass = 0;
    } else {
        g_evidence.first_changed_state =
            first_obligation_change(&g_evidence.before, &g_evidence.after);
        if (expected != ASX_OK) {
            pass = pass && strcmp(g_evidence.first_changed_state, "none") == 0;
        } else if (operation == HOLDER_HANDOFF) {
            pass = pass && g_evidence.after.state == ASX_OBLIGATION_RESERVED &&
                   g_evidence.after.kind == g_evidence.before.kind &&
                   g_evidence.after.abort_reason == g_evidence.before.abort_reason &&
                   asx_handle_index(g_evidence.after.holder) == asx_handle_index(c->destination) &&
                   g_evidence.after.region == c->destination_region;
        } else {
            pass = pass && g_evidence.after.holder == g_evidence.before.holder &&
                   g_evidence.after.region == g_evidence.before.region &&
                   g_evidence.after.kind == g_evidence.before.kind &&
                   g_evidence.after.state == (operation == HOLDER_COMMIT ? ASX_OBLIGATION_COMMITTED
                                                                         : ASX_OBLIGATION_ABORTED);
            if (operation == HOLDER_ABORT_CANCEL) {
                pass = pass && g_evidence.after.abort_reason == ASX_OBLIGATION_ABORT_CANCEL;
            }
        }
    }
    pass = pass &&
           obligation_events(c->obligation, ASX_TRACE_OBLIGATION_COMMIT) ==
               commits + (expected == ASX_OK && operation == HOLDER_COMMIT ? 1u : 0u) &&
           obligation_events(c->obligation, ASX_TRACE_OBLIGATION_ABORT) ==
               aborts + (expected == ASX_OK && operation == HOLDER_ABORT_CANCEL ? 1u : 0u) &&
           obligation_events(c->obligation, ASX_TRACE_OBLIGATION_LEAK) == leaks &&
           obligation_events(c->obligation, ASX_TRACE_OBLIGATION_HANDOFF) ==
               handoffs + (expected == ASX_OK && operation == HOLDER_HANDOFF ? 1u : 0u);
    if (snprintf(test, sizeof(test), "%s.%s", g_scenario_id, step) < 0) {
        c->failed = 1;
        return 0;
    }
    emit_evidence(test, pass, pass ? "" : "status, ownership, state or trace delta differs");
    if (!pass) c->failed = 1;
    return pass;
}

static void holder_raw_denials(holder_case *c, asx_task_id caller) {
    static const holder_operation operations[] = {HOLDER_COMMIT, HOLDER_ABORT, HOLDER_DROP};
    static const char *steps[] = {"raw_commit_denied", "raw_abort_denied", "raw_drop_denied"};
    uint32_t i;
    uint32_t rotation = (uint32_t)(g_entropy_sample % 3u);
    for (i = 0u; i < 3u; i++) {
        uint32_t which = (i + rotation) % 3u;
        (void)holder_operate(c, caller, operations[which], ASX_E_PERMISSION_DENIED, steps[which]);
    }
}

static asx_status poll_holder_source(void *data, asx_task_id self) {
    holder_case *c = (holder_case *)data;
    asx_status st;
    if (!c->ready) {
        if (c->initial_yields > 0u) {
            c->initial_yields--;
            g_source_yields++;
            return ASX_E_PENDING;
        }
        st = asx_obligation_reserve_ex(c->source_region, ASX_OBLIGATION_KIND_SEND_PERMIT, self,
                                       &c->obligation);
        if (st != ASX_OK) return st;
        c->ready = 1;
    }
    if (!c->source_action) {
        st = asx_task_park(self);
        return st == ASX_OK ? ASX_E_PENDING : st;
    }
    if (c->kind == HOLDER_RAW_GUARD) {
        (void)holder_operate(c, self, HOLDER_COMMIT, ASX_OK, "holder_commit");
    } else {
        asx_status expected = c->kind == HOLDER_HANDOFF_FULL && c->source_action == 1
                                  ? ASX_E_ADMISSION_LIMIT
                                  : ASX_OK;
        (void)holder_operate(c, self, HOLDER_HANDOFF, expected, "handoff");
        if (expected == ASX_E_ADMISSION_LIMIT) {
            c->source_action = 0;
            st = asx_task_park(self);
            return st == ASX_OK ? ASX_E_PENDING : st;
        }
        holder_raw_denials(c, self);
    }
    return c->failed ? ASX_E_INVALID_STATE : ASX_OK;
}

static asx_status poll_holder_destination(void *data, asx_task_id self) {
    holder_case *c = (holder_case *)data;
    asx_status st;
    if (!c->destination_action) {
        st = asx_task_park(self);
        return st == ASX_OK ? ASX_E_PENDING : st;
    }
    if (c->kind == HOLDER_RAW_GUARD) {
        holder_raw_denials(c, self);
    } else if (c->kind == HOLDER_HANDOFF_CANCEL) {
        asx_checkpoint_result checkpoint;
        st = asx_checkpoint(self, &checkpoint);
        if (st != ASX_OK || !checkpoint.cancelled) return ASX_E_INVALID_STATE;
        (void)holder_operate(c, self, HOLDER_ABORT_CANCEL, ASX_OK, "new_holder_cancel_abort");
        st = asx_task_finalize(self);
        if (st != ASX_OK) return st;
    } else {
        (void)holder_operate(c, self, HOLDER_COMMIT, ASX_OK, "new_holder_commit");
    }
    return c->failed ? ASX_E_INVALID_STATE : ASX_OK;
}

/* The independent holder behind the full destination quota: it parks until
 * released, then aborts the credit it holds in its own poll. */
static asx_status poll_holder_blocker(void *data, asx_task_id self) {
    holder_case *c = (holder_case *)data;
    asx_status st;
    if (!c->blocker_release) {
        st = asx_task_park(self);
        return st == ASX_OK ? ASX_E_PENDING : st;
    }
    c->blocker_status = asx_obligation_abort(c->blocker);
    return ASX_OK;
}

static void scenario_real_holders(const char *id, holder_case_kind kind) {
    SCENARIO_BEGIN(id);
    holder_case c;
    asx_lab_config cfg;
    asx_budget budget;
    asx_obligation_info info;
    asx_region_limits limits = {ASX_REGION_UNLIMITED, ASX_REGION_UNLIMITED, 1u};
    asx_drain_progress progress;
    asx_outcome outcome;
    uint64_t leaks;
    int cross_region = kind != HOLDER_RAW_GUARD && kind != HOLDER_HANDOFF_SAME;

    memset(&c, 0, sizeof(c));
    c.kind = kind;
    asx_runtime_reset(test_runtime());
    asx_lab_config_init(&cfg);
    cfg.seed = g_seed;
    cfg.max_polls = 128u;
    /* The sweep scheduler, not the lab's default dispatch (bd-9kll.9.5):
     * holder_operate reads each obligation trace event inside the holder's
     * own poll, and lab dispatch posts obligation events to the end of the
     * poll, as Rust drains them after it. The scenario checks ownership as
     * a live host sees it. */
    cfg.scheduler = ASX_LAB_SCHEDULER_SWEEP;
    SCENARIO_CHECK(asx_lab_init(&g_lab, &cfg) == ASX_OK, "seeded lab init");
    SCENARIO_CHECK(g_lab.config.seed == g_seed, "requested seed not applied to lab");
    SCENARIO_CHECK(asx_runtime_region_capacity() == (uint32_t)ASX_MAX_REGIONS &&
                       asx_runtime_task_capacity() == (uint32_t)ASX_MAX_TASKS &&
                       asx_runtime_obligation_capacity() == (uint32_t)ASX_MAX_OBLIGATIONS,
                   "binary and linked library resource configuration differ");
    g_entropy_sample = asx_lab_random_u64(&g_lab);
    c.initial_yields = (uint32_t)(g_entropy_sample % 3u);
    SCENARIO_CHECK(asx_lab_open_region(&g_lab, &c.root) == ASX_OK, "lab root open");
    c.source_region = c.root;
    c.destination_region = c.root;
    if (cross_region) {
        SCENARIO_CHECK(asx_region_open_child(c.root, &c.source_region) == ASX_OK, "source child");
        SCENARIO_CHECK(asx_region_open_child(c.root, &c.destination_region) == ASX_OK,
                       "destination child");
    }
    SCENARIO_CHECK(asx_region_set_limits(c.source_region, &limits) == ASX_OK, "source quota");
    SCENARIO_CHECK(asx_region_set_limits(c.destination_region, &limits) == ASX_OK,
                   "destination quota");
    if (kind == HOLDER_HANDOFF_FULL) {
        c.blocker_status = ASX_E_INVALID_STATE; /* until the blocker aborts its credit */
        SCENARIO_CHECK(asx_task_spawn(c.destination_region, poll_holder_blocker, &c,
                                      &c.blocker_task) == ASX_OK,
                       "spawn destination quota holder");
        SCENARIO_CHECK(asx_obligation_reserve_ex(c.destination_region, ASX_OBLIGATION_KIND_GENERIC,
                                                 c.blocker_task, &c.blocker) == ASX_OK,
                       "fill destination obligation quota");
    }
    if ((g_entropy_sample & 1u) != 0u) {
        SCENARIO_CHECK(asx_task_spawn(c.destination_region, poll_holder_destination, &c,
                                      &c.destination) == ASX_OK,
                       "destination spawn first");
        SCENARIO_CHECK(asx_task_spawn(c.source_region, poll_holder_source, &c, &c.source) == ASX_OK,
                       "source spawn second");
    } else {
        SCENARIO_CHECK(asx_task_spawn(c.source_region, poll_holder_source, &c, &c.source) == ASX_OK,
                       "source spawn first");
        SCENARIO_CHECK(asx_task_spawn(c.destination_region, poll_holder_destination, &c,
                                      &c.destination) == ASX_OK,
                       "destination spawn second");
    }
    budget = asx_budget_from_polls(cfg.max_polls);
    SCENARIO_CHECK(asx_lab_run(&g_lab, c.root, &budget) == ASX_E_WOULD_BLOCK && c.ready,
                   "real source and destination should both park");
    SCENARIO_CHECK(asx_obligation_get_info(c.obligation, &info) == ASX_OK &&
                       info.state == ASX_OBLIGATION_RESERVED &&
                       asx_handle_index(info.holder) == asx_handle_index(c.source) &&
                       info.region == c.source_region,
                   "source should own the checked credit");
    leaks = asx_obligation_leak_count(&g_lab.rt);
    if (kind == HOLDER_RAW_GUARD) {
        c.destination_action = 1;
        SCENARIO_CHECK(asx_task_wake(c.destination) == ASX_OK, "wake wrong live caller");
        budget = asx_budget_from_polls(cfg.max_polls);
        SCENARIO_CHECK(asx_lab_run(&g_lab, c.root, &budget) == ASX_E_WOULD_BLOCK && !c.failed,
                       "wrong live caller must leave source credit unchanged");
    }
    c.source_action = 1;
    SCENARIO_CHECK(asx_task_wake(c.source) == ASX_OK, "wake actual holder");
    budget = asx_budget_from_polls(cfg.max_polls);
    SCENARIO_CHECK(asx_lab_run(&g_lab, c.root, &budget) ==
                           (kind == HOLDER_RAW_GUARD ? ASX_OK : ASX_E_WOULD_BLOCK) &&
                       !c.failed,
                   "actual holder action or old holder denial failed");
    if (kind == HOLDER_HANDOFF_FULL) {
        SCENARIO_CHECK(asx_region_drain_progress(c.source_region, &progress) == ASX_OK &&
                           progress.obligations_reserved == 1u,
                       "refusal must retain source quota credit");
        SCENARIO_CHECK(asx_region_drain_progress(c.destination_region, &progress) == ASX_OK &&
                           progress.obligations_reserved == 1u,
                       "refusal must not consume destination quota");
        c.blocker_release = 1;
        SCENARIO_CHECK(asx_task_wake(c.blocker_task) == ASX_OK, "wake destination quota holder");
        budget = asx_budget_from_polls(cfg.max_polls);
        SCENARIO_CHECK(asx_lab_run(&g_lab, c.root, &budget) == ASX_E_WOULD_BLOCK && !c.failed,
                       "quota holder run failed");
        SCENARIO_CHECK(c.blocker_status == ASX_OK, "free destination quota");
        c.source_action = 2;
        SCENARIO_CHECK(asx_task_wake(c.source) == ASX_OK, "retry original holder after refusal");
        budget = asx_budget_from_polls(cfg.max_polls);
        SCENARIO_CHECK(asx_lab_run(&g_lab, c.root, &budget) == ASX_E_WOULD_BLOCK && !c.failed,
                       "same original credit must hand off after quota becomes available");
    }
    if (kind != HOLDER_RAW_GUARD) {
        SCENARIO_CHECK(asx_obligation_get_info(c.obligation, &info) == ASX_OK &&
                           info.state == ASX_OBLIGATION_RESERVED &&
                           asx_handle_index(info.holder) == asx_handle_index(c.destination) &&
                           info.region == c.destination_region,
                       "same obligation must retain new holder and destination region");
        SCENARIO_CHECK(obligation_events(c.obligation, ASX_TRACE_OBLIGATION_HANDOFF) == 1u,
                       "exactly one handoff event required");
        c.destination_action = 1;
        if (kind == HOLDER_HANDOFF_CANCEL) {
            SCENARIO_CHECK(asx_task_cancel(c.destination, ASX_CANCEL_USER) == ASX_OK,
                           "cancel new holder");
        }
        SCENARIO_CHECK(asx_task_wake(c.destination) == ASX_OK, "wake new holder");
        budget = asx_budget_from_polls(cfg.max_polls);
        SCENARIO_CHECK(asx_lab_run(&g_lab, c.root, &budget) == ASX_OK && !c.failed,
                       "new holder must settle and finish");
    }
    SCENARIO_CHECK(asx_obligation_get_info(c.obligation, &info) == ASX_OK &&
                       info.state == (kind == HOLDER_HANDOFF_CANCEL ? ASX_OBLIGATION_ABORTED
                                                                    : ASX_OBLIGATION_COMMITTED),
                   "final obligation state");
    SCENARIO_CHECK(asx_obligation_leak_count(&g_lab.rt) == leaks, "ownership work must not leak");
    SCENARIO_CHECK(obligation_events(c.obligation, ASX_TRACE_OBLIGATION_COMMIT) ==
                           (kind == HOLDER_HANDOFF_CANCEL ? 0u : 1u) &&
                       obligation_events(c.obligation, ASX_TRACE_OBLIGATION_ABORT) ==
                           (kind == HOLDER_HANDOFF_CANCEL ? 1u : 0u),
                   "exactly one resolution required");
    if (kind == HOLDER_HANDOFF_CANCEL) {
        SCENARIO_CHECK(info.abort_reason == ASX_OBLIGATION_ABORT_CANCEL, "cancel abort reason");
        SCENARIO_CHECK(asx_task_get_outcome(c.destination, &outcome) == ASX_OK &&
                           outcome.severity == ASX_OUTCOME_CANCELLED,
                       "cancelled destination task outcome");
    }
    budget = asx_budget_from_polls(cfg.max_polls);
    SCENARIO_CHECK(asx_region_drain(c.root, &budget) == ASX_OK, "drain after ownership cleanup");
    SCENARIO_CHECK(asx_quiescence_check(c.root) == ASX_OK, "ownership subtree quiescent");
    SCENARIO_END();
    asx_lab_shutdown(&g_lab);
}

/* -------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------- */

int main(void) {
    const char *seed = getenv("ASX_E2E_SEED");
    const char *run_id = getenv("ASX_E2E_RUN_ID");
    if (run_id != NULL && run_id[0] != '\0') g_run_id = run_id;
    if (seed != NULL) {
        char *end;
        unsigned long long parsed;
        errno = 0;
        parsed = strtoull(seed, &end, 10);
        if (seed[0] < '0' || seed[0] > '9' || end == seed || *end != '\0' || errno == ERANGE ||
            parsed > UINT64_MAX) {
            fprintf(stderr, "[e2e] invalid ASX_E2E_SEED: %s\n", seed);
            return 2;
        }
        g_seed = (uint64_t)parsed;
    }
    scenario_region_open_close();
    scenario_task_spawn_complete();
    scenario_task_coroutine_yields();
    scenario_obligation_reserve_commit();
    scenario_obligation_reserve_abort();
    scenario_cancel_checkpoint_finalize();
    scenario_cancel_propagate_region();
    scenario_quiescence_empty_region();
    scenario_quiescence_after_drain();
    scenario_timer_register_expire();
    scenario_timer_cancel_before_expire();
    scenario_timer_deterministic_order();
    scenario_channel_reserve_send_recv();
    scenario_channel_reserve_abort();
    scenario_channel_fifo_order();
    scenario_channel_close_sender();
    scenario_trace_digest_deterministic();
    scenario_region_poison_containment();
    scenario_drain_progress_snapshot();
    scenario_quiescence_evidence();
    scenario_real_holders("obligation.real_holder_guard", HOLDER_RAW_GUARD);
    scenario_real_holders("obligation.handoff_same_region", HOLDER_HANDOFF_SAME);
    scenario_real_holders("obligation.handoff_cross_region", HOLDER_HANDOFF_CROSS);
    scenario_real_holders("obligation.handoff_full_destination", HOLDER_HANDOFF_FULL);
    scenario_real_holders("obligation.handoff_cancel_cleanup", HOLDER_HANDOFF_CANCEL);

    fprintf(stderr, "[e2e] core_lifecycle: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
