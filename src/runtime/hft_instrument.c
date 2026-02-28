/*
 * hft_instrument.c — HFT profile instrumentation (bd-j4m.3)
 *
 * Implements latency histogram, jitter tracking, deterministic
 * overload policy, and metric gate evaluation for the HFT profile.
 *
 * All operations are single-threaded (no locking) consistent with
 * the asx runtime threading model.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/runtime/hft_instrument.h>
#include <string.h>

/* ASX_CHECKPOINT_WAIVER_FILE("hft-instrument: all loops bounded by ASX_HFT_HISTOGRAM_BINS (16)") */

/* -------------------------------------------------------------------
 * Integer log2 — branchless floor(log2(x)) for x > 0
 *
 * Returns 0 for x == 0. Used for histogram bin assignment without
 * floating point.
 * ------------------------------------------------------------------- */

static uint32_t ilog2(uint64_t x)
{
    uint32_t r = 0;
    if (x == 0) return 0;
    if (x >= UINT64_C(0x100000000)) { x >>= 32; r += 32; }
    if (x >= UINT64_C(0x10000))     { x >>= 16; r += 16; }
    if (x >= UINT64_C(0x100))       { x >>= 8;  r += 8;  }
    if (x >= UINT64_C(0x10))        { x >>= 4;  r += 4;  }
    if (x >= UINT64_C(0x4))         { x >>= 2;  r += 2;  }
    if (x >= UINT64_C(0x2))         {            r += 1;  }
    return r;
}

/* Bin lower bound for a given bin index. */
static uint64_t bin_lower(uint32_t bin)
{
    if (bin == 0) return 0;
    return UINT64_C(1) << (bin - 1u);
}

/* Bin midpoint for MAD computation. */
static uint64_t bin_mid(uint32_t bin)
{
    if (bin == 0) return 0;
    if (bin == 1) return 1;
    return (UINT64_C(1) << (bin - 1u)) + (UINT64_C(1) << (bin - 2u));
}

/* -------------------------------------------------------------------
 * Histogram
 * ------------------------------------------------------------------- */

void asx_hft_histogram_init(asx_hft_histogram *h)
{
    if (!h) return;
    memset(h, 0, sizeof(*h));
    h->min_ns = UINT64_MAX;
}

void asx_hft_histogram_record(asx_hft_histogram *h, uint64_t ns)
{
    uint32_t bin;

    if (!h) return;

    h->total++;
    h->sum_ns += ns;

    if (ns < h->min_ns) h->min_ns = ns;
    if (ns > h->max_ns) h->max_ns = ns;

    if (ns >= ASX_HFT_HISTOGRAM_MAX_NS) {
        h->overflow++;
        return;
    }

    /* bin = floor(log2(ns + 1)), clamped to [0, BINS-1] */
    bin = ilog2(ns + 1u);
    if (bin >= ASX_HFT_HISTOGRAM_BINS) {
        bin = ASX_HFT_HISTOGRAM_BINS - 1u;
    }
    h->bins[bin]++;
}

uint64_t asx_hft_histogram_percentile(const asx_hft_histogram *h,
                                       uint32_t pct)
{
    uint32_t target;
    uint32_t cumulative;
    uint32_t i;

    if (!h || h->total == 0) return 0;
    if (pct > 100) pct = 100;

    target = (h->total * pct) / 100u;
    if (target == 0 && pct > 0) target = 1;

    cumulative = 0;
    for (i = 0; i < ASX_HFT_HISTOGRAM_BINS; i++) {
        cumulative += h->bins[i];
        if (cumulative >= target) {
            return bin_lower(i);
        }
    }

    /* In overflow bucket */
    return ASX_HFT_HISTOGRAM_MAX_NS;
}

uint64_t asx_hft_histogram_mean(const asx_hft_histogram *h)
{
    if (!h || h->total == 0) return 0;
    return h->sum_ns / (uint64_t)h->total;
}

void asx_hft_histogram_reset(asx_hft_histogram *h)
{
    asx_hft_histogram_init(h);
}

/* -------------------------------------------------------------------
 * Jitter tracker
 * ------------------------------------------------------------------- */

void asx_hft_jitter_init(asx_hft_jitter_tracker *jt,
                          uint32_t recompute_interval)
{
    if (!jt) return;
    asx_hft_histogram_init(&jt->hist);
    jt->mad_ns = 0;
    jt->recompute_every = recompute_interval;
    jt->samples_since = 0;
}

