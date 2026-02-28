/*
 * vertical_adapter.c — vertical acceleration adapters (bd-j4m.5)
 *
 * Implements domain-specific overload adapters with catalog-aligned
 * fallback paths and machine-checkable isomorphism proofs.
 *
 * SPDX-License-Identifier: MIT
 */

/* ASX_CHECKPOINT_WAIVER_FILE() — bounded iteration over small fixed arrays */

#include <asx/runtime/vertical_adapter.h>
#include <string.h>

/* -------------------------------------------------------------------
 * Internal: FNV-1a decision digest
 * ------------------------------------------------------------------- */

#define FNV_OFFSET 14695981039346656037ULL
#define FNV_PRIME  1099511628211ULL

static uint64_t fnv_u32(uint64_t h, uint32_t v)
{
    const unsigned char *p = (const unsigned char *)&v;
    int i;
    for (i = 0; i < 4; i++) {
        h ^= (uint64_t)p[i];
        h *= FNV_PRIME;
    }
    return h;
}

static uint64_t compute_decision_digest(const asx_overload_decision *d)
{
    uint64_t h = FNV_OFFSET;
    h = fnv_u32(h, (uint32_t)d->triggered);
    h = fnv_u32(h, (uint32_t)d->mode);
    h = fnv_u32(h, d->load_pct);
    h = fnv_u32(h, d->shed_count);
    h = fnv_u32(h, (uint32_t)d->admit_status);
    return h;
}

/* -------------------------------------------------------------------
 * Internal: router reject streak (module-level state)
 * ------------------------------------------------------------------- */

static uint32_t g_router_reject_streak = 0;

/* -------------------------------------------------------------------
 * Descriptor table
 * ------------------------------------------------------------------- */

static const asx_adapter_descriptor g_descriptors[ASX_ADAPTER_COUNT] = {
    {
        ASX_ADAPTER_HFT,
        ASX_PROFILE_ID_HFT,
        "HFT_LATENCY",
        "Tail-latency optimized adapter with shed-oldest overload policy"
    },
    {
        ASX_ADAPTER_AUTOMOTIVE,
        ASX_PROFILE_ID_AUTOMOTIVE,
        "AUTO_COMPLIANCE",
        "Deadline-aware adapter with backpressure and compliance tracking"
    },
    {
        ASX_ADAPTER_ROUTER,
        ASX_PROFILE_ID_EMBEDDED_ROUTER,
        "ROUTER_QUEUE",
        "Resource-class-aware adapter with aggressive queue depth control"
    }
};

/* -------------------------------------------------------------------
 * Query API
 * ------------------------------------------------------------------- */

uint32_t asx_adapter_count(void)
{
    return (uint32_t)ASX_ADAPTER_COUNT;
}

asx_status asx_adapter_get_descriptor(asx_adapter_id id,
                                       asx_adapter_descriptor *out)
{
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    if ((unsigned)id >= (unsigned)ASX_ADAPTER_COUNT)
        return ASX_E_INVALID_ARGUMENT;
    *out = g_descriptors[id];
    return ASX_OK;
}

const char *asx_adapter_name(asx_adapter_id id)
{
    if ((unsigned)id >= (unsigned)ASX_ADAPTER_COUNT) return "UNKNOWN";
    return g_descriptors[id].name;
}

const char *asx_adapter_mode_str(asx_adapter_mode mode)
{
    switch (mode) {
    case ASX_ADAPTER_MODE_FALLBACK:    return "FALLBACK";
    case ASX_ADAPTER_MODE_ACCELERATED: return "ACCELERATED";
    }
    return "UNKNOWN";
}

asx_profile_id asx_adapter_profile(asx_adapter_id id)
{
    if ((unsigned)id >= (unsigned)ASX_ADAPTER_COUNT)
        return ASX_PROFILE_ID_COUNT;
    return g_descriptors[id].profile;
}

/* -------------------------------------------------------------------
 * Internal: evaluate using catalog policy for a profile
 * ------------------------------------------------------------------- */

static void evaluate_via_catalog(asx_profile_id profile,
                                 uint32_t used, uint32_t capacity,
                                 asx_overload_decision *decision)
{
    asx_overload_policy pol;
    asx_overload_catalog_to_policy(profile, &pol);
    asx_overload_evaluate(&pol, used, capacity, decision);
}

/* -------------------------------------------------------------------
 * Internal: populate annotations
 * ------------------------------------------------------------------- */

static void populate_hft_annotations(asx_adapter_result *res)
{
    const asx_hft_histogram *h = asx_hft_sched_histogram();
    res->has_annotations = 1;
    res->annotations.hft.p99_ns = asx_hft_histogram_percentile(h, 99);
    res->annotations.hft.overflow_count = h->overflow;
}

static void populate_auto_annotations(asx_adapter_result *res)
{
    const asx_auto_deadline_tracker *dt = asx_auto_deadline_global();
    const asx_auto_audit_ring *ring = asx_auto_audit_global();
    res->has_annotations = 1;
    res->annotations.automotive.miss_rate_pct100 =
        asx_auto_deadline_miss_rate(dt);
    res->annotations.automotive.audit_count = asx_auto_audit_count(ring);
}

