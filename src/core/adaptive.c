/*
 * adaptive.c — adaptive-decision contract implementation
 *
 * Expected-loss decision layer with evidence ledger and deterministic
 * fallback. See plan section 6.11 and include/asx/core/adaptive.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx_config.h>
#include <asx/core/adaptive.h>
#include <string.h>

/* -------------------------------------------------------------------
 * Internal state (static, zero-allocation)
 * ------------------------------------------------------------------- */

static asx_adaptive_policy g_policy;
static uint32_t g_decision_seq;
static uint32_t g_fallback_count;
static int g_in_fallback;

/* Ring buffer for evidence ledger */
static asx_adaptive_ledger_entry g_ledger[ASX_ADAPTIVE_LEDGER_DEPTH];
static uint32_t g_ledger_write; /* next write position */
static uint32_t g_ledger_total; /* total entries written */

/* -------------------------------------------------------------------
 * Init / reset
 * ------------------------------------------------------------------- */

void asx_adaptive_init(void) { asx_adaptive_reset(); }

void asx_adaptive_reset(void) {
    memset(&g_policy, 0, sizeof(g_policy));
    g_decision_seq = 0;
    g_fallback_count = 0;
    g_in_fallback = 0;
    g_ledger_write = 0;
    g_ledger_total = 0;
    memset(g_ledger, 0, sizeof(g_ledger));
}

/* -------------------------------------------------------------------
 * Policy
 * ------------------------------------------------------------------- */

asx_status asx_adaptive_set_policy(const asx_adaptive_policy *policy) {
    if (!policy) { return ASX_E_INVALID_ARGUMENT; }
    g_policy = *policy;
    return ASX_OK;
}

asx_adaptive_policy asx_adaptive_policy_active(void) { return g_policy; }

/* -------------------------------------------------------------------
 * Decision evaluation
 *
 * For each action a:
 *   E[L(a)] = Σ_s L(a, s) * P(s | evidence)
 *
 * Select: action* = argmin_a E[L(a)]
 * Track counterfactual (second-best) for audit.
 * ------------------------------------------------------------------- */

static uint64_t compute_expected_loss(const asx_adaptive_surface *surface,
                                      const asx_adaptive_posterior *posterior,
                                      asx_adaptive_action action) {
    uint64_t total = 0;
    uint8_t i;
    uint8_t count = surface->state_count <= ASX_ADAPTIVE_MAX_ACTIONS ? surface->state_count
                                                                     : ASX_ADAPTIVE_MAX_ACTIONS;
    for (i = 0; i < count; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: count clamped to ASX_ADAPTIVE_MAX_ACTIONS");
        uint32_t loss = surface->loss_fn(surface->loss_ctx, action, i);
        /* loss is fp 16.16, posterior is fp 0.32
         * product is fp 16.48; shift right 32 to get fp 16.16 */
        uint64_t product = (uint64_t)loss * (uint64_t)posterior->posterior[i];
        total += (product >> 32);
    }
    return total;
}

static void write_ledger(const asx_adaptive_surface *surface, const asx_adaptive_decision *decision,
                         const asx_adaptive_evidence_term *evidence, uint8_t evidence_count) {
    uint32_t slot = g_ledger_write % ASX_ADAPTIVE_LEDGER_DEPTH;
    asx_adaptive_ledger_entry *e = &g_ledger[slot];
    uint8_t i;
    uint8_t n;

    e->sequence = g_decision_seq;
    e->surface = surface->name;
    e->decision = *decision;

    n = evidence_count;
    if (n > ASX_ADAPTIVE_MAX_EVIDENCE) { n = ASX_ADAPTIVE_MAX_EVIDENCE; }
    e->evidence_count = n;
    for (i = 0; i < n; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: n <= ASX_ADAPTIVE_MAX_EVIDENCE");
        e->evidence[i] = evidence[i];
    }

    g_ledger_write++;
    if (g_ledger_total < UINT32_MAX) { g_ledger_total++; }
}

