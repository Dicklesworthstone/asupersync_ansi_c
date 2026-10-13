/*
 * asx/observability/health.h — public runtime health and threshold evaluation
 *
 * A C-only evaluator over runtime inspection and watchdog state; it is not
 * asupersync's `monitor` module (task monitors and DOWN notifications), which
 * is asx/monitor/monitor.h.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_OBSERVABILITY_HEALTH_H
#define ASX_OBSERVABILITY_HEALTH_H

#include <asx/runtime/automotive_instrument.h>
#include <asx/runtime/diagnostic.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ASX_HEALTH_REGIONS_HIGH (1u << 0)
#define ASX_HEALTH_TASKS_HIGH (1u << 1)
#define ASX_HEALTH_OBLIGATIONS_HIGH (1u << 2)
#define ASX_HEALTH_GHOSTS_PRESENT (1u << 3)
#define ASX_HEALTH_DEADLINE_MISS_RATE (1u << 4)
#define ASX_HEALTH_WATCHDOG_VIOLATIONS (1u << 5)
#define ASX_HEALTH_IO_HIGH (1u << 6)
#define ASX_HEALTH_BLOCKING_HIGH (1u << 7)

typedef struct {
    uint32_t max_region_utilization_pct;
    uint32_t max_task_utilization_pct;
    uint32_t max_obligation_utilization_pct;
    uint32_t max_io_utilization_pct;
    uint32_t max_blocking_utilization_pct;
    uint32_t max_ghost_violations;
    uint32_t max_deadline_miss_rate_pct100;
    uint32_t max_watchdog_violations;
} asx_health_policy;

typedef struct {
    asx_inspection_report inspection;
    asx_auto_compliance_result compliance;
    uint32_t triggered_mask;
    asx_evidence_level verdict;
} asx_health_report;

/* Initialize a health policy with default threshold values. */
ASX_API void asx_health_policy_init_default(asx_health_policy *policy);

/* Evaluate runtime state against threshold policy.
 * Stale or uninitialized runtimes fail closed with FAIL evidence rather than
 * being treated as healthy zero-utilization snapshots. */
ASX_API ASX_MUST_USE asx_status asx_health_evaluate(const asx_runtime *rt,
                                                    const asx_health_policy *policy,
                                                    asx_health_report *out,
                                                    asx_evidence_sink *sink);

#ifdef __cplusplus
}
#endif

#endif /* ASX_OBSERVABILITY_HEALTH_H */