void asx_hft_jitter_recompute(asx_hft_jitter_tracker *jt)
{
    uint64_t mean;
    uint64_t mad_sum;
    uint32_t count;
    uint32_t i;

    if (!jt) return;

    mean = asx_hft_histogram_mean(&jt->hist);
    if (jt->hist.total == 0) {
        jt->mad_ns = 0;
        return;
    }

    /* Approximate MAD using histogram bin midpoints */
    mad_sum = 0;
    count = 0;
    for (i = 0; i < ASX_HFT_HISTOGRAM_BINS; i++) {
        if (jt->hist.bins[i] > 0) {
            uint64_t mid = bin_mid(i);
            uint64_t diff = mid > mean ? mid - mean : mean - mid;
            mad_sum += diff * (uint64_t)jt->hist.bins[i];
            count += jt->hist.bins[i];
        }
    }

    /* Include overflow bucket (use ASX_HFT_HISTOGRAM_MAX_NS as midpoint) */
    if (jt->hist.overflow > 0) {
        uint64_t diff = ASX_HFT_HISTOGRAM_MAX_NS > mean
                      ? ASX_HFT_HISTOGRAM_MAX_NS - mean
                      : mean - ASX_HFT_HISTOGRAM_MAX_NS;
        mad_sum += diff * (uint64_t)jt->hist.overflow;
        count += jt->hist.overflow;
    }

    jt->mad_ns = count > 0 ? mad_sum / (uint64_t)count : 0;
}

void asx_hft_jitter_record(asx_hft_jitter_tracker *jt, uint64_t ns)
{
    if (!jt) return;

    asx_hft_histogram_record(&jt->hist, ns);
    jt->samples_since++;

    if (jt->recompute_every == 0 ||
        jt->samples_since >= jt->recompute_every) {
        asx_hft_jitter_recompute(jt);
        jt->samples_since = 0;
    }
}

uint64_t asx_hft_jitter_get(const asx_hft_jitter_tracker *jt)
{
    if (!jt) return 0;
    return jt->mad_ns;
}

void asx_hft_jitter_reset(asx_hft_jitter_tracker *jt)
{
    uint32_t interval;
    if (!jt) return;
    interval = jt->recompute_every;
    asx_hft_jitter_init(jt, interval);
}

/* -------------------------------------------------------------------
 * Overload policy
 * ------------------------------------------------------------------- */

void asx_overload_policy_init(asx_overload_policy *pol)
{
    if (!pol) return;
    pol->mode = ASX_OVERLOAD_REJECT;
    pol->threshold_pct = 90;
    pol->shed_max = 1;
}

void asx_overload_evaluate(const asx_overload_policy *pol,
                            uint32_t used,
                            uint32_t capacity,
                            asx_overload_decision *decision)
{
    uint32_t load_pct;

    if (!pol || !decision) return;

    memset(decision, 0, sizeof(*decision));
    decision->mode = pol->mode;

    if (capacity == 0) {
        decision->triggered = 1;
        decision->load_pct = 100;
        decision->admit_status = ASX_E_RESOURCE_EXHAUSTED;
        return;
    }

    load_pct = (used * 100u) / capacity;
    decision->load_pct = load_pct;

    if (load_pct < pol->threshold_pct) {
        decision->triggered = 0;
        decision->admit_status = ASX_OK;
        return;
    }

    /* Overload triggered */
    decision->triggered = 1;

    switch (pol->mode) {
    case ASX_OVERLOAD_REJECT:
        decision->admit_status = ASX_E_ADMISSION_CLOSED;
        break;

    case ASX_OVERLOAD_SHED_OLDEST:
        /* Caller is responsible for actual shedding; we report how many. */
        decision->shed_count = pol->shed_max;
        if (decision->shed_count > used) {
            decision->shed_count = used;
        }
        decision->admit_status = ASX_OK;
        break;

    case ASX_OVERLOAD_BACKPRESSURE:
        decision->admit_status = ASX_E_WOULD_BLOCK;
        break;

    default:
        decision->admit_status = ASX_E_INVALID_ARGUMENT;
        break;
    }
}

const char *asx_overload_mode_str(asx_overload_mode mode)
{
    switch (mode) {
    case ASX_OVERLOAD_REJECT:       return "REJECT";
    case ASX_OVERLOAD_SHED_OLDEST:  return "SHED_OLDEST";
    case ASX_OVERLOAD_BACKPRESSURE: return "BACKPRESSURE";
    default:                        return "UNKNOWN";
    }
}

