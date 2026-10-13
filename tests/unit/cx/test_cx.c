/*
 * test_cx.c — unit tests for capability context (Cx)
 *
 * Tests lifecycle, capability narrowing, budget/clock/entropy binding,
 * and cooperative checkpoints.
 *
 * We stub the runtime's task queries (asx_task_get_cancel_reason,
 * asx_checkpoint) so these tests don't depend on the full runtime being
 * initialized; test_cancellation.c checks Cx against the real runtime.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/core/budget.h>
#include <asx/cx/cx.h>
#include <asx/runtime/runtime.h>
#include <asx/security/crypto.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Stub task — avoids pulling in runtime/lifecycle                     */
/* ------------------------------------------------------------------ */

/* The stub task is not completed; a state from CANCEL_REQUESTED on means
 * a cancel was requested of it. */
static asx_task_state g_stub_task_state = ASX_TASK_RUNNING;
static asx_task_id g_stub_task_id = ASX_INVALID_ID;
static uint32_t g_stub_mask_depth = 0;

/* Stub for asx_task_get_cancel_reason, which Cx's is_cancelled reads:
 * found once a cancel was requested. */
asx_status asx_task_get_cancel_reason(asx_task_id id, asx_cancel_reason *out);
asx_status asx_task_get_cancel_reason(asx_task_id id, asx_cancel_reason *out) {
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    if (id == ASX_INVALID_ID || id != g_stub_task_id) return ASX_E_NOT_FOUND;
    if (g_stub_task_state < ASX_TASK_CANCEL_REQUESTED) return ASX_E_NOT_FOUND;
    memset(out, 0, sizeof(*out));
    out->kind = ASX_CANCEL_USER;
    return ASX_OK;
}

/* Stub for asx_checkpoint, which Cx's checkpoint delegates to: the stub
 * task observes its cancel request unless masked. */
asx_status asx_checkpoint(asx_task_id self, asx_checkpoint_result *out);
asx_status asx_checkpoint(asx_task_id self, asx_checkpoint_result *out) {
    int requested = g_stub_task_state >= ASX_TASK_CANCEL_REQUESTED;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    if (self == ASX_INVALID_ID || self != g_stub_task_id) return ASX_E_NOT_FOUND;
    out->cancelled = requested && g_stub_mask_depth == 0u;
    out->masked = requested && g_stub_mask_depth > 0u;
    out->phase = ASX_CANCEL_PHASE_REQUESTED;
    out->polls_remaining = 0;
    out->kind = ASX_CANCEL_USER;
    return ASX_OK;
}

/* Stub for asx_task_mask_depth, which Cx's checkpoint reads to defer a
 * spent bound budget while the task is masked: unlike the checkpoint's
 * own `masked`, it does not need a pending cancel. */
asx_status asx_task_mask_depth(asx_task_id id, uint32_t *out_depth);
asx_status asx_task_mask_depth(asx_task_id id, uint32_t *out_depth) {
    if (out_depth == NULL) return ASX_E_INVALID_ARGUMENT;
    if (id == ASX_INVALID_ID || id != g_stub_task_id) return ASX_E_NOT_FOUND;
    *out_depth = g_stub_mask_depth;
    return ASX_OK;
}

/* Stub for asx_region_capability_budget, which Cx's init loads its
 * envelopes from: the stub region carries g_stub_region_envelopes; any other
 * region is not held. */
static asx_region_id g_stub_region_id = ASX_INVALID_ID;
static asx_capability_budget g_stub_region_envelopes;

