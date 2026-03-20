/*
 * regression_localize.c — Replay-guided regression localization
 * and deterministic circuit-breaker (bd-3vt.5)
 *
 * SPDX-License-Identifier: MIT
 */

/* ASX_CHECKPOINT_WAIVER_FILE() -- regression localize spike, no checkpoint coverage needed */

#include <asx/asx.h>
#include <asx/runtime/regression_localize.h>
#include <asx/runtime/trace.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Subsystem classification                                           */
/* ------------------------------------------------------------------ */

static asx_task_state regression_trace_transition_from(uint64_t aux) {
    return (asx_task_state)(uint32_t)(aux >> 32);
}

static asx_task_state regression_trace_transition_to(uint64_t aux) {
    return (asx_task_state)(uint32_t)aux;
}

static int regression_trace_is_cancel_state(asx_task_state state) {
    return state == ASX_TASK_CANCEL_REQUESTED || state == ASX_TASK_CANCELLING ||
           state == ASX_TASK_FINALIZING;
}

static asx_subsystem_id regression_trace_event_subsystem(const asx_trace_event *event) {
    asx_subsystem_id sub;

    if (event == NULL) return ASX_SUBSYS_UNKNOWN;

    sub = asx_trace_event_subsystem(event->kind);
    if (event->kind == ASX_TRACE_TASK_TRANSITION) {
        asx_task_state from = regression_trace_transition_from(event->aux);
        asx_task_state to = regression_trace_transition_to(event->aux);
        if (regression_trace_is_cancel_state(from) || regression_trace_is_cancel_state(to)) {
            return ASX_SUBSYS_CANCEL;
        }
    }

    return sub;
}

asx_subsystem_id asx_trace_event_subsystem(asx_trace_event_kind kind) {
    uint32_t k = (uint32_t)kind;

    if (k <= 0x0Fu) return ASX_SUBSYS_SCHEDULER;
    if (k <= 0x1Fu) return ASX_SUBSYS_LIFECYCLE;
    if (k <= 0x2Fu) return ASX_SUBSYS_OBLIGATION;
    if (k <= 0x3Fu) return ASX_SUBSYS_CHANNEL;
    if (k <= 0x4Fu) return ASX_SUBSYS_TIMER;
    /* Cancel events share the lifecycle range in trace */
    return ASX_SUBSYS_UNKNOWN;
}

const char *asx_subsystem_name(asx_subsystem_id id) {
    switch (id) {
    case ASX_SUBSYS_SCHEDULER: return "scheduler";
    case ASX_SUBSYS_LIFECYCLE: return "lifecycle";
    case ASX_SUBSYS_OBLIGATION: return "obligation";
    case ASX_SUBSYS_CHANNEL: return "channel";
    case ASX_SUBSYS_TIMER: return "timer";
    case ASX_SUBSYS_CANCEL: return "cancel";
    case ASX_SUBSYS_UNKNOWN: return "unknown";
    }
    return "unknown";
}

/* ------------------------------------------------------------------ */
/* Performance snapshot                                               */
/* ------------------------------------------------------------------ */

void asx_perf_snapshot_build(asx_perf_snapshot *snap) {
    uint32_t count;
    uint32_t i;

    if (snap == NULL) return;
    memset(snap, 0, sizeof(*snap));

    count = asx_trace_event_count();

    for (i = 0; i < count; i++) {
        asx_trace_event ev;
        asx_subsystem_id sub;

        if (!asx_trace_event_get(i, &ev)) break;

        sub = regression_trace_event_subsystem(&ev);
        if ((uint32_t)sub < ASX_SUBSYS_COUNT) {
            snap->subsystems[(uint32_t)sub].event_count++;
            snap->subsystems[(uint32_t)sub].total_aux += ev.aux;
        }
    }

    snap->total_events = count;
    snap->trace_digest = asx_trace_digest();
}

/* ------------------------------------------------------------------ */
/* Regression localization                                            */
/* ------------------------------------------------------------------ */

static uint32_t abs_i32(int32_t v) {
    /* Widen to int64_t before negation to avoid UB when v == INT32_MIN */
    return (v < 0) ? (uint32_t)(-(int64_t)v) : (uint32_t)v;
}