static void populate_router_annotations(asx_adapter_result *res,
                                        uint32_t used, uint32_t capacity,
                                        int triggered)
{
    res->has_annotations = 1;
    res->annotations.router.queue_depth = used;
    res->annotations.router.headroom =
        (capacity > used) ? capacity - used : 0;
    if (triggered) {
        g_router_reject_streak++;
    } else {
        g_router_reject_streak = 0;
    }
    res->annotations.router.reject_streak = g_router_reject_streak;
}

/* -------------------------------------------------------------------
 * Evaluation API
 * ------------------------------------------------------------------- */

asx_status asx_adapter_evaluate(asx_adapter_id id,
                                 asx_adapter_mode mode,
                                 uint32_t used,
                                 uint32_t capacity,
                                 asx_adapter_result *out)
{
    asx_profile_id profile;

    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    if ((unsigned)id >= (unsigned)ASX_ADAPTER_COUNT)
        return ASX_E_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));
    out->adapter = id;
    out->mode = mode;

    profile = g_descriptors[id].profile;
    evaluate_via_catalog(profile, used, capacity, &out->decision);
    out->decision_digest = compute_decision_digest(&out->decision);

    /* Populate annotations only in accelerated mode */
    if (mode == ASX_ADAPTER_MODE_ACCELERATED) {
        switch (id) {
        case ASX_ADAPTER_HFT:
            populate_hft_annotations(out);
            break;
        case ASX_ADAPTER_AUTOMOTIVE:
            populate_auto_annotations(out);
            break;
        case ASX_ADAPTER_ROUTER:
            populate_router_annotations(out, used, capacity,
                                        out->decision.triggered);
            break;
        case ASX_ADAPTER_COUNT:
            break;
        }
    }

    return ASX_OK;
}

asx_status asx_adapter_evaluate_both(asx_adapter_id id,
                                      uint32_t used,
                                      uint32_t capacity,
                                      asx_adapter_result *fallback_out,
                                      asx_adapter_result *accel_out)
{
    asx_status s;

    if (fallback_out == NULL || accel_out == NULL)
        return ASX_E_INVALID_ARGUMENT;

    s = asx_adapter_evaluate(id, ASX_ADAPTER_MODE_FALLBACK,
                              used, capacity, fallback_out);
    if (s != ASX_OK) return s;

    s = asx_adapter_evaluate(id, ASX_ADAPTER_MODE_ACCELERATED,
                              used, capacity, accel_out);
    return s;
}

void asx_adapter_reset_all(void)
{
    g_router_reject_streak = 0;
    asx_hft_instrument_reset();
    asx_auto_instrument_reset();
}

/* -------------------------------------------------------------------
 * Isomorphism proof
 *
 * Both modes use the same catalog policy, so decisions are always
 * identical. The proof verifies this for every scenario.
 * ------------------------------------------------------------------- */

int asx_adapter_isomorphism_builtin(asx_adapter_id id,
                                     asx_isomorphism_result *out)
{
    asx_adapter_scenario scenarios[101];
    uint32_t i;

    if (out == NULL) return 0;
    if ((unsigned)id >= (unsigned)ASX_ADAPTER_COUNT) {
        memset(out, 0, sizeof(*out));
        out->pass = 0;
        return 0;
    }

    for (i = 0; i <= 100; i++) {
        scenarios[i].used = i;
        scenarios[i].capacity = 100;
    }

    return asx_adapter_isomorphism_check(id, scenarios, 101, out);
}

int asx_adapter_isomorphism_check(asx_adapter_id id,
                                   const asx_adapter_scenario *scenarios,
                                   uint32_t count,
                                   asx_isomorphism_result *out)
{
    uint32_t i;
    uint32_t matches = 0;

    if (out == NULL) return 0;
    memset(out, 0, sizeof(*out));
    out->adapter = id;

    if (scenarios == NULL || count == 0) {
        out->pass = 0;
        return 0;
    }

    if ((unsigned)id >= (unsigned)ASX_ADAPTER_COUNT) {
        out->pass = 0;
        return 0;
    }

    out->evaluations = count;

    for (i = 0; i < count; i++) {
        asx_adapter_result fb, ac;
        uint32_t saved_streak = g_router_reject_streak;

        asx_adapter_evaluate(id, ASX_ADAPTER_MODE_FALLBACK,
                              scenarios[i].used, scenarios[i].capacity, &fb);

        /* Restore streak state so accelerated mode starts from same state */
        g_router_reject_streak = saved_streak;

        asx_adapter_evaluate(id, ASX_ADAPTER_MODE_ACCELERATED,
                              scenarios[i].used, scenarios[i].capacity, &ac);

        if (fb.decision_digest == ac.decision_digest) {
            matches++;
        } else {
            if (out->divergence_index == 0 && i > 0) {
                out->divergence_index = i;
            } else if (matches == i) {
                out->divergence_index = i;
            }
        }
    }

    out->matches = matches;
    out->pass = (matches == count) ? 1 : 0;
    return out->pass;
}