asx_status asx_region_capability_budget(asx_region_id region, asx_capability_budget *out);
asx_status asx_region_capability_budget(asx_region_id region, asx_capability_budget *out) {
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    if (region == ASX_INVALID_ID || region != g_stub_region_id) return ASX_E_NOT_FOUND;
    *out = g_stub_region_envelopes;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Lifecycle tests                                                     */
/* ------------------------------------------------------------------ */

TEST(init_null_returns_invalid_argument) {
    ASSERT_EQ(asx_cx_init(NULL, 1, 1, ASX_CAP_NONE), ASX_E_INVALID_ARGUMENT);
}

TEST(init_invalid_region_returns_invalid_argument) {
    asx_cx cx;
    ASSERT_EQ(asx_cx_init(&cx, ASX_INVALID_ID, 1, ASX_CAP_NONE), ASX_E_INVALID_ARGUMENT);
}

TEST(init_sets_fields) {
    asx_cx cx;
    ASSERT_EQ(asx_cx_init(&cx, 42, 7, ASX_CAP_CLOCK_READ | ASX_CAP_ENTROPY), ASX_OK);
    ASSERT_EQ(asx_cx_region(&cx), (asx_region_id)42);
    ASSERT_EQ(asx_cx_task(&cx), (asx_task_id)7);
    ASSERT_TRUE(asx_cx_has_cap(&cx, ASX_CAP_CLOCK_READ));
    ASSERT_TRUE(asx_cx_has_cap(&cx, ASX_CAP_ENTROPY));
    ASSERT_FALSE(asx_cx_has_cap(&cx, ASX_CAP_SPAWN));
    ASSERT_TRUE(asx_cx_is_valid(&cx));
    ASSERT_TRUE(cx.budget == NULL);
    ASSERT_TRUE(cx.clock == NULL);
    ASSERT_TRUE(cx.entropy_state == NULL);
}

TEST(init_generation_increments) {
    asx_cx a, b;
    asx_cx_init(&a, 1, 1, ASX_CAP_NONE);
    asx_cx_init(&b, 1, 1, ASX_CAP_NONE);
    ASSERT_NE(a.generation, b.generation);
    ASSERT_TRUE(b.generation > a.generation);
}

TEST(invalidate_clears_all) {
    asx_cx cx;
    asx_cx_init(&cx, 1, 1, ASX_CAP_ALL);
    ASSERT_TRUE(asx_cx_is_valid(&cx));
    asx_cx_invalidate(&cx);
    ASSERT_FALSE(asx_cx_is_valid(&cx));
    ASSERT_EQ(cx.generation, 0u);
    ASSERT_EQ(cx.caps, ASX_CAP_NONE);
}

TEST(invalidate_null_safe) {
    /* Should not crash */
    asx_cx_invalidate(NULL);
}

TEST(is_valid_null) { ASSERT_FALSE(asx_cx_is_valid(NULL)); }

/* ------------------------------------------------------------------ */
/* Identity accessor tests                                             */
/* ------------------------------------------------------------------ */

TEST(region_null_returns_invalid) { ASSERT_EQ(asx_cx_region(NULL), ASX_INVALID_ID); }

TEST(task_null_returns_invalid) { ASSERT_EQ(asx_cx_task(NULL), ASX_INVALID_ID); }

/* ------------------------------------------------------------------ */
/* Capability query tests                                              */
/* ------------------------------------------------------------------ */

TEST(has_cap_null_returns_zero) { ASSERT_FALSE(asx_cx_has_cap(NULL, ASX_CAP_SPAWN)); }

TEST(caps_null_returns_none) { ASSERT_EQ(asx_cx_caps(NULL), ASX_CAP_NONE); }

TEST(caps_returns_all_set) {
    asx_cx cx;
    asx_cx_init(&cx, 1, 1, ASX_CAP_ALL);
    ASSERT_EQ(asx_cx_caps(&cx), ASX_CAP_ALL);
}

TEST(has_cap_multi_flag_check) {
    asx_cx cx;
    asx_cap_flags combo = ASX_CAP_CLOCK_READ | ASX_CAP_ENTROPY;
    asx_cx_init(&cx, 1, 1, combo);
    ASSERT_TRUE(asx_cx_has_cap(&cx, combo));
    ASSERT_FALSE(asx_cx_has_cap(&cx, ASX_CAP_ALL));
}

/* ------------------------------------------------------------------ */
/* Narrowing tests                                                     */
/* ------------------------------------------------------------------ */

TEST(narrow_null_parent_fails) {
    asx_cx child;
    ASSERT_EQ(asx_cx_narrow(NULL, &child, ASX_CAP_NONE), ASX_E_INVALID_ARGUMENT);
}

TEST(narrow_null_child_fails) {
    asx_cx parent;
    asx_cx_init(&parent, 1, 1, ASX_CAP_ALL);
    ASSERT_EQ(asx_cx_narrow(&parent, NULL, ASX_CAP_NONE), ASX_E_INVALID_ARGUMENT);
}

TEST(narrow_invalid_parent_fails) {
    asx_cx parent, child;
    memset(&parent, 0, sizeof(parent));
    ASSERT_EQ(asx_cx_narrow(&parent, &child, ASX_CAP_NONE), ASX_E_INVALID_STATE);
}

TEST(narrow_escalation_fails) {
    asx_cx parent, child;
    asx_cx_init(&parent, 1, 1, ASX_CAP_CLOCK_READ);
    ASSERT_EQ(asx_cx_narrow(&parent, &child, ASX_CAP_CLOCK_READ | ASX_CAP_SPAWN),
              ASX_E_INVALID_ARGUMENT);
}

TEST(narrow_subset_succeeds) {
    asx_cx parent, child;
    asx_budget budget = asx_budget_from_polls(10);
    asx_time clock = 42;
    uint64_t entropy = 0xDEAD;

    asx_cx_init(&parent, 5, 3, ASX_CAP_ALL);
    asx_cx_bind_budget(&parent, &budget);
    asx_cx_bind_clock(&parent, &clock);
    asx_cx_bind_entropy(&parent, &entropy);

    ASSERT_EQ(asx_cx_narrow(&parent, &child, ASX_CAP_CLOCK_READ), ASX_OK);
    ASSERT_EQ(asx_cx_region(&child), (asx_region_id)5);
    ASSERT_EQ(asx_cx_task(&child), (asx_task_id)3);
    ASSERT_TRUE(asx_cx_has_cap(&child, ASX_CAP_CLOCK_READ));
    ASSERT_FALSE(asx_cx_has_cap(&child, ASX_CAP_SPAWN));
    /* Borrowed pointers propagate */
    ASSERT_TRUE(child.budget == &budget);
    ASSERT_TRUE(child.clock == &clock);
    ASSERT_TRUE(child.entropy_state == &entropy);
    ASSERT_TRUE(asx_cx_is_valid(&child));
    ASSERT_NE(child.generation, parent.generation);
}

TEST(narrow_to_none_succeeds) {
    asx_cx parent, child;
    asx_cx_init(&parent, 1, 1, ASX_CAP_ALL);
    ASSERT_EQ(asx_cx_narrow(&parent, &child, ASX_CAP_NONE), ASX_OK);
    ASSERT_EQ(asx_cx_caps(&child), ASX_CAP_NONE);
}

TEST(attenuate_masks_parent_caps) {
    asx_cx parent, child;
    asx_cap_flags caps = ASX_CAP_CLOCK_READ | ASX_CAP_ENTROPY | ASX_CAP_SPAWN;

    asx_cx_init(&parent, 9, 4, caps);
    ASSERT_EQ(asx_cx_attenuate(&parent, &child, ASX_CAP_CLOCK_READ | ASX_CAP_SPAWN), ASX_OK);
    ASSERT_EQ(asx_cx_caps(&child), (asx_cap_flags)(ASX_CAP_CLOCK_READ | ASX_CAP_SPAWN));
}

/* ------------------------------------------------------------------ */
/* Wrapper / registry / macaroon tests                                */
/* ------------------------------------------------------------------ */

TEST(wrap_and_unwrap_subset_succeeds) {
    asx_cx parent, child;
    asx_cx_wrapper wrapper;

    asx_cx_init(&parent, 5, 8, ASX_CAP_CLOCK_READ | ASX_CAP_ENTROPY | ASX_CAP_SPAWN);
    ASSERT_EQ(asx_cx_wrap(&parent, ASX_CAP_CLOCK_READ | ASX_CAP_SPAWN, &wrapper), ASX_OK);
    ASSERT_EQ(asx_cx_unwrap(&parent, &wrapper, &child), ASX_OK);
    ASSERT_EQ(asx_cx_region(&child), (asx_region_id)5);
    ASSERT_EQ(asx_cx_task(&child), (asx_task_id)8);
    ASSERT_TRUE(asx_cx_has_cap(&child, ASX_CAP_CLOCK_READ));
    ASSERT_TRUE(asx_cx_has_cap(&child, ASX_CAP_SPAWN));
    ASSERT_FALSE(asx_cx_has_cap(&child, ASX_CAP_ENTROPY));
}

TEST(wrap_escalation_fails) {
    asx_cx parent;
    asx_cx_wrapper wrapper;

    asx_cx_init(&parent, 5, 8, ASX_CAP_CLOCK_READ);
    ASSERT_EQ(asx_cx_wrap(&parent, ASX_CAP_CLOCK_READ | ASX_CAP_SPAWN, &wrapper),
              ASX_E_INVALID_ARGUMENT);
}

TEST(unwrap_stale_wrapper_fails_closed) {
    asx_cx parent, child;
    asx_cx_wrapper wrapper;

    asx_cx_init(&parent, 5, 8, ASX_CAP_CLOCK_READ | ASX_CAP_SPAWN);
    ASSERT_EQ(asx_cx_wrap(&parent, ASX_CAP_CLOCK_READ, &wrapper), ASX_OK);
    wrapper.parent_generation++;
    ASSERT_EQ(asx_cx_unwrap(&parent, &wrapper, &child), ASX_E_STALE_HANDLE);
}

TEST(registry_issue_materialize_and_revoke) {
    asx_cx parent, child;
    asx_cx_registry registry;
    asx_cx_registry_entry entries[2];
    asx_cx_grant grant;

    asx_cx_init(&parent, 7, 3, ASX_CAP_CLOCK_READ | ASX_CAP_ENTROPY | ASX_CAP_SPAWN);
    ASSERT_EQ(asx_cx_registry_init(&registry, entries, 2u), ASX_OK);
    ASSERT_EQ(asx_cx_registry_issue(&parent, &registry, ASX_CAP_ENTROPY, &grant), ASX_OK);
    ASSERT_EQ(asx_cx_registry_materialize(&parent, &registry, grant, &child), ASX_OK);
    ASSERT_EQ(asx_cx_caps(&child), ASX_CAP_ENTROPY);
    ASSERT_EQ(asx_cx_registry_revoke(&registry, grant), ASX_OK);
    ASSERT_EQ(asx_cx_registry_materialize(&parent, &registry, grant, &child), ASX_E_NOT_FOUND);
}

TEST(registry_materialize_wrong_parent_fails_closed) {
    asx_cx parent, wrong_parent, child;
    asx_cx_registry registry;
    asx_cx_registry_entry entries[2];
    asx_cx_grant grant;

    asx_cx_init(&parent, 7, 3, ASX_CAP_CLOCK_READ | ASX_CAP_ENTROPY | ASX_CAP_SPAWN);
    asx_cx_init(&wrong_parent, 7, 3, ASX_CAP_CLOCK_READ | ASX_CAP_ENTROPY | ASX_CAP_SPAWN);
    ASSERT_EQ(asx_cx_registry_init(&registry, entries, 2u), ASX_OK);
    ASSERT_EQ(asx_cx_registry_issue(&parent, &registry, ASX_CAP_ENTROPY, &grant), ASX_OK);
    ASSERT_EQ(asx_cx_registry_materialize(&wrong_parent, &registry, grant, &child),
              ASX_E_STALE_HANDLE);
}

TEST(registry_capacity_exhaustion_fails) {
    asx_cx parent;
    asx_cx_registry registry;
    asx_cx_registry_entry entries[1];
    asx_cx_grant grant_a, grant_b;

    asx_cx_init(&parent, 7, 3, ASX_CAP_CLOCK_READ | ASX_CAP_ENTROPY);
    ASSERT_EQ(asx_cx_registry_init(&registry, entries, 1u), ASX_OK);
    ASSERT_EQ(asx_cx_registry_issue(&parent, &registry, ASX_CAP_CLOCK_READ, &grant_a), ASX_OK);
    ASSERT_EQ(asx_cx_registry_issue(&parent, &registry, ASX_CAP_ENTROPY, &grant_b),
              ASX_E_RESOURCE_EXHAUSTED);
}

TEST(registry_issue_escalation_fails) {
    asx_cx parent;
    asx_cx_registry registry;
    asx_cx_registry_entry entries[1];
    asx_cx_grant grant;

    asx_cx_init(&parent, 7, 3, ASX_CAP_CLOCK_READ);
    ASSERT_EQ(asx_cx_registry_init(&registry, entries, 1u), ASX_OK);
    ASSERT_EQ(asx_cx_registry_issue(&parent, &registry, ASX_CAP_CLOCK_READ | ASX_CAP_SPAWN, &grant),
              ASX_E_INVALID_ARGUMENT);
}

TEST(macaroon_issue_attenuate_and_bind) {
    asx_cx parent, child;
    asx_cx_macaroon macaroon;
    asx_auth_key root;

    asx_auth_key_from_seed(&root, 1234u);
    asx_cx_init(&parent, 11, 13, ASX_CAP_CLOCK_READ | ASX_CAP_ENTROPY | ASX_CAP_SPAWN);
    ASSERT_EQ(asx_cx_macaroon_issue(&parent, &root, ASX_CAP_CLOCK_READ | ASX_CAP_ENTROPY,
                                    &macaroon),
              ASX_OK);
    ASSERT_EQ(asx_cx_macaroon_attenuate(&macaroon, ASX_CAP_CLOCK_READ), ASX_OK);
    ASSERT_EQ(macaroon.caveat_count, 1u);
    ASSERT_EQ(asx_cx_macaroon_bind(&parent, &root, &macaroon, &child), ASX_OK);
    ASSERT_EQ(asx_cx_caps(&child), ASX_CAP_CLOCK_READ);
}

TEST(macaroon_bind_stale_parent_fails_closed) {
    asx_cx parent, child;
    asx_cx_macaroon macaroon;
    asx_auth_key root;

    asx_auth_key_from_seed(&root, 1234u);
    asx_cx_init(&parent, 11, 13, ASX_CAP_CLOCK_READ | ASX_CAP_ENTROPY);
    ASSERT_EQ(asx_cx_macaroon_issue(&parent, &root, ASX_CAP_CLOCK_READ, &macaroon), ASX_OK);
    macaroon.parent_generation++;
    ASSERT_EQ(asx_cx_macaroon_bind(&parent, &root, &macaroon, &child), ASX_E_STALE_HANDLE);
}

TEST(macaroon_signature_is_hmac_chain) {
    /* sig_0 = HMAC(root, domain || ids), sig_1 = HMAC(sig_0, 0x01 || u32le(mask)) */
    static const char domain[] = "asupersync::cx::macaroon::identifier:v1";
    asx_cx parent;
    asx_cx_macaroon macaroon;
    asx_auth_key root;
    asx_hmac_sha256_ctx mac;
    uint8_t id[24] = {0};
    uint8_t sig[ASX_CX_MACAROON_SIG_SIZE];
    uint8_t caveat[5];
    uint32_t gen;

    asx_auth_key_from_seed(&root, 99u);
    asx_cx_init(&parent, 0x0102u, 0x03u, ASX_CAP_CLOCK_READ | ASX_CAP_SPAWN);
    ASSERT_EQ(asx_cx_macaroon_issue(&parent, &root, ASX_CAP_CLOCK_READ | ASX_CAP_SPAWN, &macaroon),
              ASX_OK);

    gen = parent.generation;
    id[0] = 0x02u;
    id[1] = 0x01u;
    id[8] = 0x03u;
    id[16] = (uint8_t)(gen & 0xFFu);
    id[17] = (uint8_t)((gen >> 8) & 0xFFu);
    id[18] = (uint8_t)((gen >> 16) & 0xFFu);
    id[19] = (uint8_t)((gen >> 24) & 0xFFu);
    id[20] = (uint8_t)((ASX_CAP_CLOCK_READ | ASX_CAP_SPAWN) & 0xFFu);
    asx_hmac_sha256_init(&mac, root.bytes, ASX_AUTH_KEY_SIZE);
    asx_hmac_sha256_update(&mac, domain, sizeof(domain) - 1u);
    asx_hmac_sha256_update(&mac, id, sizeof(id));
    asx_hmac_sha256_final(&mac, sig);
    ASSERT_TRUE(memcmp(sig, macaroon.signature, sizeof(sig)) == 0);

    ASSERT_EQ(asx_cx_macaroon_attenuate(&macaroon, ASX_CAP_CLOCK_READ), ASX_OK);
    caveat[0] = 0x01u;
    caveat[1] = (uint8_t)ASX_CAP_CLOCK_READ;
    caveat[2] = 0u;
    caveat[3] = 0u;
    caveat[4] = 0u;
    asx_hmac_sha256(sig, sizeof(sig), caveat, sizeof(caveat), sig);
    ASSERT_TRUE(memcmp(sig, macaroon.signature, sizeof(sig)) == 0);
}

TEST(macaroon_forged_escalation_rejected) {
    asx_cx parent, child;
    asx_cx_macaroon macaroon, forged;
    asx_auth_key root;

    asx_auth_key_from_seed(&root, 7u);
    asx_cx_init(&parent, 11, 13, ASX_CAP_CLOCK_READ | ASX_CAP_ENTROPY | ASX_CAP_SPAWN);
    ASSERT_EQ(asx_cx_macaroon_issue(&parent, &root, ASX_CAP_CLOCK_READ | ASX_CAP_ENTROPY,
                                    &macaroon),
              ASX_OK);
    ASSERT_EQ(asx_cx_macaroon_attenuate(&macaroon, ASX_CAP_CLOCK_READ), ASX_OK);

    /* Editing the derived caps field */
    forged = macaroon;
    forged.caps |= ASX_CAP_SPAWN;
    ASSERT_EQ(asx_cx_macaroon_bind(&parent, &root, &forged, &child), ASX_E_PERMISSION_DENIED);

    /* Widening the root caps (identifier) */
    forged = macaroon;
    forged.root_caps |= ASX_CAP_SPAWN;
    ASSERT_EQ(asx_cx_macaroon_bind(&parent, &root, &forged, &child), ASX_E_PERMISSION_DENIED);

    /* Dropping the caveat */
    forged = macaroon;
    forged.caveat_count = 0u;
    forged.caps = forged.root_caps;
    ASSERT_EQ(asx_cx_macaroon_bind(&parent, &root, &forged, &child), ASX_E_PERMISSION_DENIED);

    /* Rewriting the caveat */
    forged = macaroon;
    forged.caveats[0] = ASX_CAP_CLOCK_READ | ASX_CAP_ENTROPY;
    forged.caps = ASX_CAP_CLOCK_READ | ASX_CAP_ENTROPY;
    ASSERT_EQ(asx_cx_macaroon_bind(&parent, &root, &forged, &child), ASX_E_PERMISSION_DENIED);

    /* Out-of-range caveat count */
    forged = macaroon;
    forged.caveat_count = ASX_CX_MACAROON_MAX_CAVEATS + 1u;
    ASSERT_EQ(asx_cx_macaroon_bind(&parent, &root, &forged, &child), ASX_E_PERMISSION_DENIED);

    /* The genuine macaroon still binds */
    ASSERT_EQ(asx_cx_macaroon_bind(&parent, &root, &macaroon, &child), ASX_OK);
    ASSERT_EQ(asx_cx_caps(&child), ASX_CAP_CLOCK_READ);
}

TEST(macaroon_wrong_root_key_rejected) {
    asx_cx parent, child;
    asx_cx_macaroon macaroon;
    asx_auth_key root, other;

    asx_auth_key_from_seed(&root, 1u);
    asx_auth_key_from_seed(&other, 2u);
    asx_cx_init(&parent, 11, 13, ASX_CAP_CLOCK_READ);
    ASSERT_EQ(asx_cx_macaroon_issue(&parent, &root, ASX_CAP_CLOCK_READ, &macaroon), ASX_OK);
    ASSERT_EQ(asx_cx_macaroon_bind(&parent, &other, &macaroon, &child), ASX_E_PERMISSION_DENIED);
    ASSERT_EQ(asx_cx_macaroon_bind(&parent, NULL, &macaroon, &child), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_cx_macaroon_issue(&parent, NULL, ASX_CAP_CLOCK_READ, &macaroon),
              ASX_E_INVALID_ARGUMENT);
}

TEST(macaroon_caveat_capacity_failure_atomic) {
    asx_cx parent;
    asx_cx_macaroon macaroon, before;
    asx_auth_key root;
    uint32_t i;

    asx_auth_key_from_seed(&root, 3u);
    asx_cx_init(&parent, 11, 13, ASX_CAP_ALL);
    ASSERT_EQ(asx_cx_macaroon_issue(&parent, &root, ASX_CAP_ALL, &macaroon), ASX_OK);
    for (i = 0u; i < ASX_CX_MACAROON_MAX_CAVEATS; i++) {
        ASSERT_EQ(asx_cx_macaroon_attenuate(&macaroon, ASX_CAP_ALL), ASX_OK);
    }
    before = macaroon;
    ASSERT_EQ(asx_cx_macaroon_attenuate(&macaroon, ASX_CAP_NONE), ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_TRUE(memcmp(&before, &macaroon, sizeof(macaroon)) == 0);
}

/* ------------------------------------------------------------------ */
/* Budget binding tests                                                */
/* ------------------------------------------------------------------ */

TEST(bind_budget_null_cx_fails) {
    asx_budget b = asx_budget_from_polls(5);
    ASSERT_EQ(asx_cx_bind_budget(NULL, &b), ASX_E_INVALID_ARGUMENT);
}

TEST(bind_budget_null_budget_fails) {
    asx_cx cx;
    asx_cx_init(&cx, 1, 1, ASX_CAP_BUDGET_READ);
    ASSERT_EQ(asx_cx_bind_budget(&cx, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(bind_budget_no_cap_fails) {
    asx_cx cx;
    asx_budget b = asx_budget_from_polls(5);
    asx_cx_init(&cx, 1, 1, ASX_CAP_NONE);
    ASSERT_EQ(asx_cx_bind_budget(&cx, &b), ASX_E_PERMISSION_DENIED);
}

TEST(bind_budget_and_read) {
    asx_cx cx;
    asx_budget b = asx_budget_from_polls(10);
    asx_cx_init(&cx, 1, 1, ASX_CAP_BUDGET_READ | ASX_CAP_BUDGET_CONSUME);
    ASSERT_EQ(asx_cx_bind_budget(&cx, &b), ASX_OK);
    ASSERT_TRUE(asx_cx_budget(&cx) != NULL);
    ASSERT_EQ(asx_budget_polls(asx_cx_budget(&cx)), 10u);
}

TEST(budget_null_cx_returns_null) { ASSERT_TRUE(asx_cx_budget(NULL) == NULL); }

TEST(budget_no_read_cap_returns_null) {
    asx_cx cx;
    asx_cx_init(&cx, 1, 1, ASX_CAP_BUDGET_CONSUME);
    /* Can't bind without read cap, so budget stays NULL */
    ASSERT_TRUE(asx_cx_budget(&cx) == NULL);
}

/* ------------------------------------------------------------------ */
/* Consume poll tests                                                  */
/* ------------------------------------------------------------------ */

TEST(consume_poll_null_fails) { ASSERT_EQ(asx_cx_consume_poll(NULL), ASX_E_INVALID_ARGUMENT); }

TEST(consume_poll_no_cap_fails) {
    asx_cx cx;
    asx_cx_init(&cx, 1, 1, ASX_CAP_NONE);
    ASSERT_EQ(asx_cx_consume_poll(&cx), ASX_E_PERMISSION_DENIED);
}

TEST(consume_poll_no_budget_unlimited) {
    asx_cx cx;
    asx_cx_init(&cx, 1, 1, ASX_CAP_BUDGET_CONSUME);
    ASSERT_EQ(asx_cx_consume_poll(&cx), ASX_OK);
}

TEST(consume_poll_decrements) {
    asx_cx cx;
    asx_budget b = asx_budget_from_polls(2);
    asx_cx_init(&cx, 1, 1, ASX_CAP_BUDGET_READ | ASX_CAP_BUDGET_CONSUME);
    asx_cx_bind_budget(&cx, &b);
    ASSERT_EQ(asx_cx_consume_poll(&cx), ASX_OK);
    ASSERT_EQ(asx_budget_polls(&b), 1u);
    ASSERT_EQ(asx_cx_consume_poll(&cx), ASX_OK);
    ASSERT_EQ(asx_budget_polls(&b), 0u);
    ASSERT_EQ(asx_cx_consume_poll(&cx), ASX_E_POLL_BUDGET_EXHAUSTED);
}

/* ------------------------------------------------------------------ */
/* Clock binding tests                                                 */
/* ------------------------------------------------------------------ */

TEST(bind_clock_null_cx_fails) {
    asx_time t = 0;
    ASSERT_EQ(asx_cx_bind_clock(NULL, &t), ASX_E_INVALID_ARGUMENT);
}

TEST(bind_clock_null_clock_fails) {
    asx_cx cx;
    asx_cx_init(&cx, 1, 1, ASX_CAP_CLOCK_READ);
    ASSERT_EQ(asx_cx_bind_clock(&cx, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(bind_clock_no_cap_fails) {
    asx_cx cx;
    asx_time t = 0;
    asx_cx_init(&cx, 1, 1, ASX_CAP_NONE);
    ASSERT_EQ(asx_cx_bind_clock(&cx, &t), ASX_E_PERMISSION_DENIED);
}

TEST(now_reads_bound_clock) {
    asx_cx cx;
    asx_time t = 12345;
    asx_cx_init(&cx, 1, 1, ASX_CAP_CLOCK_READ);
    asx_cx_bind_clock(&cx, &t);
    ASSERT_EQ(asx_cx_now(&cx), (asx_time)12345);
    t = 99999;
    ASSERT_EQ(asx_cx_now(&cx), (asx_time)99999);
}

TEST(now_no_clock_returns_zero) {
    asx_cx cx;
    asx_cx_init(&cx, 1, 1, ASX_CAP_CLOCK_READ);
    ASSERT_EQ(asx_cx_now(&cx), (asx_time)0);
}

TEST(now_null_returns_zero) { ASSERT_EQ(asx_cx_now(NULL), (asx_time)0); }

TEST(now_no_cap_returns_zero) {
    asx_cx cx;
    asx_time t = 42;
    asx_cx_init(&cx, 1, 1, ASX_CAP_NONE);
    /* Can't bind without cap, clock stays NULL */
    ASSERT_EQ(asx_cx_now(&cx), (asx_time)0);
    (void)t;
}

/* ------------------------------------------------------------------ */
/* Entropy binding tests                                               */
/* ------------------------------------------------------------------ */

TEST(bind_entropy_null_cx_fails) {
    uint64_t e = 0;
    ASSERT_EQ(asx_cx_bind_entropy(NULL, &e), ASX_E_INVALID_ARGUMENT);
}

TEST(bind_entropy_null_state_fails) {
    asx_cx cx;
    asx_cx_init(&cx, 1, 1, ASX_CAP_ENTROPY);
    ASSERT_EQ(asx_cx_bind_entropy(&cx, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(bind_entropy_no_cap_fails) {
    asx_cx cx;
    uint64_t e = 0;
    asx_cx_init(&cx, 1, 1, ASX_CAP_NONE);
    ASSERT_EQ(asx_cx_bind_entropy(&cx, &e), ASX_E_PERMISSION_DENIED);
}

TEST(random_u64_draws_nonzero) {
    asx_cx cx;
    uint64_t entropy = 0xCAFEBABE;
    uint64_t r1, r2;
    asx_cx_init(&cx, 1, 1, ASX_CAP_ENTROPY);
    asx_cx_bind_entropy(&cx, &entropy);
    r1 = asx_cx_random_u64(&cx);
    r2 = asx_cx_random_u64(&cx);
    /* Should produce different values */
    ASSERT_NE(r1, r2);
    /* State should have been mutated */
    ASSERT_NE(entropy, (uint64_t)0xCAFEBABE);
}

TEST(random_u64_null_returns_zero) { ASSERT_EQ(asx_cx_random_u64(NULL), 0u); }

TEST(random_u64_no_cap_returns_zero) {
    asx_cx cx;
    asx_cx_init(&cx, 1, 1, ASX_CAP_NONE);
    ASSERT_EQ(asx_cx_random_u64(&cx), 0u);
}

TEST(random_u64_no_state_returns_zero) {
    asx_cx cx;
    asx_cx_init(&cx, 1, 1, ASX_CAP_ENTROPY);
    ASSERT_EQ(asx_cx_random_u64(&cx), 0u);
}

/* ------------------------------------------------------------------ */
/* Cancellation tests                                                  */
/* ------------------------------------------------------------------ */

TEST(is_cancelled_null_returns_zero) { ASSERT_FALSE(asx_cx_is_cancelled(NULL)); }

TEST(cancel_observation_needs_no_capability) {
    /* Rust's Cx::is_cancel_requested and Cx::checkpoint need no
     * capability: a Cx narrowed to nothing still sees its task's cancel.
     * Before, a Cx without the CANCEL_CHECK bit was blind to it, so a
     * narrowed child's channel and sync waits never observed a cancel. */
    asx_cx parent;
    asx_cx cx;
    g_stub_task_id = 14;
    g_stub_task_state = ASX_TASK_CANCEL_REQUESTED;
    asx_cx_init(&parent, 1, 14, ASX_CAP_CLOCK_READ);
    ASSERT_EQ(asx_cx_narrow(&parent, &cx, ASX_CAP_NONE), ASX_OK);
    ASSERT_EQ(asx_cx_caps(&cx), ASX_CAP_NONE);
    ASSERT_TRUE(asx_cx_is_cancelled(&cx));
    ASSERT_EQ(asx_cx_checkpoint(&cx), ASX_E_CANCELLED);
}

TEST(is_cancelled_no_task_returns_zero) {
    asx_cx cx;
    asx_cx_init(&cx, 1, ASX_INVALID_ID, ASX_CAP_NONE);
    ASSERT_FALSE(asx_cx_is_cancelled(&cx));
}

TEST(is_cancelled_running_returns_zero) {
    asx_cx cx;
    g_stub_task_id = 10;
    g_stub_task_state = ASX_TASK_RUNNING;
    asx_cx_init(&cx, 1, 10, ASX_CAP_NONE);
    ASSERT_FALSE(asx_cx_is_cancelled(&cx));
}

TEST(is_cancelled_cancel_requested_returns_nonzero) {
    asx_cx cx;
    g_stub_task_id = 11;
    g_stub_task_state = ASX_TASK_CANCEL_REQUESTED;
    asx_cx_init(&cx, 1, 11, ASX_CAP_NONE);
    ASSERT_TRUE(asx_cx_is_cancelled(&cx));
}

TEST(is_cancelled_sees_the_cancel_of_a_masked_task) {
    /* A mask defers the checkpoint, not the cancel's visibility (Rust
     * is_cancel_requested_tracks_clear_and_mask_transitions,
     * cx.rs:6660-6664): is_cancelled reports it while the checkpoint is
     * Ok; unmasking lets the checkpoint observe it. */
    asx_cx cx;
    g_stub_task_id = 13;
    g_stub_task_state = ASX_TASK_CANCEL_REQUESTED;
    g_stub_mask_depth = 1;
    asx_cx_init(&cx, 1, 13, ASX_CAP_NONE);
    ASSERT_TRUE(asx_cx_is_cancelled(&cx));
    ASSERT_EQ(asx_cx_checkpoint(&cx), ASX_OK);
    g_stub_mask_depth = 0;
    ASSERT_TRUE(asx_cx_is_cancelled(&cx));
    ASSERT_EQ(asx_cx_checkpoint(&cx), ASX_E_CANCELLED);
}

TEST(is_cancelled_cancelling_returns_nonzero) {
    asx_cx cx;
    g_stub_task_id = 12;
    g_stub_task_state = ASX_TASK_CANCELLING;
    asx_cx_init(&cx, 1, 12, ASX_CAP_NONE);
    ASSERT_TRUE(asx_cx_is_cancelled(&cx));
}

/* ------------------------------------------------------------------ */
/* Checkpoint tests                                                    */
/* ------------------------------------------------------------------ */

TEST(checkpoint_null_fails) { ASSERT_EQ(asx_cx_checkpoint(NULL), ASX_E_INVALID_ARGUMENT); }

TEST(checkpoint_ok_no_cancel_no_budget) {
    asx_cx cx;
    g_stub_task_id = 20;
    g_stub_task_state = ASX_TASK_RUNNING;
    asx_cx_init(&cx, 1, 20, ASX_CAP_NONE);
    ASSERT_EQ(asx_cx_checkpoint(&cx), ASX_OK);
}

TEST(checkpoint_cancelled_returns_cancelled) {
    asx_cx cx;
    g_stub_task_id = 21;
    g_stub_task_state = ASX_TASK_CANCEL_REQUESTED;
    asx_cx_init(&cx, 1, 21, ASX_CAP_NONE);
    ASSERT_EQ(asx_cx_checkpoint(&cx), ASX_E_CANCELLED);
}

TEST(checkpoint_spends_no_poll_and_observes_a_spent_budget) {
    /* Rust's checkpoint spends no poll: the poll's unit is charged when the
     * task is polled, here by asx_cx_consume_poll, and the checkpoint turns
     * a spent quota into a cancel (checkpoint_budget_exhaustion,
     * cx.rs:3112-3160). */
    asx_cx cx;
    asx_budget b = asx_budget_from_polls(1);
    g_stub_task_id = 22;
    g_stub_task_state = ASX_TASK_RUNNING;
    g_stub_mask_depth = 0;
    asx_cx_init(&cx, 1, 22, ASX_CAP_BUDGET_READ | ASX_CAP_BUDGET_CONSUME);
    asx_cx_bind_budget(&cx, &b);
    ASSERT_EQ(asx_cx_checkpoint(&cx), ASX_OK);
    ASSERT_EQ(asx_cx_checkpoint(&cx), ASX_OK);
    ASSERT_EQ(asx_budget_polls(&b), 1u);
    ASSERT_EQ(asx_cx_consume_poll(&cx), ASX_OK);
    ASSERT_EQ(asx_budget_polls(&b), 0u);
    ASSERT_EQ(asx_cx_checkpoint(&cx), ASX_E_CANCELLED);
    ASSERT_EQ(asx_budget_polls(&b), 0u);
}

TEST(checkpoint_observes_a_spent_cost_quota) {
    asx_cx cx;
    asx_budget b = asx_budget_infinite();
    g_stub_task_id = 24;
    g_stub_task_state = ASX_TASK_RUNNING;
    g_stub_mask_depth = 0;
    b.cost_quota = 0u;
    asx_cx_init(&cx, 1, 24, ASX_CAP_BUDGET_READ);
    asx_cx_bind_budget(&cx, &b);
    ASSERT_EQ(asx_cx_checkpoint(&cx), ASX_E_CANCELLED);
}

TEST(checkpoint_defers_a_spent_budget_while_the_task_is_masked) {
    /* A mask defers the checkpoint's delivery, not the exhaustion: with no
     * cancel pending the stub task still reports a mask depth. */
    asx_cx cx;
    asx_budget b = asx_budget_zero();
    g_stub_task_id = 25;
    g_stub_task_state = ASX_TASK_RUNNING;
    g_stub_mask_depth = 1;
    asx_cx_init(&cx, 1, 25, ASX_CAP_BUDGET_READ);
    asx_cx_bind_budget(&cx, &b);
    ASSERT_EQ(asx_cx_checkpoint(&cx), ASX_OK);
    g_stub_mask_depth = 0;
    ASSERT_EQ(asx_cx_checkpoint(&cx), ASX_E_CANCELLED);
}

TEST(checkpoint_cancel_takes_priority_over_budget) {
    asx_cx cx;
    asx_budget b = asx_budget_from_polls(0);
    g_stub_task_id = 23;
    g_stub_task_state = ASX_TASK_CANCEL_REQUESTED;
    asx_cx_init(&cx, 1, 23, ASX_CAP_BUDGET_READ | ASX_CAP_BUDGET_CONSUME);
    asx_cx_bind_budget(&cx, &b);
    /* Cancel should be checked before budget */
    ASSERT_EQ(asx_cx_checkpoint(&cx), ASX_E_CANCELLED);
}

/* ------------------------------------------------------------------ */
/* Capability budget (resource envelopes; bd-9kll.3.27)                */
/* ------------------------------------------------------------------ */

#define CAP_REGION ((asx_region_id)77)

/* The stub region carries 1024 bytes of memory and 100 CPU units. */
static void cap_region_with_envelopes(void) {
    asx_capability_budget b = asx_capability_budget_unspecified();
    b = asx_capability_budget_with_memory_bytes(b, 1024u);
    b = asx_capability_budget_with_cpu_units(b, 100u);
    g_stub_region_id = CAP_REGION;
    g_stub_region_envelopes = b;
}

static void cap_no_region(void) {
    g_stub_region_id = ASX_INVALID_ID;
    g_stub_region_envelopes = asx_capability_budget_unspecified();
}

/* Field-wise: the struct has padding, so memcmp would compare bytes the
 * type does not define. */
static int cap_same(const asx_capability_budget *a, const asx_capability_budget *b) {
    return a->has_memory_bytes == b->has_memory_bytes && a->memory_bytes == b->memory_bytes &&
           a->has_cpu_units == b->has_cpu_units && a->cpu_units == b->cpu_units &&
           a->has_io_bytes == b->has_io_bytes && a->io_bytes == b->io_bytes &&
           a->has_cleanup_budget == b->has_cleanup_budget &&
           a->cleanup_budget.poll_quota == b->cleanup_budget.poll_quota &&
           a->has_artifact_bytes == b->has_artifact_bytes && a->artifact_bytes == b->artifact_bytes;
}

TEST(capability_budget_init_loads_the_regions_envelopes) {
    asx_cx cx;
    asx_capability_budget got;
    cap_region_with_envelopes();
    ASSERT_EQ(asx_cx_init(&cx, CAP_REGION, 1, ASX_CAP_NONE), ASX_OK);
    got = asx_cx_capability_budget(&cx);
    ASSERT_TRUE(got.has_memory_bytes);
    ASSERT_EQ(got.memory_bytes, (uint64_t)1024);
    ASSERT_TRUE(got.has_cpu_units);
    ASSERT_EQ(got.cpu_units, (uint64_t)100);
    ASSERT_FALSE(got.has_io_bytes);
    ASSERT_FALSE(got.has_cleanup_budget);
    ASSERT_FALSE(got.has_artifact_bytes);
    cap_no_region();
}

TEST(capability_budget_init_of_an_unheld_region_has_none) {
    asx_cx cx;
    asx_capability_budget got;
    cap_region_with_envelopes();
    ASSERT_EQ(asx_cx_init(&cx, 42, 1, ASX_CAP_NONE), ASX_OK); /* not the stub's */
    got = asx_cx_capability_budget(&cx);
    ASSERT_FALSE(got.has_memory_bytes);
    ASSERT_FALSE(got.has_cpu_units);
    cap_no_region();
}

TEST(capability_budget_null_cx_is_unspecified) {
    asx_capability_budget got = asx_cx_capability_budget(NULL);
    ASSERT_FALSE(got.has_memory_bytes);
    ASSERT_FALSE(got.has_cpu_units);
    ASSERT_FALSE(got.has_io_bytes);
    ASSERT_FALSE(got.has_cleanup_budget);
    ASSERT_FALSE(got.has_artifact_bytes);
}

TEST(capability_budget_narrow_and_attenuate_carry_the_envelopes) {
    asx_cx parent, narrowed, attenuated;
    cap_region_with_envelopes();
    ASSERT_EQ(asx_cx_init(&parent, CAP_REGION, 1, ASX_CAP_CLOCK_READ | ASX_CAP_ENTROPY), ASX_OK);
    cap_no_region(); /* what the children carry is the Cx's, not a fresh lookup */
    ASSERT_EQ(asx_cx_narrow(&parent, &narrowed, ASX_CAP_CLOCK_READ), ASX_OK);
    ASSERT_EQ(asx_cx_attenuate(&parent, &attenuated, ASX_CAP_ENTROPY), ASX_OK);
    ASSERT_EQ(asx_cx_capability_budget(&narrowed).memory_bytes, (uint64_t)1024);
    ASSERT_EQ(asx_cx_capability_budget(&narrowed).cpu_units, (uint64_t)100);
    ASSERT_EQ(asx_cx_capability_budget(&attenuated).memory_bytes, (uint64_t)1024);
    ASSERT_TRUE(asx_cx_capability_budget(&attenuated).has_cpu_units);
}

TEST(capability_budget_plan_child_tightens_without_changing_the_cx) {
    asx_cx cx;
    asx_capability_budget child = asx_capability_budget_unspecified();
    asx_capability_budget out;
    asx_capability_refusal refusal;
    cap_region_with_envelopes();
    ASSERT_EQ(asx_cx_init(&cx, CAP_REGION, 1, ASX_CAP_NONE), ASX_OK);
    cap_no_region();

    child = asx_capability_budget_with_memory_bytes(child, 256u);
    child = asx_capability_budget_with_io_bytes(child, 4096u);
    ASSERT_EQ(asx_cx_plan_child_capability_budget(&cx, &child, NULL, &out, &refusal), ASX_OK);
    ASSERT_EQ(out.memory_bytes, (uint64_t)256); /* tightened */
    ASSERT_EQ(out.cpu_units, (uint64_t)100);    /* inherited */
    ASSERT_TRUE(out.has_io_bytes);              /* the child's own */
    ASSERT_EQ(out.io_bytes, (uint64_t)4096);
    /* Read-only (Rust plan_child_capability_budget, cx.rs:2588). */
    ASSERT_EQ(asx_cx_capability_budget(&cx).memory_bytes, (uint64_t)1024);
    ASSERT_FALSE(asx_cx_capability_budget(&cx).has_io_bytes);
}

TEST(capability_budget_plan_child_refusal_names_the_dimension_and_writes_nothing) {
    asx_cx cx;
    asx_capability_budget out;
    asx_capability_budget before;
    asx_capability_requirements req = {0, 0, 1, 0, 0}; /* io_bytes */
    asx_capability_refusal refusal;
    cap_region_with_envelopes();
    ASSERT_EQ(asx_cx_init(&cx, CAP_REGION, 1, ASX_CAP_NONE), ASX_OK);
    cap_no_region();

    out = asx_capability_budget_with_artifact_bytes(asx_capability_budget_unspecified(), 0xABu);
    before = out;
    ASSERT_EQ(asx_cx_plan_child_capability_budget(&cx, NULL, &req, &out, &refusal),
              ASX_E_CAPABILITY_BUDGET_REFUSED);
    ASSERT_EQ(refusal.kind, ASX_CAPABILITY_MISSING_REQUIRED);
    ASSERT_EQ(refusal.dimension, ASX_CAPABILITY_IO_BYTES);
    ASSERT_TRUE(cap_same(&out, &before)); /* not written */

    /* The permitted near-identical case: requiring what the Cx has. */
    req.io_bytes = 0u;
    req.cpu_units = 1u;
    ASSERT_EQ(asx_cx_plan_child_capability_budget(&cx, NULL, &req, &out, &refusal), ASX_OK);
    ASSERT_EQ(out.cpu_units, (uint64_t)100);
}

TEST(capability_budget_apply_child_tightens_and_never_loosens) {
    asx_cx cx;
    asx_capability_budget tighter = asx_capability_budget_unspecified();
    asx_capability_budget looser = asx_capability_budget_unspecified();
    asx_capability_budget out;
    asx_capability_refusal refusal;
    cap_region_with_envelopes();
    ASSERT_EQ(asx_cx_init(&cx, CAP_REGION, 1, ASX_CAP_NONE), ASX_OK);
    cap_no_region();

    tighter = asx_capability_budget_with_memory_bytes(tighter, 512u);
    ASSERT_EQ(asx_cx_apply_child_capability_budget(&cx, &tighter, NULL, &out, &refusal), ASX_OK);
    ASSERT_EQ(out.memory_bytes, (uint64_t)512);
    ASSERT_EQ(asx_cx_capability_budget(&cx).memory_bytes, (uint64_t)512);
    ASSERT_EQ(asx_cx_capability_budget(&cx).cpu_units, (uint64_t)100);

    looser = asx_capability_budget_with_memory_bytes(looser, 4096u);
    looser = asx_capability_budget_with_artifact_bytes(looser, 9u);
    ASSERT_EQ(asx_cx_apply_child_capability_budget(&cx, &looser, NULL, NULL, &refusal), ASX_OK);
    ASSERT_EQ(asx_cx_capability_budget(&cx).memory_bytes, (uint64_t)512); /* not 4096 */
    ASSERT_TRUE(asx_cx_capability_budget(&cx).has_artifact_bytes);        /* a new bound */
    ASSERT_EQ(asx_cx_capability_budget(&cx).artifact_bytes, (uint64_t)9);
}

TEST(capability_budget_apply_child_refusal_leaves_the_cx_unchanged) {
    asx_cx cx;
    asx_capability_budget child = asx_capability_budget_unspecified();
    asx_capability_budget before;
    asx_capability_budget after;
    asx_capability_requirements req = {0, 0, 0, 1, 0}; /* cleanup */
    asx_capability_refusal refusal;
    cap_region_with_envelopes();
    ASSERT_EQ(asx_cx_init(&cx, CAP_REGION, 1, ASX_CAP_NONE), ASX_OK);
    cap_no_region();

    /* A child that tightens memory but misses a required dimension: the
     * tightening is not applied. */
    child = asx_capability_budget_with_memory_bytes(child, 8u);
    before = asx_cx_capability_budget(&cx);
    ASSERT_EQ(asx_cx_apply_child_capability_budget(&cx, &child, &req, NULL, &refusal),
              ASX_E_CAPABILITY_BUDGET_REFUSED);
    ASSERT_EQ(refusal.kind, ASX_CAPABILITY_MISSING_REQUIRED);
    ASSERT_EQ(refusal.dimension, ASX_CAPABILITY_CLEANUP);
    after = asx_cx_capability_budget(&cx);
    ASSERT_TRUE(cap_same(&before, &after));
    ASSERT_EQ(after.memory_bytes, (uint64_t)1024);

    /* The near-identical case supplying the dimension is admitted. */
    {
        asx_budget cleanup = asx_budget_from_polls(4);
        child = asx_capability_budget_with_cleanup_budget(child, &cleanup);
    }
    ASSERT_EQ(asx_cx_apply_child_capability_budget(&cx, &child, &req, NULL, &refusal), ASX_OK);
    ASSERT_EQ(asx_cx_capability_budget(&cx).memory_bytes, (uint64_t)8);
    ASSERT_TRUE(asx_cx_capability_budget(&cx).has_cleanup_budget);
}

TEST(capability_budget_a_tightened_cx_narrows_to_a_tightened_child) {
    asx_cx cx, narrowed;
    asx_capability_budget child = asx_capability_budget_unspecified();
    cap_region_with_envelopes();
    ASSERT_EQ(asx_cx_init(&cx, CAP_REGION, 1, ASX_CAP_CLOCK_READ), ASX_OK);
    cap_no_region();

    child = asx_capability_budget_with_cpu_units(child, 10u);
    ASSERT_EQ(asx_cx_apply_child_capability_budget(&cx, &child, NULL, NULL, NULL), ASX_OK);
    ASSERT_EQ(asx_cx_narrow(&cx, &narrowed, ASX_CAP_CLOCK_READ), ASX_OK);
    ASSERT_EQ(asx_cx_capability_budget(&narrowed).cpu_units, (uint64_t)10);

    /* A Cx is a value: tightening the narrowed copy leaves its parent as it
     * was (Rust's clones share one inner state; documented in cx.h). */
    child = asx_capability_budget_with_cpu_units(asx_capability_budget_unspecified(), 2u);
    ASSERT_EQ(asx_cx_apply_child_capability_budget(&narrowed, &child, NULL, NULL, NULL), ASX_OK);
    ASSERT_EQ(asx_cx_capability_budget(&narrowed).cpu_units, (uint64_t)2);
    ASSERT_EQ(asx_cx_capability_budget(&cx).cpu_units, (uint64_t)10);
}

TEST(capability_budget_plan_and_apply_reject_a_null_cx) {
    asx_capability_budget out;
    asx_capability_refusal refusal;
    ASSERT_EQ(asx_cx_plan_child_capability_budget(NULL, NULL, NULL, &out, &refusal),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_cx_apply_child_capability_budget(NULL, NULL, NULL, &out, &refusal),
              ASX_E_INVALID_ARGUMENT);
}

/* ------------------------------------------------------------------ */
/* Permission denied status string test                                */
/* ------------------------------------------------------------------ */

TEST(permission_denied_status_str) {
    ASSERT_STR_EQ(asx_status_str(ASX_E_PERMISSION_DENIED), "capability not granted");
}

TEST(capability_budget_refused_status_str) {
    ASSERT_STR_EQ(asx_status_str(ASX_E_CAPABILITY_BUDGET_REFUSED), "capability budget refused");
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    fprintf(stderr, "=== test_cx ===\n");

    /* Lifecycle */
    RUN_TEST(init_null_returns_invalid_argument);
    RUN_TEST(init_invalid_region_returns_invalid_argument);
    RUN_TEST(init_sets_fields);
    RUN_TEST(init_generation_increments);
    RUN_TEST(invalidate_clears_all);
    RUN_TEST(invalidate_null_safe);
    RUN_TEST(is_valid_null);

    /* Identity */
    RUN_TEST(region_null_returns_invalid);
    RUN_TEST(task_null_returns_invalid);

    /* Capabilities */
    RUN_TEST(has_cap_null_returns_zero);
    RUN_TEST(caps_null_returns_none);
    RUN_TEST(caps_returns_all_set);
    RUN_TEST(has_cap_multi_flag_check);

    /* Narrowing */
    RUN_TEST(narrow_null_parent_fails);
    RUN_TEST(narrow_null_child_fails);
    RUN_TEST(narrow_invalid_parent_fails);
    RUN_TEST(narrow_escalation_fails);
    RUN_TEST(narrow_subset_succeeds);
    RUN_TEST(narrow_to_none_succeeds);
    RUN_TEST(attenuate_masks_parent_caps);
    RUN_TEST(wrap_and_unwrap_subset_succeeds);
    RUN_TEST(wrap_escalation_fails);
    RUN_TEST(unwrap_stale_wrapper_fails_closed);
    RUN_TEST(registry_issue_materialize_and_revoke);
    RUN_TEST(registry_materialize_wrong_parent_fails_closed);
    RUN_TEST(registry_capacity_exhaustion_fails);
    RUN_TEST(registry_issue_escalation_fails);
    RUN_TEST(macaroon_issue_attenuate_and_bind);
    RUN_TEST(macaroon_bind_stale_parent_fails_closed);
    RUN_TEST(macaroon_signature_is_hmac_chain);
    RUN_TEST(macaroon_forged_escalation_rejected);
    RUN_TEST(macaroon_wrong_root_key_rejected);
    RUN_TEST(macaroon_caveat_capacity_failure_atomic);

    /* Budget */
    RUN_TEST(bind_budget_null_cx_fails);
    RUN_TEST(bind_budget_null_budget_fails);
    RUN_TEST(bind_budget_no_cap_fails);
    RUN_TEST(bind_budget_and_read);
    RUN_TEST(budget_null_cx_returns_null);
    RUN_TEST(budget_no_read_cap_returns_null);
    RUN_TEST(consume_poll_null_fails);
    RUN_TEST(consume_poll_no_cap_fails);
    RUN_TEST(consume_poll_no_budget_unlimited);
    RUN_TEST(consume_poll_decrements);

    /* Clock */
    RUN_TEST(bind_clock_null_cx_fails);
    RUN_TEST(bind_clock_null_clock_fails);
    RUN_TEST(bind_clock_no_cap_fails);
    RUN_TEST(now_reads_bound_clock);
    RUN_TEST(now_no_clock_returns_zero);
    RUN_TEST(now_null_returns_zero);
    RUN_TEST(now_no_cap_returns_zero);

    /* Entropy */
    RUN_TEST(bind_entropy_null_cx_fails);
    RUN_TEST(bind_entropy_null_state_fails);
    RUN_TEST(bind_entropy_no_cap_fails);
    RUN_TEST(random_u64_draws_nonzero);
    RUN_TEST(random_u64_null_returns_zero);
    RUN_TEST(random_u64_no_cap_returns_zero);
    RUN_TEST(random_u64_no_state_returns_zero);

    /* Cancellation */
    RUN_TEST(is_cancelled_null_returns_zero);
    RUN_TEST(cancel_observation_needs_no_capability);
    RUN_TEST(is_cancelled_no_task_returns_zero);
    RUN_TEST(is_cancelled_running_returns_zero);
    RUN_TEST(is_cancelled_cancel_requested_returns_nonzero);
    RUN_TEST(is_cancelled_sees_the_cancel_of_a_masked_task);
    RUN_TEST(is_cancelled_cancelling_returns_nonzero);

    /* Checkpoint */
    RUN_TEST(checkpoint_null_fails);
    RUN_TEST(checkpoint_ok_no_cancel_no_budget);
    RUN_TEST(checkpoint_cancelled_returns_cancelled);
    RUN_TEST(checkpoint_spends_no_poll_and_observes_a_spent_budget);
    RUN_TEST(checkpoint_observes_a_spent_cost_quota);
    RUN_TEST(checkpoint_defers_a_spent_budget_while_the_task_is_masked);
    RUN_TEST(checkpoint_cancel_takes_priority_over_budget);

    /* Capability budget */
    RUN_TEST(capability_budget_init_loads_the_regions_envelopes);
    RUN_TEST(capability_budget_init_of_an_unheld_region_has_none);
    RUN_TEST(capability_budget_null_cx_is_unspecified);
    RUN_TEST(capability_budget_narrow_and_attenuate_carry_the_envelopes);
    RUN_TEST(capability_budget_plan_child_tightens_without_changing_the_cx);
    RUN_TEST(capability_budget_plan_child_refusal_names_the_dimension_and_writes_nothing);
    RUN_TEST(capability_budget_apply_child_tightens_and_never_loosens);
    RUN_TEST(capability_budget_apply_child_refusal_leaves_the_cx_unchanged);
    RUN_TEST(capability_budget_a_tightened_cx_narrows_to_a_tightened_child);
    RUN_TEST(capability_budget_plan_and_apply_reject_a_null_cx);

    /* Status string */
    RUN_TEST(permission_denied_status_str);
    RUN_TEST(capability_budget_refused_status_str);

    TEST_REPORT();
    return test_failures;
}
