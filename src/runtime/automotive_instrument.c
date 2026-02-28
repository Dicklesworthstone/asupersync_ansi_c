/*
 * automotive_instrument.c — automotive profile instrumentation (bd-j4m.4)
 *
 * Implements deadline tracking, watchdog checkpoint monitoring,
 * degraded-mode audit logging, and compliance gate evaluation.
 *
 * All operations are single-threaded consistent with the asx
 * runtime threading model.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/runtime/automotive_instrument.h>
#include <string.h>

/* -------------------------------------------------------------------
 * Deadline tracker
 * ------------------------------------------------------------------- */

void asx_auto_deadline_init(asx_auto_deadline_tracker *dt)
{
    if (!dt) return;
    memset(dt, 0, sizeof(*dt));
}

void asx_auto_deadline_record(asx_auto_deadline_tracker *dt,
                               uint64_t deadline_ns,
                               uint64_t actual_ns)
{
    int64_t margin;

    if (!dt) return;

    dt->total_deadlines++;
    margin = (int64_t)(deadline_ns - actual_ns);

    if (actual_ns <= deadline_ns) {
        dt->deadline_hits++;
    } else {
        dt->deadline_misses++;
    }

    if (margin < dt->worst_margin_ns || dt->total_deadlines == 1) {
        dt->worst_margin_ns = margin;
    }
    if (margin > dt->best_margin_ns || dt->total_deadlines == 1) {
        dt->best_margin_ns = margin;
    }

    /* Accumulate absolute margin for mean computation */
    {
        uint64_t abs_margin = margin >= 0
            ? (uint64_t)margin
            : (uint64_t)(-margin);
        dt->total_margin_ns += abs_margin;
        dt->total_margin_count++;
    }
}

uint32_t asx_auto_deadline_miss_rate(const asx_auto_deadline_tracker *dt)
{
    if (!dt || dt->total_deadlines == 0) return 0;
    /* Return miss rate as percentage * 100 (e.g., 250 = 2.5%) */
    return (dt->deadline_misses * 10000u) / dt->total_deadlines;
}

void asx_auto_deadline_reset(asx_auto_deadline_tracker *dt)
{
    asx_auto_deadline_init(dt);
}

/* -------------------------------------------------------------------
 * Watchdog monitor
 * ------------------------------------------------------------------- */

void asx_auto_watchdog_init(asx_auto_watchdog *wd, uint64_t period_ns)
{
    if (!wd) return;
    memset(wd, 0, sizeof(*wd));
    wd->watchdog_period_ns = period_ns;
}

void asx_auto_watchdog_checkpoint(asx_auto_watchdog *wd, uint64_t now_ns)
{
    if (!wd) return;

    wd->total_checkpoints++;

    if (wd->armed) {
        uint64_t interval = now_ns - wd->last_checkpoint_ns;
        if (interval > wd->worst_interval_ns) {
            wd->worst_interval_ns = interval;
        }
        if (wd->watchdog_period_ns > 0 && interval > wd->watchdog_period_ns) {
            wd->violations++;
        }
    }

    wd->last_checkpoint_ns = now_ns;
    wd->armed = 1;
}

int asx_auto_watchdog_would_trigger(const asx_auto_watchdog *wd,
                                     uint64_t now_ns)
{
    uint64_t interval;
    if (!wd || !wd->armed || wd->watchdog_period_ns == 0) return 0;
    interval = now_ns - wd->last_checkpoint_ns;
    return interval > wd->watchdog_period_ns ? 1 : 0;
}

void asx_auto_watchdog_reset(asx_auto_watchdog *wd)
{
    uint64_t period;
    if (!wd) return;
    period = wd->watchdog_period_ns;
    asx_auto_watchdog_init(wd, period);
}

/* -------------------------------------------------------------------
 * Audit ring
 * ------------------------------------------------------------------- */

static const char *g_audit_kind_names[] = {
    "REGION_POISONED",
    "CANCEL_FORCED",
    "DEADLINE_MISS",
    "WATCHDOG_VIOLATION",
    "DEGRADED_ENTER",
    "DEGRADED_EXIT",
    "CHECKPOINT_OK"
};

void asx_auto_audit_init(asx_auto_audit_ring *ring)
{
    if (!ring) return;
    memset(ring, 0, sizeof(*ring));
}

void asx_auto_audit_record(asx_auto_audit_ring *ring,
                            asx_audit_kind kind,
                            uint64_t timestamp_ns,
                            uint64_t entity_id,
                            int64_t detail)
{
    asx_audit_entry *e;

    if (!ring) return;

    e = &ring->entries[ring->head % ASX_AUTO_AUDIT_RING_SIZE];
    e->seq = ring->next_seq++;
    e->kind = kind;
    e->timestamp_ns = timestamp_ns;
    e->entity_id = entity_id;
    e->detail = detail;

    ring->head = (ring->head + 1u) % ASX_AUTO_AUDIT_RING_SIZE;
    ring->count++;
}

const asx_audit_entry *asx_auto_audit_get(
    const asx_auto_audit_ring *ring, uint32_t index)
{
    uint32_t available;
    uint32_t start;

    if (!ring) return NULL;

    available = ring->count < ASX_AUTO_AUDIT_RING_SIZE
              ? ring->count : ASX_AUTO_AUDIT_RING_SIZE;

    if (index >= available) return NULL;

    start = ring->count <= ASX_AUTO_AUDIT_RING_SIZE
          ? 0
          : ring->head;  /* oldest entry is at head when wrapped */

    return &ring->entries[(start + index) % ASX_AUTO_AUDIT_RING_SIZE];
}

