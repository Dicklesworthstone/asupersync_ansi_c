/*
 * parallel_parity_conformance_test.c -- single-vs-multi-worker parity gate
 *
 * Conformance gate: run the same semantic scenarios under worker_count
 * 1, 2, 8, and 64 where supported. The canonical digest and structured
 * trace/event summaries must match the single-worker baseline.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../test_harness.h"
#include <asx/asx.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PARITY_EVENT_BUCKETS 0x43u
#define PARITY_MAX_ORDER 8u
#define PARITY_MAX_OUTPUTS 8u

typedef enum {
    PARITY_SCENARIO_LIFECYCLE = 0,
    PARITY_SCENARIO_CANCEL = 1,
    PARITY_SCENARIO_TIMER_WAKER = 2,
    PARITY_SCENARIO_MPSC = 3,
    PARITY_SCENARIO_BLOCKING = 4,
    PARITY_SCENARIO_REACTOR = 5
} parity_scenario;

typedef struct {
    parity_scenario scenario;
    const char *scenario_id;
    uint32_t worker_count;
    int skipped;
    asx_status status;
    uint64_t semantic_digest;
    uint64_t trace_digest;
    uint64_t event_order_digest;
    uint32_t event_count;
    uint32_t event_counts[PARITY_EVENT_BUCKETS];
    uint32_t completed_tasks;
    uint32_t queue_len;
    uint32_t reserved_count;
    uint32_t blocking_active;
    uint32_t reactor_ready;
    uint32_t waker_ready;
    uint32_t timed_promotions;
    uint32_t pressure_pct;
    uint32_t max_worker_queue_depth;
    uint32_t steals_failed;
    uint32_t timed_wake_latency_rounds_max;
    uint32_t admission_triggered;
    uint32_t admission_pressure_pct;
    asx_status admission_status;
    uint32_t commit_sequence;
    uint32_t total_worker_commits;
    uint32_t max_worker_commit_sequence;
    uint32_t commit_authority_drift;
    uint32_t native_live_enabled;
    asx_status native_live_status;
    uint32_t order_count;
    uint32_t order_values[PARITY_MAX_ORDER];
    uint32_t output_count;
    uint64_t outputs[PARITY_MAX_OUTPUTS];
} parity_result;

static uint32_t g_order_count;
static uint32_t g_order_values[PARITY_MAX_ORDER];
static uint32_t g_ghost_reactor_ready;

static const uint32_t WORKER_COUNTS[] = {1u, 2u, 8u, ASX_PARALLEL_GENERIC_TARGET_WORKERS};
static const parity_scenario SCENARIOS[] = {PARITY_SCENARIO_LIFECYCLE,   PARITY_SCENARIO_CANCEL,
                                            PARITY_SCENARIO_TIMER_WAKER, PARITY_SCENARIO_MPSC,
                                            PARITY_SCENARIO_BLOCKING,    PARITY_SCENARIO_REACTOR};

static const char *active_profile_name(void) {
#if defined(ASX_PROFILE_CORE)
    return "ASX_PROFILE_CORE";
#elif defined(ASX_PROFILE_POSIX)
    return "ASX_PROFILE_POSIX";
#elif defined(ASX_PROFILE_WIN32)
    return "ASX_PROFILE_WIN32";
#elif defined(ASX_PROFILE_FREESTANDING)
    return "ASX_PROFILE_FREESTANDING";
#elif defined(ASX_PROFILE_EMBEDDED_ROUTER)
    return "ASX_PROFILE_EMBEDDED_ROUTER";
#elif defined(ASX_PROFILE_HFT)
    return "ASX_PROFILE_HFT";
#elif defined(ASX_PROFILE_AUTOMOTIVE)
    return "ASX_PROFILE_AUTOMOTIVE";
#elif defined(ASX_PROFILE_PARALLEL)
    return "ASX_PROFILE_PARALLEL";
#elif defined(ASX_PROFILE_BROWSER)
    return "ASX_PROFILE_BROWSER";
#else
    return "ASX_PROFILE_UNKNOWN";
#endif
}

static const char *scenario_id(parity_scenario scenario) {
    switch (scenario) {
    case PARITY_SCENARIO_LIFECYCLE: return "parallel.lifecycle.yield-complete";
    case PARITY_SCENARIO_CANCEL: return "parallel.cancel.checkpoint-complete";
    case PARITY_SCENARIO_TIMER_WAKER: return "parallel.timer.equal-deadline-wakers";
    case PARITY_SCENARIO_MPSC: return "parallel.channel.mpsc-two-phase";
    case PARITY_SCENARIO_BLOCKING: return "parallel.blocking.spawn-wake";
    case PARITY_SCENARIO_REACTOR: return "parallel.reactor.ghost-readiness";
    }
    return "parallel.unknown";
}

static void reset_order(void) {
    uint32_t i;

    g_order_count = 0u;
    for (i = 0u; i < PARITY_MAX_ORDER; i++) { g_order_values[i] = 0u; }
}

static void record_order(uint32_t value) {
    if (g_order_count < PARITY_MAX_ORDER) {
        g_order_values[g_order_count] = value;
        g_order_count++;
    }
}

static void reset_all(void) {
    asx_runtime_reset();
    asx_channel_reset();
    asx_ghost_reset();
    asx_parallel_reset();
    asx_waker_reset();
    asx_timer_wheel_reset(asx_timer_wheel_global());
#if ASX_HAS_NATIVE_IO_DRIVER
    asx_io_driver_reset();
#endif
#if ASX_HAS_BLOCKING_SURFACE
    asx_blocking_pool_reset();
#endif
    reset_order();
    g_ghost_reactor_ready = 0u;
}

static asx_parallel_config default_config(uint32_t worker_count) {
    asx_parallel_config cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.worker_count = worker_count;
    cfg.fairness = ASX_FAIRNESS_ROUND_ROBIN;
    cfg.lane_weights[0] = 1u;
    cfg.lane_weights[1] = 1u;
    cfg.lane_weights[2] = 1u;
    cfg.starvation_limit = 5u;
    asx_parallel_admission_policy_init(&cfg.admission_policy);
    asx_parallel_locality_config_init(&cfg.locality);
    return cfg;
}

static asx_status poll_yield_n(void *data, asx_task_id self) {
    int *counter = (int *)data;
    (void)self;

    if (counter != NULL && *counter > 0) {
        (*counter)--;
        return ASX_E_PENDING;
    }
    return ASX_OK;
}

static asx_status poll_checkpoint_then_complete(void *data, asx_task_id self) {
    asx_checkpoint_result cr;
    (void)data;

    if (asx_checkpoint(self, &cr) == ASX_OK && cr.cancelled) { return ASX_OK; }
    return ASX_E_PENDING;
}

static asx_status poll_record_order(void *data, asx_task_id self) {
    uint32_t *value = (uint32_t *)data;
    (void)self;

    if (value != NULL) { record_order(*value); }
    return ASX_OK;
}

static uint64_t blocking_checksum_job(void *data) {
    uint64_t *value = (uint64_t *)data;

    if (value == NULL) { return UINT64_C(0); }
    return (*value * UINT64_C(17)) + UINT64_C(3);
}

static asx_status ghost_reactor_ready(void *ctx, uint64_t logical_step, uint32_t *ready_count) {
    (void)ctx;
    (void)logical_step;

    if (ready_count == NULL) { return ASX_E_INVALID_ARGUMENT; }
    *ready_count = g_ghost_reactor_ready;
    return ASX_OK;
}

static uint64_t digest_mix_byte(uint64_t digest, uint8_t byte) {
    digest ^= (uint64_t)byte;
    digest *= UINT64_C(1099511628211);
    return digest;
}

static uint64_t digest_mix_u32(uint64_t digest, uint32_t value) {
    uint32_t i;

    for (i = 0u; i < 4u; i++) {
        digest = digest_mix_byte(digest, (uint8_t)((value >> (i * 8u)) & 0xffu));
    }
    return digest;
}

static uint64_t digest_mix_u64(uint64_t digest, uint64_t value) {
    uint32_t i;

    for (i = 0u; i < 8u; i++) {
        digest = digest_mix_byte(digest, (uint8_t)((value >> (i * 8u)) & UINT64_C(0xff)));
    }
    return digest;
}

static uint64_t digest_mix_str(uint64_t digest, const char *value) {
    const unsigned char *p;

    if (value == NULL) { return digest_mix_byte(digest, 0u); }
    p = (const unsigned char *)value;
    while (*p != '\0') {
        digest = digest_mix_byte(digest, *p);
        p++;
    }
    return digest_mix_byte(digest, 0u);
}

static void capture_trace_summary(parity_result *out) {
    uint32_t i;
    uint64_t order_digest = UINT64_C(1469598103934665603);

    out->event_count = asx_trace_event_count();
    out->trace_digest = asx_trace_digest();
    for (i = 0u; i < out->event_count; i++) {
        asx_trace_event ev;
        uint32_t kind;

        if (!asx_trace_event_get(i, &ev)) { continue; }
        kind = (uint32_t)ev.kind;
        if (kind < PARITY_EVENT_BUCKETS) { out->event_counts[kind]++; }
        order_digest = digest_mix_u32(order_digest, kind);
    }
    out->event_order_digest = order_digest;
}

static void capture_worker_summary(parity_result *out) {
    uint32_t i;
    uint32_t total = 0u;

    if (!asx_parallel_is_initialized()) { return; }
    for (i = 0u; i < out->worker_count; i++) {
        asx_worker_state worker;

        if (asx_worker_get_state(i, &worker) == ASX_OK) { total += worker.tasks_completed; }
    }
    out->completed_tasks = total;
}

static void capture_metric_summary(parity_result *out) {
    asx_scheduling_metrics metrics;
    asx_parallel_telemetry_snapshot snapshot;

    if (!asx_parallel_is_initialized()) { return; }
    if (asx_parallel_get_metrics(&metrics) != ASX_OK) { return; }
    out->reactor_ready = metrics.reactor_ready;
    out->waker_ready = metrics.waker_ready;
    out->timed_promotions = metrics.timed_promotions;
    out->steals_failed = metrics.steals_failed;
    out->timed_wake_latency_rounds_max = metrics.timed_wake_latency_rounds_max;

    if (asx_parallel_get_telemetry_snapshot(&snapshot) == ASX_OK) {
        out->pressure_pct = snapshot.pressure_pct;
        out->max_worker_queue_depth = snapshot.max_worker_queue_depth;
        out->admission_triggered = (uint32_t)(snapshot.admission.triggered ? 1 : 0);
        out->admission_pressure_pct = snapshot.admission.pressure_pct;
        out->admission_status = snapshot.admission.admit_status;
        out->commit_sequence = snapshot.commit_authority.commit_sequence;
        out->total_worker_commits = snapshot.commit_authority.total_worker_commits;
        out->max_worker_commit_sequence = snapshot.commit_authority.max_worker_commit_sequence;
        out->commit_authority_drift = snapshot.commit_authority.drift_detected;
        out->native_live_enabled = snapshot.commit_authority.native_live_enabled;
        out->native_live_status = snapshot.commit_authority.native_live_status;
    }
}

static void capture_order_summary(parity_result *out) {
    uint32_t i;

    out->order_count = g_order_count;
    for (i = 0u; i < g_order_count && i < PARITY_MAX_ORDER; i++) {
        out->order_values[i] = g_order_values[i];
    }
}

static void finalize_result(parity_result *out) {
    uint32_t i;
    uint64_t digest = UINT64_C(1469598103934665603);

    capture_trace_summary(out);
    capture_worker_summary(out);
    capture_metric_summary(out);
    capture_order_summary(out);

    digest = digest_mix_str(digest, out->scenario_id);
    digest = digest_mix_u32(digest, (uint32_t)out->status);
    digest = digest_mix_u64(digest, out->event_order_digest);
    digest = digest_mix_u32(digest, out->event_count);
    for (i = 0u; i < PARITY_EVENT_BUCKETS; i++) {
        digest = digest_mix_u32(digest, out->event_counts[i]);
    }
    digest = digest_mix_u32(digest, out->completed_tasks);
    digest = digest_mix_u32(digest, out->queue_len);
    digest = digest_mix_u32(digest, out->reserved_count);
    digest = digest_mix_u32(digest, out->blocking_active);
    digest = digest_mix_u32(digest, out->reactor_ready);
    digest = digest_mix_u32(digest, out->waker_ready);
    digest = digest_mix_u32(digest, out->timed_promotions);
    digest = digest_mix_u32(digest, out->commit_sequence);
    digest = digest_mix_u32(digest, out->total_worker_commits);
    digest = digest_mix_u32(digest, out->commit_authority_drift);
    digest = digest_mix_u32(digest, out->order_count);
    for (i = 0u; i < PARITY_MAX_ORDER; i++) {
        digest = digest_mix_u32(digest, out->order_values[i]);
    }
    digest = digest_mix_u32(digest, out->output_count);
    for (i = 0u; i < PARITY_MAX_OUTPUTS; i++) { digest = digest_mix_u64(digest, out->outputs[i]); }
    out->semantic_digest = digest;
}

static void append_output(parity_result *out, uint64_t value) {
    if (out->output_count < PARITY_MAX_OUTPUTS) {
        out->outputs[out->output_count] = value;
        out->output_count++;
    }
}

static asx_status run_lifecycle(uint32_t worker_count, parity_result *out) {
    asx_parallel_config cfg = default_config(worker_count);
    asx_region_id rid;
    asx_task_id t1, t2, t3;
    asx_budget budget;
    int c1 = 2;
    int c2 = 1;
    int c3 = 0;
    asx_status st;

    st = asx_parallel_init(&cfg);
    if (st != ASX_OK) { return st; }
    st = asx_region_open(&rid);
    if (st != ASX_OK) { return st; }
    st = asx_task_spawn(rid, poll_yield_n, &c1, &t1);
    if (st != ASX_OK) { return st; }
    st = asx_task_spawn(rid, poll_yield_n, &c2, &t2);
    if (st != ASX_OK) { return st; }
    st = asx_task_spawn(rid, poll_yield_n, &c3, &t3);
    if (st != ASX_OK) { return st; }

    budget = asx_budget_from_polls(100u);
    out->status = asx_parallel_run(rid, &budget);
    append_output(out, (uint64_t)c1);
    append_output(out, (uint64_t)c2);
    append_output(out, (uint64_t)c3);
    return ASX_OK;
}

static asx_status run_cancel(uint32_t worker_count, parity_result *out) {
    asx_parallel_config cfg = default_config(worker_count);
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    asx_outcome outcome;
    asx_cancel_phase phase;
    asx_status st;

    st = asx_parallel_init(&cfg);
    if (st != ASX_OK) { return st; }
    st = asx_region_open(&rid);
    if (st != ASX_OK) { return st; }
    st = asx_task_spawn(rid, poll_checkpoint_then_complete, NULL, &tid);
    if (st != ASX_OK) { return st; }

    budget = asx_budget_from_polls(1u);
    st = asx_parallel_run(rid, &budget);
    if (st != ASX_E_POLL_BUDGET_EXHAUSTED) { return st; }

    st = asx_task_cancel(tid, ASX_CANCEL_USER);
    if (st != ASX_OK) { return st; }
    budget = asx_budget_from_polls(20u);
    out->status = asx_parallel_run(rid, &budget);
    if (out->status != ASX_OK) { return ASX_OK; }

    st = asx_task_get_outcome(tid, &outcome);
    if (st != ASX_OK) { return st; }
    st = asx_task_get_cancel_phase(tid, &phase);
    if (st != ASX_OK) { return st; }
    append_output(out, (uint64_t)outcome.severity);
    append_output(out, (uint64_t)phase);
    return ASX_OK;
}

static asx_status run_timer_waker(uint32_t worker_count, parity_result *out) {
    asx_parallel_config cfg = default_config(worker_count);
    asx_region_id rid;
    asx_task_id t1, t2;
    asx_waker w1, w2;
    asx_timer_handle h1, h2;
    void *ready_wakers[2];
    asx_budget budget;
    uint32_t s1 = 1u;
    uint32_t s2 = 2u;
    uint32_t ready_count;
    uint32_t i;
    asx_status st;

    st = asx_parallel_init(&cfg);
    if (st != ASX_OK) { return st; }
    st = asx_region_open(&rid);
    if (st != ASX_OK) { return st; }
    st = asx_task_spawn(rid, poll_record_order, &s1, &t1);
    if (st != ASX_OK) { return st; }
    st = asx_task_spawn(rid, poll_record_order, &s2, &t2);
    if (st != ASX_OK) { return st; }
    st = asx_waker_register(t1, &w1);
    if (st != ASX_OK) { return st; }
    st = asx_waker_register(t2, &w2);
    if (st != ASX_OK) { return st; }
    st = asx_inject_timed(t1);
    if (st != ASX_OK) { return st; }
    st = asx_inject_timed(t2);
    if (st != ASX_OK) { return st; }
    st = asx_timer_register(asx_timer_wheel_global(), 100u, &w1, &h1);
    if (st != ASX_OK) { return st; }
    st = asx_timer_register(asx_timer_wheel_global(), 100u, &w2, &h2);
    if (st != ASX_OK) { return st; }

    ready_count = asx_timer_collect_expired(asx_timer_wheel_global(), 100u, ready_wakers, 2u);
    if (ready_count != 2u) { return ASX_E_INVALID_STATE; }
    for (i = 0u; i < ready_count; i++) {
        st = asx_waker_wake((const asx_waker *)ready_wakers[i]);
        if (st != ASX_OK) { return st; }
    }

    budget = asx_budget_from_polls(20u);
    out->status = asx_parallel_run(rid, &budget);
    append_output(out, ready_count);
    return ASX_OK;
}

static asx_status run_mpsc(uint32_t worker_count, parity_result *out) {
    static const uint64_t VALUES[] = {UINT64_C(11), UINT64_C(22), UINT64_C(33), UINT64_C(44)};
    asx_parallel_config cfg = default_config(worker_count);
    asx_region_id rid;
    asx_channel_id channel;
    asx_send_permit permit;
    asx_task_id t1, t2;
    asx_budget budget;
    int c1 = 1;
    int c2 = 1;
    uint64_t value;
    uint32_t i;
    asx_status st;

    st = asx_parallel_init(&cfg);
    if (st != ASX_OK) { return st; }
    st = asx_region_open(&rid);
    if (st != ASX_OK) { return st; }
    st = asx_channel_create(rid, 5u, &channel);
    if (st != ASX_OK) { return st; }

    for (i = 0u; i < (uint32_t)(sizeof(VALUES) / sizeof(VALUES[0])); i++) {
        st = asx_channel_try_reserve(channel, &permit);
        if (st != ASX_OK) { return st; }
        st = asx_send_permit_send(&permit, VALUES[i]);
        if (st != ASX_OK) { return st; }
    }

    st = asx_channel_try_reserve(channel, &permit);
    if (st != ASX_OK) { return st; }
    asx_send_permit_abort(&permit);

    st = asx_channel_queue_len(channel, &out->queue_len);
    if (st != ASX_OK) { return st; }
    st = asx_channel_reserved_count(channel, &out->reserved_count);
    if (st != ASX_OK) { return st; }

    for (i = 0u; i < (uint32_t)(sizeof(VALUES) / sizeof(VALUES[0])); i++) {
        st = asx_channel_try_recv(channel, &value);
        if (st != ASX_OK) { return st; }
        append_output(out, value);
    }
    st = asx_channel_close_sender(channel);
    if (st != ASX_OK) { return st; }
    st = asx_channel_try_recv(channel, &value);
    if (st != ASX_E_DISCONNECTED) { return st; }

    st = asx_task_spawn(rid, poll_yield_n, &c1, &t1);
    if (st != ASX_OK) { return st; }
    st = asx_task_spawn(rid, poll_yield_n, &c2, &t2);
    if (st != ASX_OK) { return st; }
    budget = asx_budget_from_polls(100u);
    out->status = asx_parallel_run(rid, &budget);
    append_output(out, (uint64_t)c1);
    append_output(out, (uint64_t)c2);
    return ASX_OK;
}

static asx_status run_blocking(uint32_t worker_count, parity_result *out) {
#if ASX_HAS_BLOCKING_SURFACE
    asx_parallel_config cfg = default_config(worker_count);
    asx_region_id rid;
    asx_task_id tid;
    asx_waker waker;
    asx_blocking_handle handle;
    asx_budget budget;
    uint32_t order_value = 7u;
    uint64_t input = UINT64_C(23);
    uint64_t result = 0u;
    asx_status st;

    st = asx_parallel_init(&cfg);
    if (st != ASX_OK) { return st; }
    st = asx_region_open(&rid);
    if (st != ASX_OK) { return st; }
    st = asx_task_spawn(rid, poll_record_order, &order_value, &tid);
    if (st != ASX_OK) { return st; }
    st = asx_waker_register(tid, &waker);
    if (st != ASX_OK) { return st; }
    st = asx_inject_timed(tid);
    if (st != ASX_OK) { return st; }
    st = asx_blocking_pool_init();
    if (st != ASX_OK) { return st; }
    st = asx_spawn_blocking(blocking_checksum_job, &input, &waker, &handle);
    if (st != ASX_OK) { return st; }
    st = asx_blocking_get_result(&handle, &result);
    if (st != ASX_OK) { return st; }

    budget = asx_budget_from_polls(20u);
    out->status = asx_parallel_run(rid, &budget);
    out->blocking_active = asx_blocking_active_count();
    append_output(out, result);
    append_output(out, (uint64_t)asx_blocking_get_state(&handle));
    return ASX_OK;
#else
    (void)worker_count;
    out->skipped = 1;
    out->status = ASX_E_PERMISSION_DENIED;
    return ASX_OK;
#endif
}

static asx_status run_reactor(uint32_t worker_count, parity_result *out) {
#if ASX_HAS_NATIVE_IO_DRIVER
    asx_parallel_config cfg = default_config(worker_count);
    asx_runtime_hooks hooks;
    asx_region_id rid;
    asx_task_id tid;
    asx_waker waker;
    asx_io_token token;
    asx_budget budget;
    uint32_t order_value = 9u;
    asx_status st;

    st = asx_runtime_hooks_init(&hooks);
    if (st != ASX_OK) { return st; }
    hooks.reactor.ghost_wait_fn = ghost_reactor_ready;
    hooks.deterministic_seeded_prng = 1;
    st = asx_runtime_set_hooks(&hooks);
    if (st != ASX_OK) { return st; }
    st = asx_io_driver_init();
    if (st != ASX_OK) { return st; }
    st = asx_parallel_init(&cfg);
    if (st != ASX_OK) { return st; }
    st = asx_region_open(&rid);
    if (st != ASX_OK) { return st; }
    st = asx_task_spawn(rid, poll_record_order, &order_value, &tid);
    if (st != ASX_OK) { return st; }
    st = asx_waker_register(tid, &waker);
    if (st != ASX_OK) { return st; }
    st = asx_io_register(42, ASX_IO_READABLE, &waker, &token);
    if (st != ASX_OK) { return st; }
    st = asx_inject_timed(tid);
    if (st != ASX_OK) { return st; }

    g_ghost_reactor_ready = 1u;
    budget = asx_budget_from_polls(20u);
    out->status = asx_parallel_run(rid, &budget);
    append_output(out, (uint64_t)asx_io_active_count());
    return ASX_OK;
#else
    (void)worker_count;
    out->skipped = 1;
    out->status = ASX_E_PERMISSION_DENIED;
    return ASX_OK;
#endif
}

static asx_status run_scenario(parity_scenario scenario, uint32_t worker_count,
                               parity_result *out) {
    asx_status st = ASX_OK;

    memset(out, 0, sizeof(*out));
    out->scenario = scenario;
    out->scenario_id = scenario_id(scenario);
    out->worker_count = worker_count;

    if (worker_count > ASX_MAX_WORKERS) {
        out->skipped = 1;
        out->status = ASX_E_RESOURCE_EXHAUSTED;
        finalize_result(out);
        return ASX_OK;
    }

    reset_all();
    asx_trace_reset();

    switch (scenario) {
    case PARITY_SCENARIO_LIFECYCLE: st = run_lifecycle(worker_count, out); break;
    case PARITY_SCENARIO_CANCEL: st = run_cancel(worker_count, out); break;
    case PARITY_SCENARIO_TIMER_WAKER: st = run_timer_waker(worker_count, out); break;
    case PARITY_SCENARIO_MPSC: st = run_mpsc(worker_count, out); break;
    case PARITY_SCENARIO_BLOCKING: st = run_blocking(worker_count, out); break;
    case PARITY_SCENARIO_REACTOR: st = run_reactor(worker_count, out); break;
    }

    if (st != ASX_OK && out->status == ASX_OK) { out->status = st; }
    finalize_result(out);
    return st == ASX_OK ? ASX_OK : st;
}

static const char *result_parity_text(const parity_result *result) {
    if (result->skipped) { return "skip"; }
    if (result->status != ASX_OK) { return "fail"; }
    if (result->commit_authority_drift != 0u) { return "fail"; }
    return "pass";
}

static void emit_result_json(const parity_result *result) {
    printf("{\"kind\":\"parallel_parity_record\",");
    printf("\"scenario_id\":\"%s\",", result->scenario_id);
    printf("\"profile\":\"%s\",", active_profile_name());
    printf("\"worker_count\":%" PRIu32 ",", result->worker_count);
    printf("\"max_workers\":%" PRIu32 ",", (uint32_t)ASX_MAX_WORKERS);
    printf("\"status_code\":%d,", (int)result->status);
    printf("\"status_text\":\"%s\",", asx_status_str(result->status));
    printf("\"parity\":\"%s\",", result_parity_text(result));
    printf("\"semantic_digest\":\"fnv64:%016" PRIx64 "\",", result->semantic_digest);
    printf("\"trace_digest\":\"fnv64:%016" PRIx64 "\",", result->trace_digest);
    printf("\"event_order_digest\":\"fnv64:%016" PRIx64 "\",", result->event_order_digest);
    printf("\"event_count\":%" PRIu32 ",", result->event_count);
    printf("\"completed_tasks\":%" PRIu32 ",", result->completed_tasks);
    printf("\"queue_len\":%" PRIu32 ",", result->queue_len);
    printf("\"reserved_count\":%" PRIu32 ",", result->reserved_count);
    printf("\"blocking_active\":%" PRIu32 ",", result->blocking_active);
    printf("\"reactor_ready\":%" PRIu32 ",", result->reactor_ready);
    printf("\"waker_ready\":%" PRIu32 ",", result->waker_ready);
    printf("\"timed_promotions\":%" PRIu32 ",", result->timed_promotions);
    printf("\"pressure_pct\":%" PRIu32 ",", result->pressure_pct);
    printf("\"max_worker_queue_depth\":%" PRIu32 ",", result->max_worker_queue_depth);
    printf("\"steals_failed\":%" PRIu32 ",", result->steals_failed);
    printf("\"timed_wake_latency_rounds_max\":%" PRIu32 ",", result->timed_wake_latency_rounds_max);
    printf("\"admission\":{\"triggered\":%" PRIu32 ",\"pressure_pct\":%" PRIu32
           ",\"status_code\":%d,\"status_text\":\"%s\"},",
           result->admission_triggered, result->admission_pressure_pct,
           (int)result->admission_status, asx_status_str(result->admission_status));
    printf("\"commit_authority\":{\"commit_sequence\":%" PRIu32 ",\"total_worker_commits\":%" PRIu32
           ",\"max_worker_commit_sequence\":%" PRIu32 ",\"drift_detected\":%" PRIu32
           ",\"native_live_enabled\":%" PRIu32
           ",\"native_live_status_code\":%d,\"native_live_status_text\":\"%s\"},",
           result->commit_sequence, result->total_worker_commits,
           result->max_worker_commit_sequence, result->commit_authority_drift,
           result->native_live_enabled, (int)result->native_live_status,
           asx_status_str(result->native_live_status));
    printf("\"events\":{");
    printf("\"sched_poll\":%" PRIu32 ",", result->event_counts[ASX_TRACE_SCHED_POLL]);
    printf("\"sched_complete\":%" PRIu32 ",", result->event_counts[ASX_TRACE_SCHED_COMPLETE]);
    printf("\"sched_budget\":%" PRIu32 ",", result->event_counts[ASX_TRACE_SCHED_BUDGET]);
    printf("\"sched_quiescent\":%" PRIu32 ",", result->event_counts[ASX_TRACE_SCHED_QUIESCENT]);
    printf("\"sched_round\":%" PRIu32 ",", result->event_counts[ASX_TRACE_SCHED_ROUND]);
    printf("\"region_open\":%" PRIu32 ",", result->event_counts[ASX_TRACE_REGION_OPEN]);
    printf("\"task_spawn\":%" PRIu32 ",", result->event_counts[ASX_TRACE_TASK_SPAWN]);
    printf("\"task_transition\":%" PRIu32 ",", result->event_counts[ASX_TRACE_TASK_TRANSITION]);
    printf("\"channel_send\":%" PRIu32 ",", result->event_counts[ASX_TRACE_CHANNEL_SEND]);
    printf("\"channel_recv\":%" PRIu32 ",", result->event_counts[ASX_TRACE_CHANNEL_RECV]);
    printf("\"timer_set\":%" PRIu32 ",", result->event_counts[ASX_TRACE_TIMER_SET]);
    printf("\"timer_fire\":%" PRIu32 "},", result->event_counts[ASX_TRACE_TIMER_FIRE]);
    printf("\"order\":[");
    {
        uint32_t i;
        for (i = 0u; i < result->order_count; i++) {
            printf("%s%" PRIu32, i == 0u ? "" : ",", result->order_values[i]);
        }
    }
    printf("],\"outputs\":[");
    {
        uint32_t i;
        for (i = 0u; i < result->output_count; i++) {
            printf("%s%" PRIu64, i == 0u ? "" : ",", result->outputs[i]);
        }
    }
    printf("],\"rerun\":\"make parallel-parity\"}\n");
}

static int results_match(const parity_result *expected, const parity_result *actual) {
    uint32_t i;

    if (expected->status != actual->status) { return 0; }
    if (expected->semantic_digest != actual->semantic_digest) { return 0; }
    if (expected->event_order_digest != actual->event_order_digest) { return 0; }
    if (expected->event_count != actual->event_count) { return 0; }
    if (expected->completed_tasks != actual->completed_tasks) { return 0; }
    if (expected->queue_len != actual->queue_len) { return 0; }
    if (expected->reserved_count != actual->reserved_count) { return 0; }
    if (expected->blocking_active != actual->blocking_active) { return 0; }
    if (expected->reactor_ready != actual->reactor_ready) { return 0; }
    if (expected->waker_ready != actual->waker_ready) { return 0; }
    if (expected->timed_promotions != actual->timed_promotions) { return 0; }
    if (expected->commit_sequence != actual->commit_sequence) { return 0; }
    if (expected->total_worker_commits != actual->total_worker_commits) { return 0; }
    if (expected->max_worker_commit_sequence != actual->max_worker_commit_sequence) { return 0; }
    if (expected->commit_authority_drift != actual->commit_authority_drift) { return 0; }
    if (expected->native_live_enabled != actual->native_live_enabled) { return 0; }
    if (expected->native_live_status != actual->native_live_status) { return 0; }
    if (expected->order_count != actual->order_count) { return 0; }
    if (expected->output_count != actual->output_count) { return 0; }
    for (i = 0u; i < PARITY_EVENT_BUCKETS; i++) {
        if (expected->event_counts[i] != actual->event_counts[i]) { return 0; }
    }
    for (i = 0u; i < PARITY_MAX_ORDER; i++) {
        if (expected->order_values[i] != actual->order_values[i]) { return 0; }
    }
    for (i = 0u; i < PARITY_MAX_OUTPUTS; i++) {
        if (expected->outputs[i] != actual->outputs[i]) { return 0; }
    }
    return 1;
}

TEST(parallel_single_vs_multi_worker_digest_parity) {
    uint32_t scenario_index;
    uint32_t worker_index;
    uint32_t compared = 0u;
    uint32_t skipped = 0u;
    uint32_t commit_authority_drift_count = 0u;
    uint32_t native_live_enabled_count = 0u;

    for (scenario_index = 0u; scenario_index < (uint32_t)(sizeof(SCENARIOS) / sizeof(SCENARIOS[0]));
         scenario_index++) {
        parity_result baseline;
        asx_status st;

        st = run_scenario(SCENARIOS[scenario_index], WORKER_COUNTS[0], &baseline);
        emit_result_json(&baseline);
        ASSERT_EQ(st, ASX_OK);
        ASSERT_FALSE(baseline.skipped);
        ASSERT_EQ(baseline.status, ASX_OK);
        if (baseline.commit_authority_drift != 0u) { commit_authority_drift_count++; }
        if (baseline.native_live_enabled != 0u) { native_live_enabled_count++; }
        ASSERT_EQ(baseline.commit_authority_drift, 0u);
        ASSERT_EQ(baseline.total_worker_commits, baseline.commit_sequence);

        for (worker_index = 1u;
             worker_index < (uint32_t)(sizeof(WORKER_COUNTS) / sizeof(WORKER_COUNTS[0]));
             worker_index++) {
            parity_result actual;

            st = run_scenario(SCENARIOS[scenario_index], WORKER_COUNTS[worker_index], &actual);
            emit_result_json(&actual);
            ASSERT_EQ(st, ASX_OK);
            if (actual.skipped) {
                skipped++;
                continue;
            }
            if (actual.commit_authority_drift != 0u) { commit_authority_drift_count++; }
            if (actual.native_live_enabled != 0u) { native_live_enabled_count++; }
            ASSERT_EQ(actual.commit_authority_drift, 0u);
            ASSERT_EQ(actual.total_worker_commits, actual.commit_sequence);

            if (!results_match(&baseline, &actual)) {
                fprintf(stderr,
                        "parallel parity mismatch: scenario=%s worker=%" PRIu32
                        " baseline_digest=%016" PRIx64 " actual_digest=%016" PRIx64
                        " baseline_trace=%016" PRIx64 " actual_trace=%016" PRIx64 "\n",
                        actual.scenario_id, actual.worker_count, baseline.semantic_digest,
                        actual.semantic_digest, baseline.trace_digest, actual.trace_digest);
            }
            ASSERT_TRUE(results_match(&baseline, &actual));
            compared++;
        }
    }

    printf("{\"kind\":\"parallel_parity_summary\",");
    printf("\"status\":\"pass\",");
    printf("\"profile\":\"%s\",", active_profile_name());
    printf("\"scenario_count\":%u,", (unsigned)(sizeof(SCENARIOS) / sizeof(SCENARIOS[0])));
    printf("\"baseline_worker_count\":1,");
    printf("\"compared_records\":%" PRIu32 ",", compared);
    printf("\"skipped_records\":%" PRIu32 ",", skipped);
    printf("\"commit_authority_drift_count\":%" PRIu32 ",", commit_authority_drift_count);
    printf("\"native_live_enabled_count\":%" PRIu32 ",", native_live_enabled_count);
    printf("\"worker_counts\":[1,2,8,%" PRIu32 "],", (uint32_t)ASX_PARALLEL_GENERIC_TARGET_WORKERS);
    printf("\"max_workers\":%" PRIu32 ",", (uint32_t)ASX_MAX_WORKERS);
    printf("\"rerun\":\"make parallel-parity\"}\n");
}

int main(void) {
    fprintf(stderr, "=== parallel_parity_conformance_test ===\n");

    RUN_TEST(parallel_single_vs_multi_worker_digest_parity);

    TEST_REPORT();
    return test_failures;
}
