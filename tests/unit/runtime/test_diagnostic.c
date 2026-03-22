/*
 * test_diagnostic.c — unit tests for diagnostic context, inspection, and
 *                     evidence sink
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/core/budget.h>
#include <asx/runtime/blocking.h>
#include <asx/runtime/browser_boundary.h>
#include <asx/runtime/diagnostic.h>
#include <asx/runtime/io_driver.h>
#include <asx/runtime/rt.h>
#include <asx/runtime/runtime.h>
#include <asx/runtime/trace.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Test harness                                                        */
/* ------------------------------------------------------------------ */

static int g_pass, g_fail;
static asx_status st_sink_;
#define MUST_OK(expr)                                                                              \
    do {                                                                                           \
        st_sink_ = (expr);                                                                         \
        (void)st_sink_;                                                                            \
    } while (0)

#define ASSERT(cond, msg)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("  FAIL: %s (line %d)\n", msg, __LINE__);                                       \
            g_fail++;                                                                              \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define RUN(fn)                                                                                    \
    do {                                                                                           \
        printf("  " #fn "...\n");                                                                  \
        asx_runtime_reset();                                                                       \
        fn();                                                                                      \
        g_pass++;                                                                                  \
    } while (0)

/* ================================================================== */
/* Diagnostic context tests                                           */
/* ================================================================== */

static void test_diag_push_pop(void) {
    asx_diagnostic_ctx ctx;
    const asx_diagnostic_ctx *cur;

    ASSERT(asx_diagnostic_current() == NULL, "empty stack returns null");

    memset(&ctx, 0, sizeof(ctx));
    ctx.operation = "test_op";
    ctx.task = 42;

    MUST_OK(asx_diagnostic_push(&ctx));
    cur = asx_diagnostic_current();
    ASSERT(cur != NULL, "current not null after push");
    ASSERT(cur->task == 42, "task id preserved");
    ASSERT(strcmp(cur->operation, "test_op") == 0, "operation preserved");
    ASSERT(cur->depth == 0, "depth is 0 for first push");

    asx_diagnostic_pop();
    ASSERT(asx_diagnostic_current() == NULL, "null after pop");
}

static void test_diag_nesting(void) {
    asx_diagnostic_ctx outer, inner;
    const asx_diagnostic_ctx *cur;

    memset(&outer, 0, sizeof(outer));
    outer.operation = "outer";
    memset(&inner, 0, sizeof(inner));
    inner.operation = "inner";

    MUST_OK(asx_diagnostic_push(&outer));
    MUST_OK(asx_diagnostic_push(&inner));

    cur = asx_diagnostic_current();
    ASSERT(cur != NULL, "inner context active");
    ASSERT(strcmp(cur->operation, "inner") == 0, "inner op");
    ASSERT(cur->depth == 1, "depth is 1 for nested push");

    asx_diagnostic_pop();
    cur = asx_diagnostic_current();
    ASSERT(cur != NULL, "outer context after pop");
    ASSERT(strcmp(cur->operation, "outer") == 0, "outer op");
    ASSERT(cur->depth == 0, "depth is 0 for outer");

    asx_diagnostic_pop();
    ASSERT(asx_diagnostic_current() == NULL, "empty after all pops");
}

static void test_diag_stack_exhaustion(void) {
    asx_diagnostic_ctx ctx;
    uint32_t i;
    asx_status st;

    memset(&ctx, 0, sizeof(ctx));
    ctx.operation = "fill";

    /* Push until full (8 deep) */
    for (i = 0; i < 8; i++) {
        st = asx_diagnostic_push(&ctx);
        ASSERT(st == ASX_OK, "push within capacity");
    }

    st = asx_diagnostic_push(&ctx);
    ASSERT(st == ASX_E_RESOURCE_EXHAUSTED, "push when full");

    /* Pop all */
    for (i = 0; i < 8; i++) { asx_diagnostic_pop(); }
    ASSERT(asx_diagnostic_current() == NULL, "empty after pop all");
}

static void test_diag_null_arg(void) {
    ASSERT(asx_diagnostic_push(NULL) == ASX_E_INVALID_ARGUMENT, "null push");
}

static void test_diag_pop_empty(void) {
    /* Pop on empty stack should not crash */
    asx_diagnostic_pop();
    ASSERT(asx_diagnostic_current() == NULL, "still null");
}

static void test_diag_reset(void) {
    asx_diagnostic_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.operation = "pre_reset";

    MUST_OK(asx_diagnostic_push(&ctx));
    ASSERT(asx_diagnostic_current() != NULL, "non-null before reset");

    asx_diagnostic_reset();
    ASSERT(asx_diagnostic_current() == NULL, "null after reset");
}

/* ================================================================== */
/* Runtime inspection tests                                           */
/* ================================================================== */

static void test_inspect_initialized(void) {
    asx_runtime rt;
    asx_inspection_report rpt;

    MUST_OK(asx_runtime_init_default(&rt));
    MUST_OK(asx_inspect(&rt, &rpt));

    ASSERT(rpt.initialized, "initialized");
    ASSERT(rpt.regions.capacity == ASX_MAX_REGIONS, "region capacity");
    ASSERT(rpt.tasks.capacity == ASX_MAX_TASKS, "task capacity");
    ASSERT(rpt.obligations.capacity == ASX_MAX_OBLIGATIONS, "obligation capacity");
    ASSERT(rpt.io_driver.capacity == ASX_MAX_IO_TOKENS, "io capacity");
    ASSERT(rpt.blocking.capacity == ASX_MAX_BLOCKING_TASKS, "blocking capacity");
    ASSERT(rpt.trace.capacity == ASX_TRACE_CAPACITY, "trace capacity");
    ASSERT(!rpt.any_poisoned, "no poisoned regions");
    ASSERT(rpt.io_driver.active_count == 0, "io starts empty");
    ASSERT(rpt.blocking.active_count == 0, "blocking starts empty");

    asx_runtime_shutdown(&rt);
}

static void test_inspect_with_entities(void) {
    asx_runtime rt;
    asx_inspection_report rpt;
    asx_region_id region;
    asx_waker waker;
    asx_io_token tok;

    MUST_OK(asx_runtime_init_default(&rt));
    MUST_OK(asx_region_open(&region));
    if (asx_surface_available_active(ASX_SURFACE_IO_DRIVER)) {
        MUST_OK(asx_waker_register(55, &waker));
        MUST_OK(asx_io_register(55, ASX_IO_READABLE, &waker, &tok));
    }

    MUST_OK(asx_inspect(&rt, &rpt));
    /* At least one region alive (the one we opened + possibly the runtime's) */
    ASSERT(rpt.regions.active_count >= 1, "at least 1 active region");
    ASSERT(rpt.trace.event_count > 0, "trace events recorded");
    if (asx_surface_available_active(ASX_SURFACE_IO_DRIVER)) {
        ASSERT(rpt.io_driver.active_count == 1, "io registration counted");
        asx_io_deregister(&tok);
    } else {
        ASSERT(rpt.io_driver.active_count == 0, "io inactive when unsupported");
    }

    asx_runtime_shutdown(&rt);
}

static void test_inspect_null_args(void) {
    asx_runtime rt;
    asx_inspection_report rpt;

    ASSERT(asx_inspect(NULL, &rpt) == ASX_E_INVALID_ARGUMENT, "null rt");
    ASSERT(asx_inspect(&rt, NULL) == ASX_E_INVALID_ARGUMENT, "null out");
}

static void test_inspect_uninitialized(void) {
    asx_runtime rt;
    asx_inspection_report rpt;

    memset(&rt, 0, sizeof(rt));
    MUST_OK(asx_inspect(&rt, &rpt));
    ASSERT(!rpt.initialized, "not initialized");
    ASSERT(rpt.regions.active_count == 0, "regions zero uninitialized");
    ASSERT(rpt.tasks.active_count == 0, "tasks zero uninitialized");
    ASSERT(rpt.obligations.active_count == 0, "obligations zero uninitialized");
    ASSERT(rpt.io_driver.active_count == 0, "io zero uninitialized");
    ASSERT(rpt.blocking.active_count == 0, "blocking zero uninitialized");
    ASSERT(rpt.trace.event_count == 0, "trace count zero uninitialized");
    ASSERT(rpt.trace.digest == 0u, "trace digest zero uninitialized");
    ASSERT(rpt.ghosts.violation_count == 0, "ghosts zero uninitialized");
    ASSERT(rpt.telemetry_emitted == 0u, "telemetry zero uninitialized");
    ASSERT(rpt.telemetry_digest == 0u, "telemetry digest zero uninitialized");
    ASSERT(rpt.event_log_count == 0u, "event log zero uninitialized");
    ASSERT(rpt.event_log_digest == 0u, "event log digest zero uninitialized");
}

static void test_inspect_stale_runtime_fails_closed(void) {
    asx_runtime a, b;
    asx_inspection_report rpt;
    asx_region_id region;

    MUST_OK(asx_runtime_init_default(&a));
    MUST_OK(asx_region_open(&region));
    MUST_OK(asx_runtime_init_default(&b));

    MUST_OK(asx_inspect(&a, &rpt));
    ASSERT(!rpt.initialized, "stale runtime not initialized");
    ASSERT(rpt.regions.active_count == 0, "stale regions fail closed");
    ASSERT(rpt.tasks.active_count == 0, "stale tasks fail closed");
    ASSERT(rpt.obligations.active_count == 0, "stale obligations fail closed");
    ASSERT(rpt.io_driver.active_count == 0, "stale io fail closed");
    ASSERT(rpt.blocking.active_count == 0, "stale blocking fail closed");
    ASSERT(rpt.trace.event_count == 0, "stale trace count fail closed");
    ASSERT(rpt.trace.digest == 0u, "stale trace digest fail closed");
    ASSERT(rpt.ghosts.violation_count == 0, "stale ghosts fail closed");
    ASSERT(rpt.telemetry_emitted == 0u, "stale telemetry fail closed");
    ASSERT(rpt.telemetry_digest == 0u, "stale telemetry digest fail closed");
    ASSERT(rpt.event_log_count == 0u, "stale event log fail closed");
    ASSERT(rpt.event_log_digest == 0u, "stale event log digest fail closed");
    ASSERT(!rpt.any_poisoned, "stale poisoned flag fail closed");

    asx_runtime_shutdown(&a);
    asx_runtime_shutdown(&b);
}

/* ================================================================== */
/* Evidence sink tests                                                */
/* ================================================================== */

static void test_evidence_init(void) {
    asx_evidence_sink sink;
    asx_evidence_sink_init(&sink);

    ASSERT(sink.count == 0, "empty after init");
    ASSERT(sink.pass_count == 0, "no passes");
    ASSERT(sink.fail_count == 0, "no fails");
    ASSERT(asx_evidence_sink_has_room(&sink), "has room");
    ASSERT(asx_evidence_verdict(&sink) == ASX_EVIDENCE_PASS, "empty verdict pass");
}

static void test_evidence_record(void) {
    asx_evidence_sink sink;
    asx_evidence_sink_init(&sink);

    MUST_OK(asx_evidence_record(&sink, "test:a", ASX_EVIDENCE_PASS, "all good", 0));
    MUST_OK(asx_evidence_record(&sink, "test:b", ASX_EVIDENCE_WARN, "something off", 123));
    MUST_OK(asx_evidence_record(&sink, "test:c", ASX_EVIDENCE_INFO, "note", 0));

    ASSERT(sink.count == 3, "3 entries");
    ASSERT(sink.pass_count == 1, "1 pass");
    ASSERT(sink.warn_count == 1, "1 warn");
    ASSERT(sink.info_count == 1, "1 info");
    ASSERT(sink.fail_count == 0, "0 fails");

    /* Check entry fields */
    ASSERT(strcmp(sink.entries[0].source, "test:a") == 0, "source a");
    ASSERT(sink.entries[0].level == ASX_EVIDENCE_PASS, "level a");
    ASSERT(sink.entries[0].sequence == 0, "seq 0");
    ASSERT(sink.entries[1].sequence == 1, "seq 1");
    ASSERT(sink.entries[1].entity_id == 123, "entity b");
}

static void test_evidence_verdict(void) {
    asx_evidence_sink sink;
    asx_evidence_sink_init(&sink);

    /* Pass only → PASS */
    MUST_OK(asx_evidence_record(&sink, "a", ASX_EVIDENCE_PASS, "ok", 0));
    ASSERT(asx_evidence_verdict(&sink) == ASX_EVIDENCE_PASS, "pass verdict");

    /* Add warn → WARN */
    MUST_OK(asx_evidence_record(&sink, "b", ASX_EVIDENCE_WARN, "hmm", 0));
    ASSERT(asx_evidence_verdict(&sink) == ASX_EVIDENCE_WARN, "warn verdict");

    /* Add fail → FAIL */
    MUST_OK(asx_evidence_record(&sink, "c", ASX_EVIDENCE_FAIL, "bad", 0));
    ASSERT(asx_evidence_verdict(&sink) == ASX_EVIDENCE_FAIL, "fail verdict");
}

static void test_evidence_exhaustion(void) {
    asx_evidence_sink sink;
    uint32_t i;
    asx_status st;
    asx_evidence_sink_init(&sink);

    for (i = 0; i < ASX_EVIDENCE_SINK_CAPACITY; i++) {
        st = asx_evidence_record(&sink, "fill", ASX_EVIDENCE_INFO, "x", 0);
        ASSERT(st == ASX_OK, "record within capacity");
    }

    ASSERT(!asx_evidence_sink_has_room(&sink), "no room");
    st = asx_evidence_record(&sink, "over", ASX_EVIDENCE_INFO, "y", 0);
    ASSERT(st == ASX_E_RESOURCE_EXHAUSTED, "exhausted");
}

static void test_evidence_null_args(void) {
    asx_evidence_sink sink;
    ASSERT(asx_evidence_record(NULL, "a", ASX_EVIDENCE_INFO, "x", 0) == ASX_E_INVALID_ARGUMENT,
           "null sink");
    ASSERT(asx_evidence_verdict(NULL) == ASX_EVIDENCE_FAIL, "null verdict");
    ASSERT(!asx_evidence_sink_has_room(NULL), "null no room");

    /* init/reset with NULL should not crash */
    asx_evidence_sink_init(NULL);
    asx_evidence_sink_reset(NULL);
    (void)sink;
}

static void test_evidence_rejects_invalid_level(void) {
    asx_evidence_sink sink;

    asx_evidence_sink_init(&sink);
    ASSERT(asx_evidence_record(&sink, "bad", (asx_evidence_level)99, "x", 0) ==
               ASX_E_INVALID_ARGUMENT,
           "invalid level rejected");
    ASSERT(sink.count == 0, "invalid level does not append");
    ASSERT(sink.pass_count == 0, "invalid level does not change counters");
    ASSERT(sink.warn_count == 0, "invalid level does not change counters");
    ASSERT(sink.fail_count == 0, "invalid level does not change counters");
    ASSERT(sink.info_count == 0, "invalid level does not change counters");
}

static void test_evidence_reset(void) {
    asx_evidence_sink sink;
    asx_evidence_sink_init(&sink);

    MUST_OK(asx_evidence_record(&sink, "a", ASX_EVIDENCE_FAIL, "bad", 0));
    ASSERT(sink.count == 1, "has entry");

    asx_evidence_sink_reset(&sink);
    ASSERT(sink.count == 0, "empty after reset");
    ASSERT(sink.fail_count == 0, "no fails after reset");
}

/* ================================================================== */
/* Inspect-to-evidence pipeline tests                                 */
/* ================================================================== */

static void test_inspect_to_evidence_healthy(void) {
    asx_runtime rt;
    asx_evidence_sink sink;

    MUST_OK(asx_runtime_init_default(&rt));
    asx_evidence_sink_init(&sink);

    MUST_OK(asx_inspect_to_evidence(&rt, &sink));

    /* Should have multiple entries */
    ASSERT(sink.count >= 6, "at least 6 evidence entries");
    ASSERT(sink.fail_count == 0, "no failures in healthy runtime");
    ASSERT(asx_evidence_verdict(&sink) == ASX_EVIDENCE_PASS ||
               asx_evidence_verdict(&sink) == ASX_EVIDENCE_WARN,
           "healthy or warn verdict");

    /* First entry should be runtime check */
    ASSERT(strcmp(sink.entries[0].source, "inspect:runtime") == 0, "first entry is runtime");
    ASSERT(sink.entries[0].level == ASX_EVIDENCE_PASS, "runtime passes");
    ASSERT(strcmp(sink.entries[4].source, "inspect:io") == 0, "io entry present");
    ASSERT(strcmp(sink.entries[5].source, "inspect:blocking") == 0, "blocking entry present");

    asx_runtime_shutdown(&rt);
}

static void test_inspect_to_evidence_uninitialized(void) {
    asx_runtime rt;
    asx_evidence_sink sink;

    memset(&rt, 0, sizeof(rt));
    asx_evidence_sink_init(&sink);

    MUST_OK(asx_inspect_to_evidence(&rt, &sink));

    ASSERT(sink.fail_count > 0, "uninitialized has failures");
    ASSERT(asx_evidence_verdict(&sink) == ASX_EVIDENCE_FAIL, "fail verdict");
}

static void test_inspect_to_evidence_null_args(void) {
    asx_runtime rt;
    asx_evidence_sink sink;

    ASSERT(asx_inspect_to_evidence(NULL, &sink) == ASX_E_INVALID_ARGUMENT, "null rt");
    ASSERT(asx_inspect_to_evidence(&rt, NULL) == ASX_E_INVALID_ARGUMENT, "null sink");
}

/* ================================================================== */
/* End-to-end: diagnostic context + inspection + evidence             */
/* ================================================================== */

static void test_e2e_diagnostic_workflow(void) {
    asx_runtime rt;
    asx_diagnostic_ctx ctx;
    asx_evidence_sink sink;
    asx_inspection_report rpt;
    const asx_diagnostic_ctx *cur;

    MUST_OK(asx_runtime_init_default(&rt));
    asx_evidence_sink_init(&sink);

    /* Set diagnostic context */
    memset(&ctx, 0, sizeof(ctx));
    ctx.operation = "health_check";

    MUST_OK(asx_diagnostic_push(&ctx));
    cur = asx_diagnostic_current();
    ASSERT(cur != NULL, "context active");
    ASSERT(strcmp(cur->operation, "health_check") == 0, "operation set");

    /* Run inspection */
    MUST_OK(asx_inspect(&rt, &rpt));
    ASSERT(rpt.initialized, "rt is initialized");

    /* Pipe to evidence */
    MUST_OK(asx_inspect_to_evidence(&rt, &sink));
    ASSERT(asx_evidence_verdict(&sink) == ASX_EVIDENCE_PASS ||
               asx_evidence_verdict(&sink) == ASX_EVIDENCE_WARN,
           "healthy or warn");

    /* Pop context */
    asx_diagnostic_pop();
    ASSERT(asx_diagnostic_current() == NULL, "context cleared");

    asx_runtime_shutdown(&rt);
}

/* ================================================================== */
/* Main                                                               */
/* ================================================================== */

int main(void) {
    printf("test_diagnostic:\n");

    /* Diagnostic context */
    RUN(test_diag_push_pop);
    RUN(test_diag_nesting);
    RUN(test_diag_stack_exhaustion);
    RUN(test_diag_null_arg);
    RUN(test_diag_pop_empty);
    RUN(test_diag_reset);

    /* Runtime inspection */
    RUN(test_inspect_initialized);
    RUN(test_inspect_with_entities);
    RUN(test_inspect_null_args);
    RUN(test_inspect_uninitialized);
    RUN(test_inspect_stale_runtime_fails_closed);

    /* Evidence sink */
    RUN(test_evidence_init);
    RUN(test_evidence_record);
    RUN(test_evidence_verdict);
    RUN(test_evidence_exhaustion);
    RUN(test_evidence_null_args);
    RUN(test_evidence_rejects_invalid_level);
    RUN(test_evidence_reset);

    /* Inspect-to-evidence pipeline */
    RUN(test_inspect_to_evidence_healthy);
    RUN(test_inspect_to_evidence_uninitialized);
    RUN(test_inspect_to_evidence_null_args);

    /* E2E workflow */
    RUN(test_e2e_diagnostic_workflow);

    printf("\n  %d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