asx_status asx_adaptive_decide(const asx_adaptive_surface *surface,
                               const asx_adaptive_posterior *posterior,
                               const asx_adaptive_evidence_term *evidence, uint8_t evidence_count,
                               asx_adaptive_decision *out_decision) {
    uint8_t a;
    uint64_t best_loss;
    uint64_t second_loss;
    asx_adaptive_action best_action;
    asx_adaptive_action second_action;
    int use_fallback;

    if (!surface || !posterior || !out_decision) { return ASX_E_INVALID_ARGUMENT; }
    if (surface->action_count < 1 || surface->action_count > ASX_ADAPTIVE_MAX_ACTIONS) {
        return ASX_E_INVALID_ARGUMENT;
    }
    if (surface->state_count < 1 || surface->state_count > ASX_ADAPTIVE_MAX_ACTIONS) {
        return ASX_E_INVALID_ARGUMENT;
    }
    if (surface->fallback >= surface->action_count) { return ASX_E_INVALID_ARGUMENT; }
    if (posterior->state_count != surface->state_count) { return ASX_E_INVALID_ARGUMENT; }
    if (!surface->loss_fn) { return ASX_E_INVALID_ARGUMENT; }
    if (evidence_count > 0u && evidence == NULL) { return ASX_E_INVALID_ARGUMENT; }

    /* Check fallback conditions */
    use_fallback = 0;

    if (g_policy.confidence_threshold_fp32 > 0 &&
        posterior->confidence_fp32 < g_policy.confidence_threshold_fp32) {
        use_fallback = 1;
    }
    if (g_policy.budget_remaining > 0 && g_decision_seq >= g_policy.budget_remaining) {
        use_fallback = 1;
    }

    g_in_fallback = use_fallback;

    if (use_fallback) {
        /* Deterministic fallback: use surface's declared fallback action */
        out_decision->selected = surface->fallback;
        out_decision->expected_loss_fp16 = 0;
        out_decision->counterfactual = surface->fallback;
        out_decision->cf_loss_fp16 = 0;
        out_decision->used_fallback = 1;
        out_decision->confidence_fp32 = posterior->confidence_fp32;
        g_fallback_count++;
    } else {
        /* Expected-loss evaluation */
        best_loss = UINT64_MAX;
        second_loss = UINT64_MAX;
        best_action = 0;
        second_action = 0;

        for (a = 0; a < surface->action_count; a++) {
            ASX_CHECKPOINT_WAIVER("bounded: action_count <= ASX_ADAPTIVE_MAX_ACTIONS");
            uint64_t el = compute_expected_loss(surface, posterior, a);
            if (el < best_loss) {
                second_loss = best_loss;
                second_action = best_action;
                best_loss = el;
                best_action = a;
            } else if (el < second_loss) {
                second_loss = el;
                second_action = a;
            }
        }

        out_decision->selected = best_action;
        out_decision->expected_loss_fp16 = (uint32_t)best_loss;
        out_decision->counterfactual = second_action;
        out_decision->cf_loss_fp16 = (uint32_t)second_loss;
        out_decision->used_fallback = 0;
        out_decision->confidence_fp32 = posterior->confidence_fp32;
    }

    /* Write to evidence ledger */
    write_ledger(surface, out_decision, evidence, evidence_count);
    g_decision_seq++;

    return ASX_OK;
}

/* -------------------------------------------------------------------
 * Ledger queries
 * ------------------------------------------------------------------- */

uint32_t asx_adaptive_ledger_count(void) { return g_ledger_total; }

int asx_adaptive_ledger_overflowed(void) {
    return g_ledger_total > ASX_ADAPTIVE_LEDGER_DEPTH ? 1 : 0;
}

int asx_adaptive_ledger_get(uint32_t index, asx_adaptive_ledger_entry *out) {
    uint32_t readable;
    uint32_t start;

    if (!out) { return 0; }

    if (g_ledger_total <= ASX_ADAPTIVE_LEDGER_DEPTH) {
        readable = g_ledger_total;
        start = 0;
    } else {
        readable = ASX_ADAPTIVE_LEDGER_DEPTH;
        start = g_ledger_write % ASX_ADAPTIVE_LEDGER_DEPTH;
    }

    if (index >= readable) { return 0; }

    *out = g_ledger[(start + index) % ASX_ADAPTIVE_LEDGER_DEPTH];
    return 1;
}

uint64_t asx_adaptive_ledger_digest(void) {
    /* FNV-1a over ledger entries */
    uint64_t hash = 14695981039346656037ULL;
    uint32_t readable;
    uint32_t start;
    uint32_t i;

    if (g_ledger_total <= ASX_ADAPTIVE_LEDGER_DEPTH) {
        readable = g_ledger_total;
        start = 0;
    } else {
        readable = ASX_ADAPTIVE_LEDGER_DEPTH;
        start = g_ledger_write % ASX_ADAPTIVE_LEDGER_DEPTH;
    }

    for (i = 0; i < readable; i++) {
        const asx_adaptive_ledger_entry *e = &g_ledger[(start + i) % ASX_ADAPTIVE_LEDGER_DEPTH];
        /* Hash individual fields to avoid padding-dependent digests.
         * Raw struct hashing via sizeof() includes padding bytes which
         * differ across platforms/compilers, breaking determinism. */
#define LEDGER_HASH_U32(val)                                                                       \
    do {                                                                                           \
        hash ^= (uint64_t)(val);                                                                   \
        hash *= 1099511628211ULL;                                                                  \
    } while (0)
        LEDGER_HASH_U32(e->decision.selected);
        LEDGER_HASH_U32(e->decision.expected_loss_fp16);
        LEDGER_HASH_U32(e->decision.counterfactual);
        LEDGER_HASH_U32(e->decision.cf_loss_fp16);
        LEDGER_HASH_U32((uint32_t)e->decision.used_fallback);
        LEDGER_HASH_U32(e->decision.confidence_fp32);

        LEDGER_HASH_U32((uint32_t)e->evidence_count);
        {
            uint8_t j;
            for (j = 0; j < e->evidence_count; j++) { LEDGER_HASH_U32(e->evidence[j].value_fp32); }
        }
#undef LEDGER_HASH_U32
        /* Include sequence */
        hash ^= (uint64_t)e->sequence;
        hash *= 1099511628211ULL;
    }

    return hash;
}

/* -------------------------------------------------------------------
 * Fallback queries
 * ------------------------------------------------------------------- */

int asx_adaptive_in_fallback(void) { return g_in_fallback; }

uint32_t asx_adaptive_fallback_count(void) { return g_fallback_count; }
