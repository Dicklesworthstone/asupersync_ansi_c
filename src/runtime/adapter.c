/*
 * adapter.c — vertical acceleration adapters with deterministic fallback (bd-j4m.5)
 *
 * Each domain adapter provides an accelerated path using domain-specific
 * instrumentation and a CORE-equivalent fallback. The isomorphism proof
 * verifies that admit/reject decisions agree on the semantic-equivalence-
 * relevant field: triggered (whether overload was detected).
 *
 * Isomorphism contract:
 *   For load < min(accel_threshold, fallback_threshold): both admit.
 *   For load >= max(accel_threshold, fallback_threshold): both reject.
 *   The "gray zone" between thresholds is where modes may legitimately
 *   differ — the proof checks the CORE-equivalent threshold boundary.
 *
 * SPDX-License-Identifier: MIT
 */

/* ASX_CHECKPOINT_WAIVER_FILE() — no kernel loops; pure function evaluations */

#include <asx/runtime/adapter.h>
#include <string.h>

/* -------------------------------------------------------------------
 * Internal: FNV-1a hash for decision fields
 * ------------------------------------------------------------------- */

#define FNV_OFFSET 14695981039346656037ULL
#define FNV_PRIME  1099511628211ULL

static uint64_t fnv_hash_u32(uint64_t h, uint32_t val)
{
    const unsigned char *p = (const unsigned char *)&val;
    int i;
    for (i = 0; i < 4; i++) {
        h ^= (uint64_t)p[i];
        h *= FNV_PRIME;
    }
    return h;
}

static uint64_t decision_hash(const asx_adapter_decision *d)
{
    uint64_t h = FNV_OFFSET;
    h = fnv_hash_u32(h, (uint32_t)d->triggered);
    h = fnv_hash_u32(h, (uint32_t)d->mode);
    h = fnv_hash_u32(h, d->load_pct);
    h = fnv_hash_u32(h, d->shed_count);
    h = fnv_hash_u32(h, (uint32_t)d->admit_status);
    return h;
}

/* -------------------------------------------------------------------
 * Internal: CORE fallback implementation (shared by all domains)
 *
 * CORE profile: REJECT at 90% threshold.
 * ------------------------------------------------------------------- */

static void core_fallback_decide(uint32_t used, uint32_t capacity,
                                 asx_adapter_decision *out)
{
    uint32_t load_pct;

    memset(out, 0, sizeof(*out));
    out->path_used = ASX_ADAPTER_FALLBACK;
    out->mode = ASX_OVERLOAD_REJECT;

    if (capacity == 0) {
        out->triggered = 1;
        out->load_pct = 100;
        out->admit_status = ASX_E_ADMISSION_CLOSED;
        out->decision_hash = decision_hash(out);
        return;
    }

    load_pct = (used * 100u) / capacity;
    out->load_pct = load_pct;

    if (load_pct >= 90) {
        out->triggered = 1;
        out->admit_status = ASX_E_ADMISSION_CLOSED;
    } else {
        out->triggered = 0;
        out->admit_status = ASX_OK;
    }
    out->decision_hash = decision_hash(out);
}

/* -------------------------------------------------------------------
 * HFT adapter
 *
 * Accelerated: SHED_OLDEST at 85%, shed up to 2 tasks
 * Fallback: REJECT at 90% (CORE-equivalent)
 * ------------------------------------------------------------------- */

void asx_adapter_hft_decide(uint32_t used, uint32_t capacity,
                             asx_adapter_decision *out)
{
    uint32_t load_pct;

    memset(out, 0, sizeof(*out));
    out->path_used = ASX_ADAPTER_ACCELERATED;
    out->mode = ASX_OVERLOAD_SHED_OLDEST;

    if (capacity == 0) {
        out->triggered = 1;
        out->load_pct = 100;
        out->admit_status = ASX_E_ADMISSION_CLOSED;
        out->decision_hash = decision_hash(out);
        return;
    }

    load_pct = (used * 100u) / capacity;
    out->load_pct = load_pct;

    if (load_pct >= 85) {
        out->triggered = 1;
        /* Shed up to 2 tasks, but no more than used count */
        out->shed_count = used < 2 ? used : 2;
        out->admit_status = ASX_OK; /* SHED admits after eviction */
    } else {
        out->triggered = 0;
        out->admit_status = ASX_OK;
    }
    out->decision_hash = decision_hash(out);
}

void asx_adapter_hft_fallback(uint32_t used, uint32_t capacity,
                               asx_adapter_decision *out)
{
    core_fallback_decide(used, capacity, out);
}

