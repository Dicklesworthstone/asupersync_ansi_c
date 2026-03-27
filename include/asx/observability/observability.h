/*
 * asx/observability/observability.h — public observability snapshot family
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_OBSERVABILITY_OBSERVABILITY_H
#define ASX_OBSERVABILITY_OBSERVABILITY_H

#include <asx/evidence_sink/evidence_sink.h>
#include <asx/runtime/diagnostic.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    asx_inspection_report inspection;
    asx_evidence_summary evidence;
    uint64_t trace_digest;
    uint64_t telemetry_digest;
    uint64_t event_log_digest;
} asx_observability_snapshot;

/* Capture a combined observability snapshot from the runtime and evidence sink. */
ASX_API ASX_MUST_USE asx_status asx_observability_capture(const asx_runtime *rt,
                                                          const asx_evidence_sink *sink,
                                                          asx_observability_snapshot *out);

/* -------------------------------------------------------------------
 * Metrics — counter/gauge/histogram primitives
 * ------------------------------------------------------------------- */

#ifndef ASX_METRICS_MAX_ENTRIES
#define ASX_METRICS_MAX_ENTRIES 32u
#endif

#ifndef ASX_METRICS_NAME_MAX
#define ASX_METRICS_NAME_MAX 64u
#endif

typedef enum {
    ASX_METRIC_COUNTER   = 0,
    ASX_METRIC_GAUGE     = 1,
    ASX_METRIC_HISTOGRAM = 2
} asx_metric_type;

typedef struct {
    char name[ASX_METRICS_NAME_MAX];
    asx_metric_type type;
    int64_t value;       /* counter/gauge current value */
    uint64_t count;      /* histogram: number of observations */
    int64_t sum;         /* histogram: sum of observations */
    int64_t min_val;     /* histogram: minimum observed */
    int64_t max_val;     /* histogram: maximum observed */
    uint8_t active;
} asx_metric_entry;

typedef struct {
    asx_metric_entry entries[ASX_METRICS_MAX_ENTRIES];
    uint32_t count;
} asx_metrics;

/* Initialize a metrics collection. */
ASX_API void asx_metrics_init(asx_metrics *m);

/* Register a counter metric. */
ASX_API ASX_MUST_USE asx_status asx_metrics_counter(asx_metrics *m, const char *name);

/* Register a gauge metric. */
ASX_API ASX_MUST_USE asx_status asx_metrics_gauge(asx_metrics *m, const char *name);

/* Register a histogram metric. */
ASX_API ASX_MUST_USE asx_status asx_metrics_histogram(asx_metrics *m, const char *name);

/* Increment a counter by delta. */
ASX_API ASX_MUST_USE asx_status asx_metrics_increment(asx_metrics *m, const char *name,
                                                        int64_t delta);

/* Set a gauge value. */
ASX_API ASX_MUST_USE asx_status asx_metrics_set(asx_metrics *m, const char *name, int64_t value);

/* Record a histogram observation. */
ASX_API ASX_MUST_USE asx_status asx_metrics_observe(asx_metrics *m, const char *name,
                                                      int64_t value);

/* Get a metric by name. Returns NULL if not found. */
ASX_API const asx_metric_entry *asx_metrics_get(const asx_metrics *m, const char *name);

/* Get the number of registered metrics. */
ASX_API uint32_t asx_metrics_count(const asx_metrics *m);

/* -------------------------------------------------------------------
 * Task inspector — query task details for diagnostics
 * ------------------------------------------------------------------- */

typedef struct {
    asx_task_id task_id;
    asx_task_state state;
    asx_region_id region_id;
    uint8_t cancel_pending;
    uint32_t cancel_epoch;
    uint32_t cleanup_polls_remaining;
} asx_task_inspection;

/* Inspect a specific task. */
ASX_API ASX_MUST_USE asx_status asx_task_inspect(asx_task_id task_id,
                                                   asx_task_inspection *out);

/* -------------------------------------------------------------------
 * Resource accounting — track resource usage
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t regions_active;
    uint32_t regions_capacity;
    uint32_t tasks_active;
    uint32_t tasks_capacity;
    uint32_t obligations_active;
    uint32_t obligations_capacity;
    uint64_t total_polls;
    uint64_t total_task_completions;
    uint64_t total_cancellations;
} asx_resource_accounting;

/* Capture current resource accounting. */
ASX_API ASX_MUST_USE asx_status asx_resource_accounting_capture(const asx_runtime *rt,
                                                                  asx_resource_accounting *out);

#ifdef __cplusplus
}
#endif

#endif /* ASX_OBSERVABILITY_OBSERVABILITY_H */
