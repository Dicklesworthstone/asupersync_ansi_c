/*
 * observability.c — metrics, task inspection, resource accounting
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/observability/observability.h>
#include <asx/runtime/runtime.h>
#include <limits.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Snapshot capture                                                    */
/* ------------------------------------------------------------------ */

asx_status asx_observability_capture(const asx_runtime *rt, const asx_evidence_sink *sink,
                                     asx_observability_snapshot *out) {
    asx_status st;

    if (rt == NULL || sink == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;

    st = asx_inspect(rt, &out->inspection);
    if (st != ASX_OK) return st;

    st = asx_evidence_sink_summarize(sink, &out->evidence);
    if (st != ASX_OK) return st;

    out->trace_digest = out->inspection.trace.digest;
    out->telemetry_digest = out->inspection.telemetry_digest;
    out->event_log_digest = out->inspection.event_log_digest;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Metrics                                                             */
/* ------------------------------------------------------------------ */

void asx_metrics_init(asx_metrics *m) {
    if (m == NULL) return;
    memset(m, 0, sizeof(*m));
}

static int64_t metrics_clamp_add_i64(int64_t lhs, int64_t rhs) {
    if (rhs > 0 && lhs > INT64_MAX - rhs) return INT64_MAX;
    if (rhs < 0 && lhs < INT64_MIN - rhs) return INT64_MIN;
    return lhs + rhs;
}

static asx_metric_entry *metrics_find(asx_metrics *m, const char *name) {
    uint32_t i;
    if (m == NULL || name == NULL) return NULL;
    for (i = 0u; i < m->count; i++) {
        if (m->entries[i].active && strcmp(m->entries[i].name, name) == 0) {
            return &m->entries[i];
        }
    }
    return NULL;
}

static asx_status metrics_register(asx_metrics *m, const char *name, asx_metric_type type) {
    size_t len;
    asx_metric_entry *e;

    if (m == NULL || name == NULL) return ASX_E_INVALID_ARGUMENT;
    if (metrics_find(m, name) != NULL) return ASX_E_ALREADY_EXISTS;
    if (m->count >= ASX_METRICS_MAX_ENTRIES) return ASX_E_RESOURCE_EXHAUSTED;

    len = strlen(name);
    if (len == 0u || len >= ASX_METRICS_NAME_MAX) return ASX_E_INVALID_ARGUMENT;

    e = &m->entries[m->count];
    memset(e, 0, sizeof(*e));
    memcpy(e->name, name, len + 1u);
    e->type = type;
    e->active = 1u;
    if (type == ASX_METRIC_HISTOGRAM) {
        e->min_val = INT64_MAX;
        e->max_val = INT64_MIN;
    }
    m->count++;
    return ASX_OK;
}

asx_status asx_metrics_counter(asx_metrics *m, const char *name) {
    return metrics_register(m, name, ASX_METRIC_COUNTER);
}

asx_status asx_metrics_gauge(asx_metrics *m, const char *name) {
    return metrics_register(m, name, ASX_METRIC_GAUGE);
}

asx_status asx_metrics_histogram(asx_metrics *m, const char *name) {
    return metrics_register(m, name, ASX_METRIC_HISTOGRAM);
}

asx_status asx_metrics_increment(asx_metrics *m, const char *name, int64_t delta) {
    asx_metric_entry *e = metrics_find(m, name);
    if (e == NULL) return ASX_E_NOT_FOUND;
    if (e->type != ASX_METRIC_COUNTER) return ASX_E_INVALID_STATE;
    e->value = metrics_clamp_add_i64(e->value, delta);
    return ASX_OK;
}

asx_status asx_metrics_set(asx_metrics *m, const char *name, int64_t value) {
    asx_metric_entry *e = metrics_find(m, name);
    if (e == NULL) return ASX_E_NOT_FOUND;
    if (e->type != ASX_METRIC_GAUGE) return ASX_E_INVALID_STATE;
    e->value = value;
    return ASX_OK;
}

asx_status asx_metrics_observe(asx_metrics *m, const char *name, int64_t value) {
    asx_metric_entry *e = metrics_find(m, name);
    if (e == NULL) return ASX_E_NOT_FOUND;
    if (e->type != ASX_METRIC_HISTOGRAM) return ASX_E_INVALID_STATE;
    e->count++;
    e->sum = metrics_clamp_add_i64(e->sum, value);
    if (value < e->min_val) e->min_val = value;
    if (value > e->max_val) e->max_val = value;
    return ASX_OK;
}

const asx_metric_entry *asx_metrics_get(const asx_metrics *m, const char *name) {
    uint32_t i;
    if (m == NULL || name == NULL) return NULL;
    for (i = 0u; i < m->count; i++) {
        if (m->entries[i].active && strcmp(m->entries[i].name, name) == 0) {
            return &m->entries[i];
        }
    }
    return NULL;
}

uint32_t asx_metrics_count(const asx_metrics *m) {
    if (m == NULL) return 0u;
    return m->count;
}

/* ------------------------------------------------------------------ */
/* Task inspector                                                      */
/* ------------------------------------------------------------------ */

asx_status asx_task_inspect(asx_task_id task_id, asx_task_inspection *out) {
    asx_task_state state;
    asx_status st;

    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->task_id = task_id;

    st = asx_task_get_state(task_id, &state);
    if (st != ASX_OK) return st;
    out->state = state;

    /* Region and cancel info require runtime internal access;
     * expose what's available through public API. */
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Resource accounting                                                 */
/* ------------------------------------------------------------------ */

asx_status asx_resource_accounting_capture(const asx_runtime *rt, asx_resource_accounting *out) {
    asx_inspection_report rpt;
    asx_status st;

    if (rt == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    st = asx_inspect(rt, &rpt);
    if (st != ASX_OK) return st;

    out->regions_active = rpt.regions.active_count;
    out->regions_capacity = rpt.regions.capacity;
    out->tasks_active = rpt.tasks.active_count;
    out->tasks_capacity = rpt.tasks.capacity;
    out->obligations_active = rpt.obligations.active_count;
    out->obligations_capacity = rpt.obligations.capacity;

    return ASX_OK;
}
