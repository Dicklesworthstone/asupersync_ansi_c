/*
 * diagnostic.c — structured diagnostic context, inspection, and evidence sink
 *
 * SPDX-License-Identifier: MIT
 */

#include "runtime_internal.h"
#include <asx/core/ghost.h>
#include <asx/runtime/diagnostic.h>
#include <asx/runtime/event.h>
#include <asx/runtime/rt.h>
#include <asx/runtime/runtime.h>
#include <asx/runtime/telemetry.h>
#include <asx/runtime/trace.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Diagnostic context stack                                            */
/* ------------------------------------------------------------------ */

#ifndef ASX_DIAGNOSTIC_STACK_DEPTH
#define ASX_DIAGNOSTIC_STACK_DEPTH 8u
#endif

static asx_diagnostic_ctx g_diag_stack[ASX_DIAGNOSTIC_STACK_DEPTH];
static uint32_t g_diag_depth;

asx_status asx_diagnostic_push(const asx_diagnostic_ctx *ctx) {
    if (ctx == NULL) return ASX_E_INVALID_ARGUMENT;
    if (g_diag_depth >= ASX_DIAGNOSTIC_STACK_DEPTH) return ASX_E_RESOURCE_EXHAUSTED;

    g_diag_stack[g_diag_depth] = *ctx;
    g_diag_stack[g_diag_depth].depth = g_diag_depth;
    g_diag_depth++;
    return ASX_OK;
}

void asx_diagnostic_pop(void) {
    if (g_diag_depth > 0) g_diag_depth--;
}

const asx_diagnostic_ctx *asx_diagnostic_current(void) {
    if (g_diag_depth == 0) return NULL;
    return &g_diag_stack[g_diag_depth - 1];
}

void asx_diagnostic_reset(void) {
    memset(g_diag_stack, 0, sizeof(g_diag_stack));
    g_diag_depth = 0;
}

/* ------------------------------------------------------------------ */
/* Runtime inspection                                                  */
/* ------------------------------------------------------------------ */

static uint32_t count_alive_regions(void) {
    uint32_t i, n = 0;
    for (i = 0; i < g_region_count; i++) {
        if (g_regions[i].alive) n++;
    }
    return n;
}

static uint32_t count_alive_tasks(void) {
    uint32_t i, n = 0;
    for (i = 0; i < g_task_count; i++) {
        if (g_tasks[i].alive) n++;
    }
    return n;
}

static uint32_t count_alive_obligations(void) {
    uint32_t i, n = 0;
    for (i = 0; i < g_obligation_count; i++) {
        if (g_obligations[i].alive) n++;
    }
    return n;
}

static int any_region_poisoned(void) {
    uint32_t i;
    for (i = 0; i < g_region_count; i++) {
        if (g_regions[i].alive && g_regions[i].poisoned) return 1;
    }
    return 0;
}

asx_status asx_inspect(const asx_runtime *rt, asx_inspection_report *out) {
    if (rt == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));

    /* Entity gauges */
    out->regions.active_count = count_alive_regions();
    out->regions.capacity = ASX_MAX_REGIONS;
    out->regions.peak_count = g_region_count; /* high water mark */

    out->tasks.active_count = count_alive_tasks();
    out->tasks.capacity = ASX_MAX_TASKS;
    out->tasks.peak_count = g_task_count;

    out->obligations.active_count = count_alive_obligations();
    out->obligations.capacity = ASX_MAX_OBLIGATIONS;
    out->obligations.peak_count = g_obligation_count;

    /* Trace */
    out->trace.event_count = asx_trace_event_count();
    out->trace.capacity = ASX_TRACE_CAPACITY;
    out->trace.digest = asx_trace_digest();

    /* Error ledger — summarize across all tasks */
    {
        uint32_t i;
        out->errors.total_errors = 0;
        out->errors.overflowed = 0;
        for (i = 0; i < g_task_count; i++) {
            if (g_tasks[i].alive) {
                /* We can't easily query per-task error counts without
                 * a task handle. Track the task count as a proxy. */
            }
        }
        /* Note: per-task error counts require task handles.
         * The total_errors field stays 0 until a task-enumeration
         * API is added. This is documented as a known limitation. */
    }

    /* Ghost monitors */
    out->ghosts.violation_count = asx_ghost_violation_count();
    out->ghosts.overflowed = (uint32_t)asx_ghost_ring_overflowed();
    out->ghosts.obligation_leaks = 0; /* requires region scan */

    /* Runtime state */
    out->safety_profile = asx_runtime_safety_profile(rt);
    out->containment_policy = asx_runtime_containment_policy(rt);
    out->initialized = asx_runtime_is_initialized(rt);
    out->any_poisoned = any_region_poisoned();

    /* Telemetry */
    out->telemetry_emitted = asx_telemetry_emitted_count();
    out->telemetry_filtered = asx_telemetry_filtered_count();
    out->telemetry_digest = asx_telemetry_digest();

    /* Event log */
    out->event_log_count = asx_event_log_count();
    out->event_log_digest = asx_event_hash_chain();

    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Evidence sink                                                       */
/* ------------------------------------------------------------------ */

void asx_evidence_sink_init(asx_evidence_sink *sink) {
    if (sink == NULL) return;
    memset(sink, 0, sizeof(*sink));
}

