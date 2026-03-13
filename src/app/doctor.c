/*
 * doctor.c — runtime diagnostic checks and health reports
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/app/doctor.h>
#include <asx/runtime/rt.h>
#include <asx/runtime/runtime.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Check helpers                                                       */
/* ------------------------------------------------------------------ */

static void add_check(asx_doctor_report *r, const char *name, asx_doctor_severity sev,
                      const char *msg, uint32_t value, uint32_t capacity) {
    if (r->check_count >= ASX_DOCTOR_MAX_CHECKS) return;

    r->checks[r->check_count].name = name;
    r->checks[r->check_count].severity = sev;
    r->checks[r->check_count].message = msg;
    r->checks[r->check_count].value = value;
    r->checks[r->check_count].capacity = capacity;
    r->check_count++;

    switch (sev) {
    case ASX_DOCTOR_OK: r->pass_count++; break;
    case ASX_DOCTOR_WARN: r->warn_count++; break;
    case ASX_DOCTOR_FAIL: r->fail_count++; break;
    }
}

/* ------------------------------------------------------------------ */
/* Diagnostic checks                                                   */
/* ------------------------------------------------------------------ */

static void check_runtime_initialized(const asx_runtime *rt, asx_doctor_report *r) {
    if (asx_runtime_is_initialized(rt)) {
        add_check(r, "runtime", ASX_DOCTOR_OK, "runtime initialized", 1, 1);
    } else {
        add_check(r, "runtime", ASX_DOCTOR_FAIL, "runtime not initialized", 0, 1);
    }
}

static void check_region_utilization(const asx_runtime *rt, asx_doctor_report *r) {
    uint32_t count = asx_runtime_region_count(rt);
    uint32_t cap = asx_runtime_region_capacity();
    asx_doctor_severity sev;

    if (count == 0) {
        sev = ASX_DOCTOR_OK;
    } else if (count >= cap) {
        sev = ASX_DOCTOR_FAIL;
    } else if (count > cap * 3 / 4) {
        sev = ASX_DOCTOR_WARN;
    } else {
        sev = ASX_DOCTOR_OK;
    }

    add_check(r, "regions", sev,
              count >= cap          ? "region arena exhausted"
              : count > cap * 3 / 4 ? "region arena >75% utilized"
                                    : "region utilization normal",
              count, cap);
}

static void check_task_utilization(const asx_runtime *rt, asx_doctor_report *r) {
    uint32_t count = asx_runtime_task_count(rt);
    uint32_t cap = asx_runtime_task_capacity();
    asx_doctor_severity sev;

    if (count == 0) {
        sev = ASX_DOCTOR_OK;
    } else if (count >= cap) {
        sev = ASX_DOCTOR_FAIL;
    } else if (count > cap * 3 / 4) {
        sev = ASX_DOCTOR_WARN;
    } else {
        sev = ASX_DOCTOR_OK;
    }

    add_check(r, "tasks", sev,
              count >= cap          ? "task arena exhausted"
              : count > cap * 3 / 4 ? "task arena >75% utilized"
                                    : "task utilization normal",
              count, cap);
}

static void check_obligation_utilization(const asx_runtime *rt, asx_doctor_report *r) {
    uint32_t count = asx_runtime_obligation_count(rt);
    uint32_t cap = asx_runtime_obligation_capacity();
    asx_doctor_severity sev;

    if (count >= cap) {
        sev = ASX_DOCTOR_FAIL;
    } else if (count > cap * 3 / 4) {
        sev = ASX_DOCTOR_WARN;
    } else {
        sev = ASX_DOCTOR_OK;
    }

    add_check(r, "obligations", sev,
              count >= cap          ? "obligation arena exhausted"
              : count > cap * 3 / 4 ? "obligation arena >75% utilized"
                                    : "obligation utilization normal",
              count, cap);
}

static void check_safety_profile(const asx_runtime *rt, asx_doctor_report *r) {
    asx_safety_profile prof = asx_runtime_safety_profile(rt);

    add_check(r, "safety_profile", ASX_DOCTOR_OK,
              prof == ASX_SAFETY_DEBUG      ? "debug (full ghost monitors)"
              : prof == ASX_SAFETY_HARDENED ? "hardened (error ledger)"
                                            : "release (zero-cost stubs)",
              (uint32_t)prof, 2);
}

static void check_containment_policy(const asx_runtime *rt, asx_doctor_report *r) {
    asx_containment_policy pol = asx_runtime_containment_policy(rt);

    add_check(r, "containment", ASX_DOCTOR_OK,
              pol == ASX_CONTAIN_FAIL_FAST       ? "fail-fast"
              : pol == ASX_CONTAIN_POISON_REGION ? "poison-region"
                                                 : "error-only",
              (uint32_t)pol, 2);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

asx_status asx_doctor_run(const asx_runtime *rt, asx_doctor_report *report) {
    if (rt == NULL || report == NULL) return ASX_E_INVALID_ARGUMENT;

    memset(report, 0, sizeof(*report));

    check_runtime_initialized(rt, report);
    check_region_utilization(rt, report);
    check_task_utilization(rt, report);
    check_obligation_utilization(rt, report);
    check_safety_profile(rt, report);
    check_containment_policy(rt, report);

    return ASX_OK;
}

int asx_doctor_is_healthy(const asx_doctor_report *report) {
    if (report == NULL) return 0;
    return report->fail_count == 0;
}

asx_doctor_severity asx_doctor_overall(const asx_doctor_report *report) {
    if (report == NULL) return ASX_DOCTOR_FAIL;
    if (report->fail_count > 0) return ASX_DOCTOR_FAIL;
    if (report->warn_count > 0) return ASX_DOCTOR_WARN;
    return ASX_DOCTOR_OK;
}