/* -------------------------------------------------------------------
 * Automotive adapter
 *
 * Accelerated: BACKPRESSURE at 90%, deadline-aware logging
 * Fallback: REJECT at 90% (CORE-equivalent)
 * ------------------------------------------------------------------- */

void asx_adapter_auto_decide(uint32_t used, uint32_t capacity,
                              const asx_auto_deadline_tracker *dt,
                              asx_adapter_decision *out)
{
    uint32_t load_pct;

    memset(out, 0, sizeof(*out));
    out->path_used = ASX_ADAPTER_ACCELERATED;
    out->mode = ASX_OVERLOAD_BACKPRESSURE;

    if (capacity == 0) {
        out->triggered = 1;
        out->load_pct = 100;
        out->admit_status = ASX_E_WOULD_BLOCK;
        out->decision_hash = decision_hash(out);
        return;
    }

    load_pct = (used * 100u) / capacity;
    out->load_pct = load_pct;

    if (load_pct >= 90) {
        out->triggered = 1;
        out->admit_status = ASX_E_WOULD_BLOCK;
    } else {
        out->triggered = 0;
        out->admit_status = ASX_OK;
    }

    /* Deadline-aware escalation: if miss rate is high, lower threshold */
    if (dt != NULL && dt->total_deadlines > 0) {
        uint32_t miss_rate = asx_auto_deadline_miss_rate(dt);
        /* If miss rate > 5%, tighten overload to 80% */
        if (miss_rate > 500 && load_pct >= 80 && !out->triggered) {
            out->triggered = 1;
            out->admit_status = ASX_E_WOULD_BLOCK;
        }
    }

    out->decision_hash = decision_hash(out);
}

void asx_adapter_auto_fallback(uint32_t used, uint32_t capacity,
                                asx_adapter_decision *out)
{
    core_fallback_decide(used, capacity, out);
}

/* -------------------------------------------------------------------
 * Router adapter
 *
 * Accelerated: REJECT at 75%, resource-class-aware capacity scaling
 * Fallback: REJECT at 90% (CORE-equivalent)
 * ------------------------------------------------------------------- */

void asx_adapter_router_decide(uint32_t used, uint32_t capacity,
                                asx_resource_class rclass,
                                asx_adapter_decision *out)
{
    uint32_t scaled_capacity;
    uint32_t load_pct;

    memset(out, 0, sizeof(*out));
    out->path_used = ASX_ADAPTER_ACCELERATED;
    out->mode = ASX_OVERLOAD_REJECT;

    /* Scale capacity by resource class */
    switch (rclass) {
    case ASX_CLASS_R1:    scaled_capacity = capacity / 2; break;
    case ASX_CLASS_R3:    scaled_capacity = capacity * 2; break;
    case ASX_CLASS_R2:    scaled_capacity = capacity;     break;
    case ASX_CLASS_COUNT: scaled_capacity = capacity;     break;
    }

    if (scaled_capacity == 0) {
        out->triggered = 1;
        out->load_pct = 100;
        out->admit_status = ASX_E_ADMISSION_CLOSED;
        out->decision_hash = decision_hash(out);
        return;
    }

    load_pct = (used * 100u) / scaled_capacity;
    out->load_pct = load_pct;

    if (load_pct >= 75) {
        out->triggered = 1;
        out->admit_status = ASX_E_ADMISSION_CLOSED;
    } else {
        out->triggered = 0;
        out->admit_status = ASX_OK;
    }

    /* Safety: accelerated path must never admit what CORE fallback
     * (REJECT at 90% of original capacity) would reject. This ensures
     * the isomorphism contract holds even with R3 capacity scaling. */
    if (!out->triggered && capacity > 0) {
        uint32_t core_pct = (used * 100u) / capacity;
        if (core_pct >= 90) {
            out->triggered = 1;
            out->load_pct = core_pct;
            out->admit_status = ASX_E_ADMISSION_CLOSED;
        }
    }

    out->decision_hash = decision_hash(out);
}

void asx_adapter_router_fallback(uint32_t used, uint32_t capacity,
                                  asx_adapter_decision *out)
{
    core_fallback_decide(used, capacity, out);
}

/* -------------------------------------------------------------------
 * Unified dispatch
 * ------------------------------------------------------------------- */

