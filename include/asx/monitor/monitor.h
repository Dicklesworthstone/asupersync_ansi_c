/*
 * asx/monitor/monitor.h — public monitor and threshold evaluation family
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_MONITOR_MONITOR_H
#define ASX_MONITOR_MONITOR_H

#include <asx/runtime/automotive_instrument.h>
#include <asx/runtime/diagnostic.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ASX_MONITOR_REGIONS_HIGH (1u << 0)
#define ASX_MONITOR_TASKS_HIGH (1u << 1)
#define ASX_MONITOR_OBLIGATIONS_HIGH (1u << 2)
#define ASX_MONITOR_GHOSTS_PRESENT (1u << 3)
#define ASX_MONITOR_DEADLINE_MISS_RATE (1u << 4)
#define ASX_MONITOR_WATCHDOG_VIOLATIONS (1u << 5)
#define ASX_MONITOR_IO_HIGH (1u << 6)
#define ASX_MONITOR_BLOCKING_HIGH (1u << 7)

typedef struct {
    uint32_t max_region_utilization_pct;
    uint32_t max_task_utilization_pct;
    uint32_t max_obligation_utilization_pct;
    uint32_t max_io_utilization_pct;
    uint32_t max_blocking_utilization_pct;
    uint32_t max_ghost_violations;
    uint32_t max_deadline_miss_rate_pct100;
    uint32_t max_watchdog_violations;
} asx_monitor_policy;

typedef struct {
    asx_inspection_report inspection;
    asx_auto_compliance_result compliance;
    uint32_t triggered_mask;
    asx_evidence_level verdict;
} asx_monitor_report;

ASX_API void asx_monitor_policy_init_default(asx_monitor_policy *policy);

ASX_API ASX_MUST_USE asx_status asx_monitor_evaluate(const asx_runtime *rt,
                                                     const asx_monitor_policy *policy,
                                                     asx_monitor_report *out,
                                                     asx_evidence_sink *sink);

#ifdef __cplusplus
}
#endif

#endif /* ASX_MONITOR_MONITOR_H */