void asx_regression_localize(const asx_perf_snapshot *baseline, const asx_perf_snapshot *current,
                             const asx_replay_result *replay, asx_regression_report *report) {
    uint32_t i;
    uint32_t max_delta;
    uint32_t total_delta;

    if (report == NULL) return;
    memset(report, 0, sizeof(*report));

    if (baseline == NULL || current == NULL) return;

    /* Check for digest mismatch */
    if (baseline->trace_digest == current->trace_digest) {
        report->regressed = 0;
        return;
    }

    report->regressed = 1;

    /* If replay result available, extract divergence info */
    if (replay != NULL && replay->result != ASX_REPLAY_MATCH) {
        report->divergence_index = replay->divergence_index;
        report->divergence_kind = replay->result;
    }

    /* Compute per-subsystem deltas and blame scores */
    total_delta = 0;
    for (i = 0; i < ASX_SUBSYS_COUNT; i++) {
        int32_t ed = (int32_t)current->subsystems[i].event_count -
                     (int32_t)baseline->subsystems[i].event_count;
        total_delta += abs_i32(ed);
    }

    /* Rank subsystems by absolute event delta */
    max_delta = (total_delta > 0) ? total_delta : 1;
    report->suspect_count = 0;

    for (i = 0; i < ASX_SUBSYS_COUNT && report->suspect_count < ASX_MAX_SUSPECTS; i++) {
        int32_t ed = (int32_t)current->subsystems[i].event_count -
                     (int32_t)baseline->subsystems[i].event_count;
        int64_t ad =
            (int64_t)current->subsystems[i].total_aux - (int64_t)baseline->subsystems[i].total_aux;
        uint32_t score;

        if (ed == 0 && ad == 0) continue;

        score = (uint32_t)(((uint64_t)abs_i32(ed) * 100u) / max_delta);

        report->suspects[report->suspect_count].id = (asx_subsystem_id)i;
        report->suspects[report->suspect_count].event_delta = ed;
        report->suspects[report->suspect_count].aux_delta = ad;
        report->suspects[report->suspect_count].blame_score = score;
        report->suspect_count++;
    }

    /* Sort suspects by blame_score descending (simple selection sort) */
    {
        uint32_t j;
        for (i = 0; i < report->suspect_count; i++) {
            uint32_t best = i;
            for (j = i + 1; j < report->suspect_count; j++) {
                if (report->suspects[j].blame_score > report->suspects[best].blame_score) {
                    best = j;
                }
            }
            if (best != i) {
                asx_regression_suspect tmp = report->suspects[i];
                report->suspects[i] = report->suspects[best];
                report->suspects[best] = tmp;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Deterministic circuit-breaker                                       */
/* ------------------------------------------------------------------ */

static const asx_cb_config g_cb_defaults = {
    3, /* failure_threshold */
    2, /* recovery_probes */
    10 /* cooldown_events */
};

void asx_cb_init(asx_cb_context *ctx) {
    if (ctx == NULL) return;
    memset(ctx, 0, sizeof(*ctx));
    ctx->state = ASX_CB_CLOSED;
}

void asx_cb_configure(asx_cb_context *ctx, const asx_cb_config *cfg) {
    (void)ctx;
    (void)cfg;
    /* Config is passed per-call; this is a no-op placeholder for
     * potential future state-level config caching. */
}

void asx_cb_record_success(asx_cb_context *ctx, const asx_cb_config *cfg) {
    const asx_cb_config *c;

    if (ctx == NULL) return;
    c = (cfg != NULL) ? cfg : &g_cb_defaults;

    switch (ctx->state) {
    case ASX_CB_CLOSED: ctx->consecutive_failures = 0; break;

    case ASX_CB_OPEN:
        /* Successes in OPEN are ignored (traffic shouldn't reach here) */
        break;

    case ASX_CB_HALF_OPEN:
        if (ctx->consecutive_successes < UINT32_MAX) ctx->consecutive_successes++;
        if (ctx->consecutive_successes >= c->recovery_probes) {
            ctx->state = ASX_CB_CLOSED;
            ctx->consecutive_failures = 0;
            ctx->consecutive_successes = 0;
            if (ctx->total_recoveries < UINT32_MAX) ctx->total_recoveries++;
        }
        break;
    }
}

void asx_cb_record_failure(asx_cb_context *ctx, const asx_cb_config *cfg) {
    const asx_cb_config *c;

    if (ctx == NULL) return;
    c = (cfg != NULL) ? cfg : &g_cb_defaults;

    switch (ctx->state) {
    case ASX_CB_CLOSED:
        if (ctx->consecutive_failures < UINT32_MAX) ctx->consecutive_failures++;
        if (ctx->consecutive_failures >= c->failure_threshold) {
            ctx->state = ASX_CB_OPEN;
            ctx->events_since_trip = 0;
            if (ctx->total_trips < UINT32_MAX) ctx->total_trips++;
        }
        break;

    case ASX_CB_OPEN:
        /* Already tripped */
        break;

    case ASX_CB_HALF_OPEN:
        /* Probe failed — back to OPEN */
        ctx->state = ASX_CB_OPEN;
        ctx->events_since_trip = 0;
        ctx->consecutive_successes = 0;
        if (ctx->total_trips < UINT32_MAX) ctx->total_trips++;
        break;
    }
}

void asx_cb_tick(asx_cb_context *ctx, const asx_cb_config *cfg) {
    const asx_cb_config *c;

    if (ctx == NULL) return;
    c = (cfg != NULL) ? cfg : &g_cb_defaults;

    if (ctx->state == ASX_CB_OPEN) {
        if (ctx->events_since_trip < UINT32_MAX) ctx->events_since_trip++;
        if (ctx->events_since_trip >= c->cooldown_events) {
            ctx->state = ASX_CB_HALF_OPEN;
            ctx->consecutive_successes = 0;
        }
    }
}

int asx_cb_allows(const asx_cb_context *ctx) {
    if (ctx == NULL) return 0;
    return ctx->state != ASX_CB_OPEN;
}

const char *asx_cb_state_name(asx_cb_state state) {
    switch (state) {
    case ASX_CB_CLOSED: return "closed";
    case ASX_CB_OPEN: return "open";
    case ASX_CB_HALF_OPEN: return "half-open";
    }
    return "unknown";
}