asx_status asx_evidence_record(asx_evidence_sink *sink, const char *source,
                               asx_evidence_level level, const char *message, uint64_t entity_id) {
    asx_evidence_entry *e;

    if (sink == NULL) return ASX_E_INVALID_ARGUMENT;
    if (sink->count >= ASX_EVIDENCE_SINK_CAPACITY) return ASX_E_RESOURCE_EXHAUSTED;

    e = &sink->entries[sink->count];
    e->source = source;
    e->level = level;
    e->message = message;
    e->entity_id = entity_id;
    e->sequence = sink->next_seq++;
    sink->count++;

    switch (level) {
    case ASX_EVIDENCE_INFO: sink->info_count++; break;
    case ASX_EVIDENCE_PASS: sink->pass_count++; break;
    case ASX_EVIDENCE_WARN: sink->warn_count++; break;
    case ASX_EVIDENCE_FAIL: sink->fail_count++; break;
    }

    return ASX_OK;
}

asx_evidence_level asx_evidence_verdict(const asx_evidence_sink *sink) {
    if (sink == NULL) return ASX_EVIDENCE_FAIL;
    if (sink->fail_count > 0) return ASX_EVIDENCE_FAIL;
    if (sink->warn_count > 0) return ASX_EVIDENCE_WARN;
    return ASX_EVIDENCE_PASS;
}

int asx_evidence_sink_has_room(const asx_evidence_sink *sink) {
    if (sink == NULL) return 0;
    return sink->count < ASX_EVIDENCE_SINK_CAPACITY;
}

void asx_evidence_sink_reset(asx_evidence_sink *sink) {
    if (sink == NULL) return;
    memset(sink, 0, sizeof(*sink));
}

/* ------------------------------------------------------------------ */
/* Inspect-to-evidence pipeline                                        */
/* ------------------------------------------------------------------ */

asx_status asx_inspect_to_evidence(const asx_runtime *rt, asx_evidence_sink *sink) {
    asx_inspection_report rpt;
    asx_status st;

    if (rt == NULL || sink == NULL) return ASX_E_INVALID_ARGUMENT;

    st = asx_inspect(rt, &rpt);
    if (st != ASX_OK) return st;

    /* Runtime initialization */
    st =
        asx_evidence_record(sink, "inspect:runtime",
                            rpt.initialized ? ASX_EVIDENCE_PASS : ASX_EVIDENCE_FAIL,
                            rpt.initialized ? "runtime initialized" : "runtime not initialized", 0);
    if (st != ASX_OK) return st;

    /* Region utilization */
    {
        asx_evidence_level lev = ASX_EVIDENCE_PASS;
        const char *msg = "region utilization normal";
        if (rpt.regions.active_count >= rpt.regions.capacity) {
            lev = ASX_EVIDENCE_FAIL;
            msg = "region arena exhausted";
        } else if (rpt.regions.active_count > rpt.regions.capacity * 3 / 4) {
            lev = ASX_EVIDENCE_WARN;
            msg = "region arena >75% utilized";
        }
        st = asx_evidence_record(sink, "inspect:regions", lev, msg, 0);
        if (st != ASX_OK) return st;
    }

    /* Task utilization */
    {
        asx_evidence_level lev = ASX_EVIDENCE_PASS;
        const char *msg = "task utilization normal";
        if (rpt.tasks.active_count >= rpt.tasks.capacity) {
            lev = ASX_EVIDENCE_FAIL;
            msg = "task arena exhausted";
        } else if (rpt.tasks.active_count > rpt.tasks.capacity * 3 / 4) {
            lev = ASX_EVIDENCE_WARN;
            msg = "task arena >75% utilized";
        }
        st = asx_evidence_record(sink, "inspect:tasks", lev, msg, 0);
        if (st != ASX_OK) return st;
    }

    /* Obligation utilization */
    {
        asx_evidence_level lev = ASX_EVIDENCE_PASS;
        const char *msg = "obligation utilization normal";
        if (rpt.obligations.active_count >= rpt.obligations.capacity) {
            lev = ASX_EVIDENCE_FAIL;
            msg = "obligation arena exhausted";
        } else if (rpt.obligations.active_count > rpt.obligations.capacity * 3 / 4) {
            lev = ASX_EVIDENCE_WARN;
            msg = "obligation arena >75% utilized";
        }
        st = asx_evidence_record(sink, "inspect:obligations", lev, msg, 0);
        if (st != ASX_OK) return st;
    }

    /* Poisoned regions */
    if (rpt.any_poisoned) {
        st = asx_evidence_record(sink, "inspect:poison", ASX_EVIDENCE_WARN,
                                 "one or more regions poisoned", 0);
        if (st != ASX_OK) return st;
    }

    /* Ghost violations */
    if (rpt.ghosts.violation_count > 0) {
        st = asx_evidence_record(sink, "inspect:ghosts", ASX_EVIDENCE_FAIL,
                                 "ghost safety violations detected", 0);
        if (st != ASX_OK) return st;
    }

    /* Trace digest (informational) */
    st = asx_evidence_record(sink, "inspect:trace", ASX_EVIDENCE_INFO, "trace digest captured",
                             rpt.trace.digest);
    if (st != ASX_OK) return st;

    return ASX_OK;
}