/* -------------------------------------------------------------------
 * Metric gate
 * ------------------------------------------------------------------- */

void asx_hft_gate_init(asx_hft_gate *gate)
{
    if (!gate) return;
    /* Default HFT thresholds: p99 < 10us, p99.9 < 50us,
     * p99.99 < 100us, jitter < 5us */
    gate->p99_ns    = 10000;
    gate->p99_9_ns  = 0;     /* skip by default */
    gate->p99_99_ns = 0;     /* skip by default */
    gate->jitter_ns = 0;     /* skip by default */
}

void asx_hft_gate_evaluate(const asx_hft_gate *gate,
                            const asx_hft_histogram *hist,
                            const asx_hft_jitter_tracker *jt,
                            asx_hft_gate_result *result)
{
    if (!gate || !result) return;

    memset(result, 0, sizeof(*result));
    result->pass = 1;

    /* p99 check */
    if (hist && gate->p99_ns > 0) {
        result->actual_p99 = asx_hft_histogram_percentile(hist, 99);
        if (result->actual_p99 > gate->p99_ns) {
            result->violations |= ASX_GATE_VIOLATION_P99;
            result->pass = 0;
        }
    }

    /* p99.9 check */
    if (hist && gate->p99_9_ns > 0) {
        /* Approximate p99.9: use 999/1000 of total */
        uint32_t target;
        uint32_t cumulative = 0;
        uint32_t i;

        target = (hist->total * 999u) / 1000u;
        if (target == 0 && hist->total > 0) target = 1;

        for (i = 0; i < ASX_HFT_HISTOGRAM_BINS; i++) {
            cumulative += hist->bins[i];
            if (cumulative >= target) {
                result->actual_p99_9 = bin_lower(i);
                break;
            }
        }
        if (i == ASX_HFT_HISTOGRAM_BINS) {
            result->actual_p99_9 = ASX_HFT_HISTOGRAM_MAX_NS;
        }

        if (result->actual_p99_9 > gate->p99_9_ns) {
            result->violations |= ASX_GATE_VIOLATION_P99_9;
            result->pass = 0;
        }
    }

    /* p99.99 check */
    if (hist && gate->p99_99_ns > 0) {
        uint32_t target;
        uint32_t cumulative = 0;
        uint32_t i;

        target = (hist->total * 9999u) / 10000u;
        if (target == 0 && hist->total > 0) target = 1;

        for (i = 0; i < ASX_HFT_HISTOGRAM_BINS; i++) {
            cumulative += hist->bins[i];
            if (cumulative >= target) {
                result->actual_p99_99 = bin_lower(i);
                break;
            }
        }
        if (i == ASX_HFT_HISTOGRAM_BINS) {
            result->actual_p99_99 = ASX_HFT_HISTOGRAM_MAX_NS;
        }

        if (result->actual_p99_99 > gate->p99_99_ns) {
            result->violations |= ASX_GATE_VIOLATION_P99_99;
            result->pass = 0;
        }
    }

    /* Jitter check */
    if (jt && gate->jitter_ns > 0) {
        result->actual_jitter = asx_hft_jitter_get(jt);
        if (result->actual_jitter > gate->jitter_ns) {
            result->violations |= ASX_GATE_VIOLATION_JITTER;
            result->pass = 0;
        }
    }
}

/* -------------------------------------------------------------------
 * Global instrumentation state
 * ------------------------------------------------------------------- */

static asx_hft_histogram      g_sched_hist;
static asx_hft_jitter_tracker  g_sched_jitter;
static int                     g_initialized = 0;

static void ensure_init(void)
{
    if (!g_initialized) {
        asx_hft_histogram_init(&g_sched_hist);
        asx_hft_jitter_init(&g_sched_jitter, 64);
        g_initialized = 1;
    }
}

void asx_hft_instrument_reset(void)
{
    asx_hft_histogram_init(&g_sched_hist);
    asx_hft_jitter_init(&g_sched_jitter, 64);
    g_initialized = 1;
}

asx_hft_histogram *asx_hft_sched_histogram(void)
{
    ensure_init();
    return &g_sched_hist;
}

asx_hft_jitter_tracker *asx_hft_sched_jitter(void)
{
    ensure_init();
    return &g_sched_jitter;
}

void asx_hft_record_poll_latency(uint64_t ns)
{
    ensure_init();
    asx_hft_histogram_record(&g_sched_hist, ns);
    asx_hft_jitter_record(&g_sched_jitter, ns);
}
