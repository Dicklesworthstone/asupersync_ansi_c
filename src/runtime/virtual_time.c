/*
 * virtual_time.c — Virtual-time injection layer (bd-3vt.8)
 *
 * Implements programmed clock anomaly injection for deterministic
 * replay of jitter, stalls, and clock jumps.
 *
 * SPDX-License-Identifier: MIT
 */

/* ASX_CHECKPOINT_WAIVER_FILE() -- virtual-time spike, no checkpoint coverage needed */

#include <asx/asx.h>
#include <asx/runtime/virtual_time.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Initialization                                                      */
/* ------------------------------------------------------------------ */

void asx_vtime_init(asx_vtime_state *vt, asx_time base_ns, uint64_t tick_ns) {
    if (vt == NULL) return;

    memset(vt, 0, sizeof(*vt));
    vt->base_time = base_ns;
    vt->current_time = base_ns;
    vt->tick_ns = tick_ns;
}

void asx_vtime_reset(asx_vtime_state *vt) {
    if (vt == NULL) return;

    vt->current_time = vt->base_time;
    vt->query_count = 0;
    vt->stall_remaining = 0;
    /* anomaly schedule is preserved */
}

/* ------------------------------------------------------------------ */
/* Anomaly scheduling                                                  */
/* ------------------------------------------------------------------ */

static asx_status vtime_add_anomaly(asx_vtime_state *vt, asx_vtime_anomaly_kind kind,
                                    uint32_t trigger_query, int64_t param, uint32_t duration) {
    asx_vtime_anomaly *a;

    if (vt == NULL) return ASX_E_INVALID_ARGUMENT;
    if (vt->anomaly_count >= ASX_VTIME_MAX_ANOMALIES) { return ASX_E_RESOURCE_EXHAUSTED; }

    a = &vt->anomalies[vt->anomaly_count];
    a->kind = kind;
    a->trigger_query = trigger_query;
    a->param = param;
    a->duration = duration;
    vt->anomaly_count++;

    return ASX_OK;
}

asx_status asx_vtime_add_jitter(asx_vtime_state *vt, uint32_t trigger_query, int64_t delta_ns) {
    return vtime_add_anomaly(vt, ASX_VTIME_ANOMALY_JITTER, trigger_query, delta_ns, 0);
}

asx_status asx_vtime_add_stall(asx_vtime_state *vt, uint32_t trigger_query, uint32_t hold_queries) {
    return vtime_add_anomaly(vt, ASX_VTIME_ANOMALY_STALL, trigger_query, 0, hold_queries);
}

asx_status asx_vtime_add_jump(asx_vtime_state *vt, uint32_t trigger_query, uint64_t jump_ns) {
    return vtime_add_anomaly(vt, ASX_VTIME_ANOMALY_JUMP, trigger_query, (int64_t)jump_ns, 0);
}

/* ------------------------------------------------------------------ */
/* Virtual clock query                                                 */
/* ------------------------------------------------------------------ */

asx_time asx_vtime_now_ns(void *ctx) {
    asx_vtime_state *vt = (asx_vtime_state *)ctx;
    uint32_t i;
    uint32_t query;

    if (vt == NULL) return 0;

    query = vt->query_count;

    /* Check for active stall */
    if (vt->stall_remaining > 0) {
        vt->stall_remaining--;
        if (vt->query_count < UINT32_MAX) vt->query_count++;
        return vt->current_time; /* Time does not advance */
    }

    /* Normal tick advance with overflow saturation */
    if (query > 0) {
        if (vt->current_time <= UINT64_MAX - vt->tick_ns) {
            vt->current_time += vt->tick_ns;
        } else {
            vt->current_time = UINT64_MAX;
        }
    }

    /* Apply anomalies for this query */
    for (i = 0; i < vt->anomaly_count; i++) {
        const asx_vtime_anomaly *a = &vt->anomalies[i];

        if (a->trigger_query != query) continue;

        switch (a->kind) {
        case ASX_VTIME_ANOMALY_JITTER:
            if (a->param >= 0) {
                asx_time add = (asx_time)a->param;
                if (vt->current_time <= UINT64_MAX - add) {
                    vt->current_time += add;
                } else {
                    vt->current_time = UINT64_MAX;
                }
            } else {
                /* Avoid UB: widen before negation for INT64_MIN safety */
                uint64_t abs_delta;
                if (a->param == (-9223372036854775807LL - 1)) {
                    abs_delta = (uint64_t)9223372036854775808ULL;
                } else {
                    abs_delta = (uint64_t)(-(a->param));
                }
                if (vt->current_time >= abs_delta) {
                    vt->current_time -= abs_delta;
                } else {
                    vt->current_time = 0;
                }
            }
            break;

        case ASX_VTIME_ANOMALY_STALL:
            /* Begin stall: hold time for duration queries */
            vt->stall_remaining = a->duration;
            break;

        case ASX_VTIME_ANOMALY_JUMP: {
            asx_time jump = (asx_time)a->param;
            if (vt->current_time <= UINT64_MAX - jump) {
                vt->current_time += jump;
            } else {
                vt->current_time = UINT64_MAX;
            }
            break;
        }
        default: break;
        }
    }

    if (vt->query_count < UINT32_MAX) vt->query_count++;
    return vt->current_time;
}

/* ------------------------------------------------------------------ */
/* Install / uninstall                                                 */
/* ------------------------------------------------------------------ */

/*
 * Installation is intentionally lightweight: callers set up hooks
 * manually via asx_runtime_set_hooks() since the hook contract
 * requires deterministic validation. This function provides a
 * convenience for the common case.
 */

asx_status asx_vtime_install(asx_vtime_state *vt) {
    (void)vt;
    /*
     * Walking skeleton: callers install via asx_runtime_set_hooks()
     * with clock.logical_now_ns_fn = asx_vtime_now_ns and
     * clock.ctx = vt. This preserves the hook contract validation.
     *
     * A convenience wrapper would bypass hook validation, which
     * violates the architectural contract. Deferred.
     */
    return ASX_OK;
}

void asx_vtime_uninstall(asx_vtime_state *vt) {
    (void)vt;
    /* Symmetric with install — callers restore hooks manually */
}

/* ------------------------------------------------------------------ */
/* Queries                                                             */
/* ------------------------------------------------------------------ */

asx_time asx_vtime_current(const asx_vtime_state *vt) {
    if (vt == NULL) return 0;
    return vt->current_time;
}

uint32_t asx_vtime_query_count(const asx_vtime_state *vt) {
    if (vt == NULL) return 0;
    return vt->query_count;
}