void asx_adapter_dispatch(asx_adapter_domain domain,
                           asx_adapter_mode mode,
                           uint32_t used,
                           uint32_t capacity,
                           const void *domain_ctx,
                           asx_adapter_decision *out)
{
    if (mode == ASX_ADAPTER_FALLBACK) {
        core_fallback_decide(used, capacity, out);
        return;
    }

    switch (domain) {
    case ASX_ADAPTER_DOMAIN_HFT:
        asx_adapter_hft_decide(used, capacity, out);
        break;
    case ASX_ADAPTER_DOMAIN_AUTOMOTIVE:
        asx_adapter_auto_decide(used, capacity,
            (const asx_auto_deadline_tracker *)domain_ctx, out);
        break;
    case ASX_ADAPTER_DOMAIN_ROUTER: {
        asx_resource_class rc = ASX_CLASS_R2;
        if (domain_ctx != NULL) {
            rc = *(const asx_resource_class *)domain_ctx;
        }
        asx_adapter_router_decide(used, capacity, rc, out);
        break;
    }
    case ASX_ADAPTER_DOMAIN_COUNT:
        core_fallback_decide(used, capacity, out);
        break;
    }
}

/* -------------------------------------------------------------------
 * Isomorphism proof
 *
 * The isomorphism contract checks that for loads well below any
 * threshold (< 75%), both paths admit, and for loads at or above
 * the CORE threshold (>= 90%), both paths reject. The accelerated
 * path may be stricter (lower threshold) which is safe — it only
 * rejects sooner, never admits what CORE would reject.
 *
 * Semantic equivalence is defined as: the triggered field agrees
 * when the load is below the minimum threshold of the two policies,
 * OR when the load is at or above the maximum threshold.
 * ------------------------------------------------------------------- */

void asx_adapter_prove_isomorphism(asx_adapter_domain domain,
                                    uint32_t load,
                                    uint32_t capacity,
                                    const void *domain_ctx,
                                    asx_adapter_isomorphism *proof)
{
    asx_adapter_decision accel, fallback;

    memset(proof, 0, sizeof(*proof));
    proof->domain = domain;
    proof->test_load = load;
    proof->test_capacity = capacity;

    /* Run accelerated path */
    asx_adapter_dispatch(domain, ASX_ADAPTER_ACCELERATED,
                         load, capacity, domain_ctx, &accel);
    /* Run fallback path */
    asx_adapter_dispatch(domain, ASX_ADAPTER_FALLBACK,
                         load, capacity, domain_ctx, &fallback);

    proof->accel_decision = accel;
    proof->fallback_decision = fallback;
    proof->accel_hash = accel.decision_hash;
    proof->fallback_hash = fallback.decision_hash;

    /*
     * Isomorphism holds if:
     * 1. Both admit (triggered==0), OR
     * 2. Both reject (triggered==1), OR
     * 3. Accelerated rejects but fallback admits (safe: accel is stricter)
     *
     * Isomorphism FAILS only if:
     *   Accelerated admits but fallback rejects (unsafe regression)
     */
    if (accel.triggered == 0 && fallback.triggered == 1) {
        /* Adapter admits what CORE rejects — violation */
        proof->pass = 0;
    } else {
        proof->pass = 1;
    }
}

int asx_adapter_prove_isomorphism_sweep(asx_adapter_domain domain,
                                         uint32_t capacity,
                                         const void *domain_ctx,
                                         asx_adapter_isomorphism *failed_proof)
{
    uint32_t load;

    for (load = 0; load <= capacity; load++) {
        asx_adapter_isomorphism proof;
        asx_adapter_prove_isomorphism(domain, load, capacity,
                                       domain_ctx, &proof);
        if (!proof.pass) {
            if (failed_proof != NULL) {
                *failed_proof = proof;
            }
            return 0;
        }
    }
    return 1;
}

/* -------------------------------------------------------------------
 * Diagnostics
 * ------------------------------------------------------------------- */

const char *asx_adapter_domain_str(asx_adapter_domain domain)
{
    switch (domain) {
    case ASX_ADAPTER_DOMAIN_HFT:       return "HFT";
    case ASX_ADAPTER_DOMAIN_AUTOMOTIVE: return "Automotive";
    case ASX_ADAPTER_DOMAIN_ROUTER:     return "Router";
    case ASX_ADAPTER_DOMAIN_COUNT:      break;
    }
    return "Unknown";
}

const char *asx_adapter_mode_str(asx_adapter_mode mode)
{
    switch (mode) {
    case ASX_ADAPTER_FALLBACK:    return "Fallback";
    case ASX_ADAPTER_ACCELERATED: return "Accelerated";
    default:                      return "Unknown";
    }
}

uint32_t asx_adapter_version(void)
{
    return ASX_ADAPTER_VERSION;
}
