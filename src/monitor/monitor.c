/*
 * monitor.c — public monitor and threshold evaluation family
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/monitor/monitor.h>

void asx_monitor_policy_init_default(asx_monitor_policy *policy) {
    if (policy == NULL) return;
    policy->max_region_utilization_pct = 75u;
    policy->max_task_utilization_pct = 75u;
    policy->max_obligation_utilization_pct = 75u;
    policy->max_io_utilization_pct = 75u;
    policy->max_blocking_utilization_pct = 75u;
    policy->max_ghost_violations = 0u;
    policy->max_deadline_miss_rate_pct100 = 100u;
    policy->max_watchdog_violations = 0u;
}

static uint32_t utilization_pct(uint32_t count, uint32_t capacity) {
    if (capacity == 0u) return 0u;
    return (count * 100u) / capacity;
}

asx_status asx_monitor_evaluate(const asx_runtime *rt, const asx_monitor_policy *policy,
                                asx_monitor_report *out, asx_evidence_sink *sink) {
    asx_monitor_policy defaults;
    asx_status st;
    uint32_t pct;

    if (rt == NULL || out == NULL || sink == NULL) return ASX_E_INVALID_ARGUMENT;
    if (policy == NULL) {
        asx_monitor_policy_init_default(&defaults);
        policy = &defaults;
    }

    st = asx_inspect(rt, &out->inspection);
    if (st != ASX_OK) return st;

    asx_evidence_sink_reset(sink);
    out->triggered_mask = 0u;

    if (!out->inspection.initialized) {
        out->verdict = ASX_EVIDENCE_FAIL;
        return asx_evidence_record(sink, "monitor:runtime", ASX_EVIDENCE_FAIL,
                                   "runtime not initialized", 0);
    }

    pct = utilization_pct(out->inspection.regions.active_count, out->inspection.regions.capacity);
    if (pct > policy->max_region_utilization_pct) {
        out->triggered_mask |= ASX_MONITOR_REGIONS_HIGH;
        st = asx_evidence_record(sink, "monitor:regions", ASX_EVIDENCE_WARN,
                                 "region utilization crossed threshold", 0);
        if (st != ASX_OK) return st;
    }

    pct = utilization_pct(out->inspection.tasks.active_count, out->inspection.tasks.capacity);
    if (pct > policy->max_task_utilization_pct) {
        out->triggered_mask |= ASX_MONITOR_TASKS_HIGH;
        st = asx_evidence_record(sink, "monitor:tasks", ASX_EVIDENCE_WARN,
                                 "task utilization crossed threshold", 0);
        if (st != ASX_OK) return st;
    }

    pct =
        utilization_pct(out->inspection.obligations.active_count, out->inspection.obligations.capacity);
    if (pct > policy->max_obligation_utilization_pct) {
        out->triggered_mask |= ASX_MONITOR_OBLIGATIONS_HIGH;
        st = asx_evidence_record(sink, "monitor:obligations", ASX_EVIDENCE_WARN,
                                 "obligation utilization crossed threshold", 0);
        if (st != ASX_OK) return st;
    }

    pct = utilization_pct(out->inspection.io_driver.active_count, out->inspection.io_driver.capacity);
    if (pct > policy->max_io_utilization_pct) {
        out->triggered_mask |= ASX_MONITOR_IO_HIGH;
        st = asx_evidence_record(sink, "monitor:io_driver", ASX_EVIDENCE_WARN,
                                 "io registration utilization crossed threshold", 0);
        if (st != ASX_OK) return st;
    }

    pct = utilization_pct(out->inspection.blocking.active_count, out->inspection.blocking.capacity);
    if (pct > policy->max_blocking_utilization_pct) {
        out->triggered_mask |= ASX_MONITOR_BLOCKING_HIGH;
        st = asx_evidence_record(sink, "monitor:blocking", ASX_EVIDENCE_WARN,
                                 "blocking utilization crossed threshold", 0);
        if (st != ASX_OK) return st;
    }

    if (out->inspection.ghosts.violation_count > policy->max_ghost_violations) {
        out->triggered_mask |= ASX_MONITOR_GHOSTS_PRESENT;
        st = asx_evidence_record(sink, "monitor:ghosts", ASX_EVIDENCE_FAIL,
                                 "ghost monitor violations present", 0);
        if (st != ASX_OK) return st;
    }

    {
        asx_auto_compliance_gate gate;
        asx_auto_deadline_tracker *dt = asx_auto_deadline_global();
        asx_auto_watchdog *wd = asx_auto_watchdog_global();

        asx_auto_compliance_gate_init(&gate);
        gate.max_miss_rate_pct100 = policy->max_deadline_miss_rate_pct100;
        gate.max_watchdog_violations = policy->max_watchdog_violations;
        asx_auto_compliance_evaluate(&gate, dt, wd, &out->compliance);

        if (out->compliance.actual_miss_rate > gate.max_miss_rate_pct100) {
            out->triggered_mask |= ASX_MONITOR_DEADLINE_MISS_RATE;
            st = asx_evidence_record(sink, "monitor:deadline", ASX_EVIDENCE_WARN,
                                     "deadline miss rate crossed threshold", 0);
            if (st != ASX_OK) return st;
        }

        if (out->compliance.actual_violations > gate.max_watchdog_violations) {
            out->triggered_mask |= ASX_MONITOR_WATCHDOG_VIOLATIONS;
            st = asx_evidence_record(sink, "monitor:watchdog", ASX_EVIDENCE_FAIL,
                                     "watchdog violations crossed threshold", 0);
            if (st != ASX_OK) return st;
        }
    }

    if (sink->count == 0u) {
        st = asx_evidence_record(sink, "monitor:healthy", ASX_EVIDENCE_PASS,
                                 "all monitor thresholds within policy", 0);
        if (st != ASX_OK) return st;
    }

    out->verdict = asx_evidence_verdict(sink);
    return ASX_OK;
}