uint32_t asx_auto_audit_count(const asx_auto_audit_ring *ring)
{
    if (!ring) return 0;
    return ring->count < ASX_AUTO_AUDIT_RING_SIZE
         ? ring->count : ASX_AUTO_AUDIT_RING_SIZE;
}

uint32_t asx_auto_audit_total(const asx_auto_audit_ring *ring)
{
    if (!ring) return 0;
    return ring->count;
}

const char *asx_audit_kind_str(asx_audit_kind kind)
{
    if ((unsigned)kind < ASX_AUDIT_KIND_COUNT) {
        return g_audit_kind_names[(unsigned)kind];
    }
    return "UNKNOWN";
}

void asx_auto_audit_reset(asx_auto_audit_ring *ring)
{
    asx_auto_audit_init(ring);
}

/* -------------------------------------------------------------------
 * Compliance gate
 * ------------------------------------------------------------------- */

void asx_auto_compliance_gate_init(asx_auto_compliance_gate *gate)
{
    if (!gate) return;
    /* Default automotive thresholds:
     * - max 1.0% deadline miss rate
     * - max 0 watchdog violations
     * - at least 1 checkpoint expected */
    gate->max_miss_rate_pct100 = 100;   /* 1.0% */
    gate->max_watchdog_violations = 0;
    gate->min_checkpoints = 1;
}

void asx_auto_compliance_evaluate(
    const asx_auto_compliance_gate *gate,
    const asx_auto_deadline_tracker *dt,
    const asx_auto_watchdog *wd,
    asx_auto_compliance_result *result)
{
    if (!gate || !result) return;

    memset(result, 0, sizeof(*result));
    result->pass = 1;

    /* Deadline miss rate check */
    if (dt) {
        result->actual_miss_rate = asx_auto_deadline_miss_rate(dt);
        if (result->actual_miss_rate > gate->max_miss_rate_pct100) {
            result->violation_mask |= ASX_COMPLIANCE_DEADLINE_RATE;
            result->pass = 0;
        }
    }

    /* Watchdog violation check */
    if (wd) {
        result->actual_violations = wd->violations;
        result->actual_checkpoints = wd->total_checkpoints;

        if (wd->violations > gate->max_watchdog_violations) {
            result->violation_mask |= ASX_COMPLIANCE_WATCHDOG;
            result->pass = 0;
        }

        if (wd->total_checkpoints < gate->min_checkpoints) {
            result->violation_mask |= ASX_COMPLIANCE_CHECKPOINT_MIN;
            result->pass = 0;
        }
    }
}

/* -------------------------------------------------------------------
 * Global instrumentation state
 * ------------------------------------------------------------------- */

static asx_auto_deadline_tracker g_deadline;
static asx_auto_watchdog         g_watchdog;
static asx_auto_audit_ring       g_audit;
static int                       g_auto_initialized = 0;

static void ensure_auto_init(void)
{
    if (!g_auto_initialized) {
        asx_auto_deadline_init(&g_deadline);
        asx_auto_watchdog_init(&g_watchdog, 10000000u); /* 10ms default period */
        asx_auto_audit_init(&g_audit);
        g_auto_initialized = 1;
    }
}

void asx_auto_instrument_reset(void)
{
    asx_auto_deadline_init(&g_deadline);
    asx_auto_watchdog_init(&g_watchdog, 10000000u);
    asx_auto_audit_init(&g_audit);
    g_auto_initialized = 1;
}

asx_auto_deadline_tracker *asx_auto_deadline_global(void)
{
    ensure_auto_init();
    return &g_deadline;
}

asx_auto_watchdog *asx_auto_watchdog_global(void)
{
    ensure_auto_init();
    return &g_watchdog;
}

asx_auto_audit_ring *asx_auto_audit_global(void)
{
    ensure_auto_init();
    return &g_audit;
}

void asx_auto_record_deadline(uint64_t deadline_ns,
                               uint64_t actual_ns,
                               uint64_t entity_id)
{
    ensure_auto_init();
    asx_auto_deadline_record(&g_deadline, deadline_ns, actual_ns);

    /* Auto-log misses to audit ring */
    if (actual_ns > deadline_ns) {
        int64_t margin = (int64_t)(deadline_ns - actual_ns);
        asx_auto_audit_record(&g_audit, ASX_AUDIT_DEADLINE_MISS,
                               actual_ns, entity_id, margin);
    }
}

void asx_auto_record_checkpoint(uint64_t now_ns, uint64_t entity_id)
{
    int would_trigger;

    ensure_auto_init();

    /* Check if this checkpoint would trigger a violation before recording */
    would_trigger = asx_auto_watchdog_would_trigger(&g_watchdog, now_ns);

    asx_auto_watchdog_checkpoint(&g_watchdog, now_ns);

    /* Auto-log violations to audit ring */
    if (would_trigger) {
        uint64_t interval = now_ns - g_watchdog.last_checkpoint_ns;
        asx_auto_audit_record(&g_audit, ASX_AUDIT_WATCHDOG_VIOLATION,
                               now_ns, entity_id, (int64_t)interval);
    }
}
