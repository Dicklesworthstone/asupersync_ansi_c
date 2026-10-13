/*
 * cx.c — capability context implementation
 *
 * Zero dynamic allocation. All Cx state is caller-owned or
 * borrowed from the runtime's static tables.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/cx/cx.h>
#include <asx/portable.h>
#include <asx/runtime/runtime.h>
#include <asx/security/crypto.h>
#include <stddef.h>
#include <string.h>

/* Simple splitmix64-style step for deterministic entropy */
static uint64_t cx_entropy_step(uint64_t *state) {
    uint64_t z = (*state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static uint32_t g_cx_generation = 1u;

static int cx_caps_subset(asx_cap_flags have, asx_cap_flags want) { return (want & ~have) == 0u; }

static asx_status cx_copy_with_caps(const asx_cx *parent, asx_cx *child, asx_cap_flags child_caps) {
    if (parent == NULL || child == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!asx_cx_is_valid(parent)) return ASX_E_INVALID_STATE;
    if (!cx_caps_subset(parent->caps, child_caps)) return ASX_E_INVALID_ARGUMENT;

    *child = *parent; /* the envelopes carry over: a narrowed Cx never gains one */
    child->region_id = parent->region_id;
    child->task_id = parent->task_id;
    child->caps = child_caps;
    child->budget = parent->budget;
    child->clock = parent->clock;
    child->entropy_state = parent->entropy_state;
    child->generation = g_cx_generation++;
    return ASX_OK;
}

static int cx_grant_valid(const asx_cx_registry *registry, asx_cx_grant grant,
                          const asx_cx_registry_entry **out_entry) {
    const asx_cx_registry_entry *entry;

    if (registry == NULL || registry->entries == NULL) return 0;
    if (grant.slot >= registry->capacity) return 0;

    entry = &registry->entries[grant.slot];
    if (!entry->alive) return 0;
    if (entry->generation != grant.generation) return 0;

    if (out_entry != NULL) *out_entry = entry;
    return 1;
}

static int cx_grant_matches_parent(const asx_cx *parent, const asx_cx_registry_entry *entry) {
    if (parent == NULL || entry == NULL) return 0;
    return parent->generation == entry->parent_generation &&
           parent->region_id == entry->parent_region_id && parent->task_id == entry->parent_task_id;
}

/* ------------------------------------------------------------------ */
/* Cx lifecycle                                                        */
/* ------------------------------------------------------------------ */

asx_status asx_cx_init(asx_cx *cx, asx_region_id region_id, asx_task_id task_id,
                       asx_cap_flags caps) {
    if (cx == NULL) return ASX_E_INVALID_ARGUMENT;
    if (region_id == ASX_INVALID_ID) return ASX_E_INVALID_ARGUMENT;

    memset(cx, 0, sizeof(*cx));
    cx->region_id = region_id;
    cx->task_id = task_id;
    cx->caps = caps;
    /* Its region's envelope; an id the runtime does not hold (a bare one) has none. */
    if (asx_region_capability_budget(region_id, &cx->capability_budget) != ASX_OK)
        cx->capability_budget = asx_capability_budget_unspecified();
    cx->generation = g_cx_generation++;
    return ASX_OK;
}

asx_status asx_cx_narrow(const asx_cx *parent, asx_cx *child, asx_cap_flags child_caps) {
    return cx_copy_with_caps(parent, child, child_caps);
}

asx_status asx_cx_attenuate(const asx_cx *parent, asx_cx *child, asx_cap_flags cap_mask) {
    if (parent == NULL || child == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!asx_cx_is_valid(parent)) return ASX_E_INVALID_STATE;
    return cx_copy_with_caps(parent, child, parent->caps & cap_mask);
}

void asx_cx_invalidate(asx_cx *cx) {
    if (cx == NULL) return;
    memset(cx, 0, sizeof(*cx));
}

int asx_cx_is_valid(const asx_cx *cx) {
    if (cx == NULL) return 0;
    return cx->generation != 0u && cx->region_id != ASX_INVALID_ID;
}

/* ------------------------------------------------------------------ */
/* Identity accessors                                                  */
/* ------------------------------------------------------------------ */

asx_region_id asx_cx_region(const asx_cx *cx) {
    if (cx == NULL) return ASX_INVALID_ID;
    return cx->region_id;
}

asx_task_id asx_cx_task(const asx_cx *cx) {
    if (cx == NULL) return ASX_INVALID_ID;
    return cx->task_id;
}

/* ------------------------------------------------------------------ */
/* Capability queries                                                  */
/* ------------------------------------------------------------------ */

int asx_cx_has_cap(const asx_cx *cx, asx_cap_flags cap) {
    if (cx == NULL) return 0;
    return (cx->caps & cap) == cap;
}

asx_cap_flags asx_cx_caps(const asx_cx *cx) {
    if (cx == NULL) return ASX_CAP_NONE;
    return cx->caps;
}

asx_status asx_cx_wrap(const asx_cx *parent, asx_cap_flags child_caps,
                       asx_cx_wrapper *out_wrapper) {
    if (parent == NULL || out_wrapper == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!asx_cx_is_valid(parent)) return ASX_E_INVALID_STATE;
    if (!cx_caps_subset(parent->caps, child_caps)) return ASX_E_INVALID_ARGUMENT;

    out_wrapper->region_id = parent->region_id;
    out_wrapper->task_id = parent->task_id;
    out_wrapper->caps = child_caps;
    out_wrapper->parent_generation = parent->generation;
    return ASX_OK;
}

asx_status asx_cx_unwrap(const asx_cx *parent, const asx_cx_wrapper *wrapper, asx_cx *out_child) {
    if (parent == NULL || wrapper == NULL || out_child == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!asx_cx_is_valid(parent)) return ASX_E_INVALID_STATE;
    if (wrapper->parent_generation != parent->generation) return ASX_E_STALE_HANDLE;
    if (wrapper->region_id != parent->region_id || wrapper->task_id != parent->task_id) {
        return ASX_E_PERMISSION_DENIED;
    }

    return cx_copy_with_caps(parent, out_child, wrapper->caps);
}

asx_status asx_cx_registry_init(asx_cx_registry *registry, asx_cx_registry_entry *entries,
                                uint32_t capacity) {
    uint32_t i;

    if (registry == NULL || entries == NULL || capacity == 0u) return ASX_E_INVALID_ARGUMENT;

    registry->entries = entries;
    registry->capacity = capacity;
    registry->next_generation = 1u;

    for (i = 0; i < capacity; ++i) {
        registry->entries[i].generation = 0u;
        registry->entries[i].caps = ASX_CAP_NONE;
        registry->entries[i].parent_region_id = ASX_INVALID_ID;
        registry->entries[i].parent_task_id = ASX_INVALID_ID;
        registry->entries[i].parent_generation = 0u;
        registry->entries[i].alive = 0;
    }

    return ASX_OK;
}

asx_status asx_cx_registry_issue(const asx_cx *parent, asx_cx_registry *registry,
                                 asx_cap_flags child_caps, asx_cx_grant *out_grant) {
    uint32_t i;

    if (parent == NULL || registry == NULL || out_grant == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!asx_cx_is_valid(parent)) return ASX_E_INVALID_STATE;
    if (!cx_caps_subset(parent->caps, child_caps)) return ASX_E_INVALID_ARGUMENT;

    for (i = 0; i < registry->capacity; ++i) {
        asx_cx_registry_entry *entry = &registry->entries[i];
        if (entry->alive) continue;

        entry->generation = registry->next_generation++;
        if (entry->generation == 0u) entry->generation = registry->next_generation++;
        entry->caps = child_caps;
        entry->parent_region_id = parent->region_id;
        entry->parent_task_id = parent->task_id;
        entry->parent_generation = parent->generation;
        entry->alive = 1;

        out_grant->slot = i;
        out_grant->generation = entry->generation;
        return ASX_OK;
    }

    return ASX_E_RESOURCE_EXHAUSTED;
}

asx_status asx_cx_registry_revoke(asx_cx_registry *registry, asx_cx_grant grant) {
    if (registry == NULL || registry->entries == NULL) return ASX_E_INVALID_ARGUMENT;
    if (grant.slot >= registry->capacity) return ASX_E_NOT_FOUND;
    if (!cx_grant_valid(registry, grant, NULL)) return ASX_E_NOT_FOUND;

    registry->entries[grant.slot].alive = 0;
    registry->entries[grant.slot].caps = ASX_CAP_NONE;
    registry->entries[grant.slot].parent_region_id = ASX_INVALID_ID;
    registry->entries[grant.slot].parent_task_id = ASX_INVALID_ID;
    registry->entries[grant.slot].parent_generation = 0u;
    return ASX_OK;
}

asx_status asx_cx_registry_materialize(const asx_cx *parent, const asx_cx_registry *registry,
                                       asx_cx_grant grant, asx_cx *out_child) {
    const asx_cx_registry_entry *entry = NULL;

    if (parent == NULL || registry == NULL || out_child == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!asx_cx_is_valid(parent)) return ASX_E_INVALID_STATE;
    if (!cx_grant_valid(registry, grant, &entry)) return ASX_E_NOT_FOUND;
    if (!cx_grant_matches_parent(parent, entry)) return ASX_E_STALE_HANDLE;

    return cx_copy_with_caps(parent, out_child, entry->caps);
}

/* ------------------------------------------------------------------ */
/* Capability macaroons (HMAC-SHA256 chain)                            */
/* ------------------------------------------------------------------ */

static const char k_macaroon_id_domain[] = "asupersync::cx::macaroon::identifier:v1";
#define CX_CAVEAT_KIND_CAP_MASK 0x01u

/* sig_0 = HMAC(root_key, domain || u64le(region) || u64le(task) ||
 *              u32le(generation) || u32le(root_caps)) */
static void cx_macaroon_root_sig(const asx_auth_key *root_key, asx_region_id region_id,
                                 asx_task_id task_id, uint32_t generation, asx_cap_flags root_caps,
                                 uint8_t out[ASX_CX_MACAROON_SIG_SIZE]) {
    asx_hmac_sha256_ctx mac;
    uint8_t id[24];

    asx_store_le_u64(id, (uint64_t)region_id);
    asx_store_le_u64(id + 8, (uint64_t)task_id);
    asx_store_le_u32(id + 16, generation);
    asx_store_le_u32(id + 20, (uint32_t)root_caps);

    asx_hmac_sha256_init(&mac, root_key->bytes, ASX_AUTH_KEY_SIZE);
    asx_hmac_sha256_update(&mac, k_macaroon_id_domain, sizeof(k_macaroon_id_domain) - 1u);
    asx_hmac_sha256_update(&mac, id, sizeof(id));
    asx_hmac_sha256_final(&mac, out);
}

/* sig_i = HMAC(sig_{i-1}, kind || u32le(mask)); sig may be updated in place. */
static void cx_macaroon_chain_caveat(uint8_t sig[ASX_CX_MACAROON_SIG_SIZE],
                                     asx_cap_flags caveat_caps) {
    uint8_t caveat[5];
    uint8_t next[ASX_CX_MACAROON_SIG_SIZE];

    caveat[0] = (uint8_t)CX_CAVEAT_KIND_CAP_MASK;
    asx_store_le_u32(caveat + 1, (uint32_t)caveat_caps);
    asx_hmac_sha256(sig, ASX_CX_MACAROON_SIG_SIZE, caveat, sizeof(caveat), next);
    memcpy(sig, next, sizeof(next));
    asx_crypto_secure_zero(next, sizeof(next));
}

asx_status asx_cx_macaroon_issue(const asx_cx *parent, const asx_auth_key *root_key,
                                 asx_cap_flags child_caps, asx_cx_macaroon *out_macaroon) {
    if (parent == NULL || root_key == NULL || out_macaroon == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!asx_cx_is_valid(parent)) return ASX_E_INVALID_STATE;
    if (!cx_caps_subset(parent->caps, child_caps)) return ASX_E_INVALID_ARGUMENT;

    memset(out_macaroon, 0, sizeof(*out_macaroon));
    out_macaroon->caps = child_caps;
    out_macaroon->root_caps = child_caps;
    out_macaroon->parent_region_id = parent->region_id;
    out_macaroon->parent_task_id = parent->task_id;
    out_macaroon->parent_generation = parent->generation;
    out_macaroon->caveat_count = 0u;
    cx_macaroon_root_sig(root_key, parent->region_id, parent->task_id, parent->generation,
                         child_caps, out_macaroon->signature);
    return ASX_OK;
}

asx_status asx_cx_macaroon_attenuate(asx_cx_macaroon *macaroon, asx_cap_flags caveat_caps) {
    if (macaroon == NULL) return ASX_E_INVALID_ARGUMENT;
    if (macaroon->caveat_count >= ASX_CX_MACAROON_MAX_CAVEATS) return ASX_E_RESOURCE_EXHAUSTED;

    macaroon->caveats[macaroon->caveat_count] = caveat_caps;
    macaroon->caveat_count++;
    macaroon->caps &= caveat_caps;
    cx_macaroon_chain_caveat(macaroon->signature, caveat_caps);
    return ASX_OK;
}

asx_status asx_cx_macaroon_bind(const asx_cx *parent, const asx_auth_key *root_key,
                                const asx_cx_macaroon *macaroon, asx_cx *out_child) {
    uint8_t sig[ASX_CX_MACAROON_SIG_SIZE];
    asx_cap_flags effective;
    uint32_t i;
    int sig_ok;

    if (parent == NULL || root_key == NULL || macaroon == NULL || out_child == NULL) {
        return ASX_E_INVALID_ARGUMENT;
    }
    if (!asx_cx_is_valid(parent)) return ASX_E_INVALID_STATE;
    if (macaroon->parent_generation != parent->generation ||
        macaroon->parent_region_id != parent->region_id ||
        macaroon->parent_task_id != parent->task_id) {
        return ASX_E_STALE_HANDLE;
    }
    if (macaroon->caveat_count > ASX_CX_MACAROON_MAX_CAVEATS) return ASX_E_PERMISSION_DENIED;

    /* Recompute the chain from the root key and compare in constant time. */
    cx_macaroon_root_sig(root_key, macaroon->parent_region_id, macaroon->parent_task_id,
                         macaroon->parent_generation, macaroon->root_caps, sig);
    effective = macaroon->root_caps;
    for (i = 0u; i < macaroon->caveat_count; i++) {
        cx_macaroon_chain_caveat(sig, macaroon->caveats[i]);
        effective &= macaroon->caveats[i];
    }
    sig_ok = asx_crypto_ct_equal(sig, macaroon->signature, sizeof(sig));
    asx_crypto_secure_zero(sig, sizeof(sig));
    if (!sig_ok || effective != macaroon->caps) return ASX_E_PERMISSION_DENIED;

    /* Root caps were a parent subset at issue; recheck against the live parent. */
    return cx_copy_with_caps(parent, out_child, effective);
}

/* ------------------------------------------------------------------ */
/* Budget access                                                       */
/* ------------------------------------------------------------------ */

asx_status asx_cx_bind_budget(asx_cx *cx, asx_budget *budget) {
    if (cx == NULL || budget == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!asx_cx_is_valid(cx)) return ASX_E_INVALID_STATE;
    if (!asx_cx_has_cap(cx, ASX_CAP_BUDGET_READ)) return ASX_E_PERMISSION_DENIED;

    cx->budget = budget;
    return ASX_OK;
}

const asx_budget *asx_cx_budget(const asx_cx *cx) {
    if (cx == NULL || !asx_cx_has_cap(cx, ASX_CAP_BUDGET_READ)) return NULL;
    return cx->budget;
}

asx_status asx_cx_consume_poll(asx_cx *cx) {
    if (cx == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!asx_cx_has_cap(cx, ASX_CAP_BUDGET_CONSUME)) return ASX_E_PERMISSION_DENIED;
    if (cx->budget == NULL) return ASX_OK; /* no budget bound = unlimited */

    if (asx_budget_consume_poll(cx->budget) == 0u) return ASX_E_POLL_BUDGET_EXHAUSTED;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Clock access                                                        */
/* ------------------------------------------------------------------ */

asx_status asx_cx_bind_clock(asx_cx *cx, asx_time *clock) {
    if (cx == NULL || clock == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!asx_cx_is_valid(cx)) return ASX_E_INVALID_STATE;
    if (!asx_cx_has_cap(cx, ASX_CAP_CLOCK_READ)) return ASX_E_PERMISSION_DENIED;

    cx->clock = clock;
    return ASX_OK;
}

asx_time asx_cx_now(const asx_cx *cx) {
    if (cx == NULL || !asx_cx_has_cap(cx, ASX_CAP_CLOCK_READ)) return 0u;
    if (cx->clock != NULL) return *cx->clock;
    return 0u; /* no clock bound */
}

/* ------------------------------------------------------------------ */
/* Entropy access                                                      */
/* ------------------------------------------------------------------ */

asx_status asx_cx_bind_entropy(asx_cx *cx, uint64_t *entropy_state) {
    if (cx == NULL || entropy_state == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!asx_cx_is_valid(cx)) return ASX_E_INVALID_STATE;
    if (!asx_cx_has_cap(cx, ASX_CAP_ENTROPY)) return ASX_E_PERMISSION_DENIED;

    cx->entropy_state = entropy_state;
    return ASX_OK;
}

uint64_t asx_cx_random_u64(asx_cx *cx) {
    if (cx == NULL || !asx_cx_has_cap(cx, ASX_CAP_ENTROPY)) return 0u;
    if (cx->entropy_state == NULL) return 0u;
    return cx_entropy_step(cx->entropy_state);
}

/* ------------------------------------------------------------------ */
/* Cancellation checkpoint                                             */
/* ------------------------------------------------------------------ */

int asx_cx_is_cancelled(const asx_cx *cx) {
    asx_cancel_reason reason;
    if (cx == NULL || cx->task_id == ASX_INVALID_ID) return 0;
    /* Rust Cx::is_cancel_requested (cx.rs:2641): whether a cancel was
     * requested of the task, a budget cancel its checkpoint raised
     * included. It stays set once the task completes, and a mask does not
     * hide it: "masking defers checkpoint delivery, not cancellation
     * visibility" (cx.rs:6660-6664). The task's cancel reason is recorded
     * exactly then. */
    return asx_task_get_cancel_reason(cx->task_id, &reason) == ASX_OK;
}

asx_status asx_cx_checkpoint(asx_cx *cx) {
    asx_checkpoint_result cr;
    uint32_t mask_depth = 0u;

    if (cx == NULL) return ASX_E_INVALID_ARGUMENT;

    /* Check cancellation first, as Rust's Cx::checkpoint does
     * (cx.rs:3112-3160): it observes the task's pending cancel, which
     * acknowledges it, and a passed budget deadline; a masked task sees
     * neither. asx_checkpoint is exactly that for the task. Rust's
     * checkpoint needs no capability: a narrowed Cx still sees its
     * task's cancel. */
    if (cx->task_id != ASX_INVALID_ID) {
        if (asx_checkpoint(cx->task_id, &cr) == ASX_OK && cr.cancelled) return ASX_E_CANCELLED;
        if (asx_task_mask_depth(cx->task_id, &mask_depth) != ASX_OK) mask_depth = 0u;
    }

    /* A budget bound with asx_cx_bind_budget is observed, never spent: the
     * checkpoint costs no poll (Rust charges a poll's unit when the
     * scheduler polls, and the checkpoint turns a spent poll or cost quota
     * into a cancel, checkpoint_budget_exhaustion, cx.rs:3112-3160). A mask
     * defers the delivery. Spend a poll with asx_cx_consume_poll. */
    if (mask_depth == 0u && cx->budget != NULL &&
        (cx->budget->poll_quota == 0u || cx->budget->cost_quota == 0u)) {
        return ASX_E_CANCELLED;
    }

    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Capability budget (resource envelopes; bd-9kll.3.27)                */
/* ------------------------------------------------------------------ */

asx_capability_budget asx_cx_capability_budget(const asx_cx *cx) {
    if (cx == NULL) return asx_capability_budget_unspecified();
    return cx->capability_budget;
}

asx_status asx_cx_plan_child_capability_budget(const asx_cx *cx, const asx_capability_budget *child,
                                               const asx_capability_requirements *requirements,
                                               asx_capability_budget *out,
                                               asx_capability_refusal *refusal) {
    if (cx == NULL) return ASX_E_INVALID_ARGUMENT;
    return asx_capability_budget_plan_child(&cx->capability_budget, child, requirements, out,
                                            refusal);
}

asx_status asx_cx_apply_child_capability_budget(asx_cx *cx, const asx_capability_budget *child,
                                                const asx_capability_requirements *requirements,
                                                asx_capability_budget *out,
                                                asx_capability_refusal *refusal) {
    asx_capability_budget effective;
    asx_status st;
    if (cx == NULL) return ASX_E_INVALID_ARGUMENT;
#ifdef ASX_MUTANT_CAPABILITY_APPLY_ON_REFUSAL
    /* Gate mutant (tests/gates/test_oracle_mutants.sh): tightened before it
     * is admitted, so a refusal leaves the Cx changed. */
    cx->capability_budget = asx_capability_budget_meet(&cx->capability_budget, child);
#endif
    st = asx_capability_budget_plan_child(&cx->capability_budget, child, requirements, &effective,
                                          refusal);
    if (st != ASX_OK) return st; /* the Cx is as it was */
    cx->capability_budget = effective;
    if (out != NULL) *out = effective;
    return ASX_OK;
}

asx_status asx_cx_open_child_region(const asx_cx *cx, const asx_budget *budget,
                                    const asx_capability_budget *capability_budget,
                                    const asx_capability_requirements *requirements,
                                    asx_region_id *out_child, asx_capability_refusal *refusal) {
    asx_capability_budget request;
    if (cx == NULL || out_child == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!asx_cx_is_valid(cx)) return ASX_E_INVALID_STATE;
    if (!asx_cx_has_cap(cx, ASX_CAP_SPAWN)) return ASX_E_PERMISSION_DENIED;
        /* cx.rs:4514-4516: the Cx's own envelope met with the spec's; the mint
         * meets the parent record's with it. */
#ifdef ASX_MUTANT_CAPABILITY_CX_OPEN_IGNORES_CX
    /* Gate mutant (tests/gates/test_oracle_mutants.sh): the Cx's own envelope
     * is left out, so a Cx that gave up authority gets it back. */
    request = asx_capability_budget_meet(NULL, capability_budget);
#else
    request = asx_capability_budget_meet(&cx->capability_budget, capability_budget);
#endif
    return asx_region_open_child_poll_with_capability_budget(cx->task_id, cx->region_id, budget,
                                                             &request, requirements, out_child,
                                                             refusal);
}
