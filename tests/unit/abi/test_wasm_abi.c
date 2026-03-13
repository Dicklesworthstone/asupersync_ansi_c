/*
 * test_wasm_abi.c — unit tests for wasm ABI type contracts and transitions
 *
 * Bead: bd-1eqo.15.4
 *
 * Tests: ABI version, compatibility checking, bootstrap transition matrix,
 * provider transition matrix, hook transition matrix, string helpers,
 * error envelope types, and boundary payload types.
 *
 * SPDX-License-Identifier: MIT
 */

#include "test_harness.h"
#include <asx/abi/wasm_abi.h>
#include <string.h>

/* ================================================================== */
/* ABI version and signature                                           */
/* ================================================================== */

TEST(abi_version_current_returns_valid) {
    asx_abi_version v = asx_abi_version_current();
    ASSERT_EQ(v.major, ASX_WASM_ABI_VERSION_MAJOR);
    ASSERT_EQ(v.minor, ASX_WASM_ABI_VERSION_MINOR);
    ASSERT_EQ(v.patch, ASX_WASM_ABI_VERSION_PATCH);
    /* Core features should always be present */
    ASSERT_TRUE(v.feature_flags & ASX_ABI_FEATURE_CHANNELS);
    ASSERT_TRUE(v.feature_flags & ASX_ABI_FEATURE_TIMERS);
    ASSERT_TRUE(v.feature_flags & ASX_ABI_FEATURE_OBLIGATIONS);
    ASSERT_TRUE(v.feature_flags & ASX_ABI_FEATURE_SYMBOLS);
}

TEST(abi_signature_encodes_version) {
    asx_abi_signature sig = asx_abi_signature_compute();
    uint8_t major = (uint8_t)(sig >> 56);
    uint8_t minor = (uint8_t)(sig >> 48);
    uint16_t patch = (uint16_t)(sig >> 32);
    ASSERT_EQ(major, ASX_WASM_ABI_VERSION_MAJOR);
    ASSERT_EQ(minor, ASX_WASM_ABI_VERSION_MINOR);
    ASSERT_EQ(patch, ASX_WASM_ABI_VERSION_PATCH);
}

TEST(abi_signature_is_deterministic) {
    asx_abi_signature a = asx_abi_signature_compute();
    asx_abi_signature b = asx_abi_signature_compute();
    ASSERT_EQ(a, b);
}

/* ================================================================== */
/* Compatibility checking                                              */
/* ================================================================== */

TEST(compat_same_version_is_compatible) {
    asx_abi_version local = {0, 1, 0, 0x1F};
    asx_abi_version remote = {0, 1, 0, 0x1F};
    asx_abi_compat_result result;
    asx_status s = asx_abi_check_compat(&local, &remote, &result);
    ASSERT_EQ(s, ASX_OK);
    ASSERT_EQ(result.compat, ASX_ABI_COMPATIBLE);
    ASSERT_EQ(result.missing_features, 0u);
    ASSERT_EQ(result.extra_features, 0u);
}

TEST(compat_major_mismatch_is_incompatible) {
    asx_abi_version local = {0, 1, 0, 0};
    asx_abi_version remote = {1, 0, 0, 0};
    asx_abi_compat_result result;
    asx_status s = asx_abi_check_compat(&local, &remote, &result);
    ASSERT_EQ(s, ASX_OK);
    ASSERT_EQ(result.compat, ASX_ABI_INCOMPATIBLE);
}

TEST(compat_older_minor_is_degraded) {
    asx_abi_version local = {0, 1, 0, 0x1F};
    asx_abi_version remote = {0, 2, 0, 0x1F};
    asx_abi_compat_result result;
    asx_status s = asx_abi_check_compat(&local, &remote, &result);
    ASSERT_EQ(s, ASX_OK);
    ASSERT_EQ(result.compat, ASX_ABI_DEGRADED);
}

TEST(compat_missing_features_is_degraded) {
    asx_abi_version local = {0, 1, 0, ASX_ABI_FEATURE_CHANNELS};
    asx_abi_version remote = {0, 1, 0, ASX_ABI_FEATURE_CHANNELS | ASX_ABI_FEATURE_SYMBOLS};
    asx_abi_compat_result result;
    asx_status s = asx_abi_check_compat(&local, &remote, &result);
    ASSERT_EQ(s, ASX_OK);
    ASSERT_EQ(result.compat, ASX_ABI_DEGRADED);
    ASSERT_TRUE(result.missing_features & ASX_ABI_FEATURE_SYMBOLS);
}

TEST(compat_extra_features_still_compatible) {
    asx_abi_version local = {0, 1, 0, 0xFF};
    asx_abi_version remote = {0, 1, 0, 0x0F};
    asx_abi_compat_result result;
    asx_status s = asx_abi_check_compat(&local, &remote, &result);
    ASSERT_EQ(s, ASX_OK);
    ASSERT_EQ(result.compat, ASX_ABI_COMPATIBLE);
    ASSERT_TRUE(result.extra_features != 0u);
}

TEST(compat_null_args_return_error) {
    asx_abi_version v = {0, 1, 0, 0};
    asx_abi_compat_result result;
    ASSERT_EQ(asx_abi_check_compat(NULL, &v, &result), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_abi_check_compat(&v, NULL, &result), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_abi_check_compat(&v, &v, NULL), ASX_E_INVALID_ARGUMENT);
}

/* ================================================================== */
/* Compat string                                                       */
/* ================================================================== */

TEST(compat_str_all_values) {
    ASSERT_TRUE(strcmp(asx_abi_compat_str(ASX_ABI_COMPATIBLE), "compatible") == 0);
    ASSERT_TRUE(strcmp(asx_abi_compat_str(ASX_ABI_DEGRADED), "degraded") == 0);
    ASSERT_TRUE(strcmp(asx_abi_compat_str(ASX_ABI_INCOMPATIBLE), "incompatible") == 0);
    ASSERT_TRUE(strcmp(asx_abi_compat_str((asx_abi_compat)99), "unknown") == 0);
}

/* ================================================================== */
/* Bootstrap transition matrix                                         */
/* ================================================================== */

TEST(boot_legal_forward_transitions) {
    ASSERT_EQ(asx_boot_transition_check(ASX_BOOT_PHASE_UNINITIALIZED, ASX_BOOT_PHASE_ABI_CHECK),
              ASX_OK);
    ASSERT_EQ(asx_boot_transition_check(ASX_BOOT_PHASE_ABI_CHECK, ASX_BOOT_PHASE_HOOKS_BIND),
              ASX_OK);
    ASSERT_EQ(asx_boot_transition_check(ASX_BOOT_PHASE_HOOKS_BIND, ASX_BOOT_PHASE_PROVIDER_INIT),
              ASX_OK);
    ASSERT_EQ(asx_boot_transition_check(ASX_BOOT_PHASE_PROVIDER_INIT, ASX_BOOT_PHASE_RUNTIME_INIT),
              ASX_OK);
    ASSERT_EQ(asx_boot_transition_check(ASX_BOOT_PHASE_RUNTIME_INIT, ASX_BOOT_PHASE_READY), ASX_OK);
    ASSERT_EQ(asx_boot_transition_check(ASX_BOOT_PHASE_READY, ASX_BOOT_PHASE_SHUTDOWN), ASX_OK);
    ASSERT_EQ(asx_boot_transition_check(ASX_BOOT_PHASE_SHUTDOWN, ASX_BOOT_PHASE_TERMINATED),
              ASX_OK);
}

TEST(boot_failure_transitions) {
    ASSERT_EQ(asx_boot_transition_check(ASX_BOOT_PHASE_ABI_CHECK, ASX_BOOT_PHASE_FAILED), ASX_OK);
    ASSERT_EQ(asx_boot_transition_check(ASX_BOOT_PHASE_HOOKS_BIND, ASX_BOOT_PHASE_FAILED), ASX_OK);
    ASSERT_EQ(asx_boot_transition_check(ASX_BOOT_PHASE_PROVIDER_INIT, ASX_BOOT_PHASE_FAILED),
              ASX_OK);
    ASSERT_EQ(asx_boot_transition_check(ASX_BOOT_PHASE_RUNTIME_INIT, ASX_BOOT_PHASE_FAILED),
              ASX_OK);
    ASSERT_EQ(asx_boot_transition_check(ASX_BOOT_PHASE_SHUTDOWN, ASX_BOOT_PHASE_FAILED), ASX_OK);
}

TEST(boot_recovery_transitions) {
    ASSERT_EQ(asx_boot_transition_check(ASX_BOOT_PHASE_FAILED, ASX_BOOT_PHASE_UNINITIALIZED),
              ASX_OK);
    ASSERT_EQ(asx_boot_transition_check(ASX_BOOT_PHASE_TERMINATED, ASX_BOOT_PHASE_UNINITIALIZED),
              ASX_OK);
}

TEST(boot_illegal_transitions) {
    /* Can't skip phases */
    ASSERT_EQ(asx_boot_transition_check(ASX_BOOT_PHASE_UNINITIALIZED, ASX_BOOT_PHASE_READY),
              ASX_E_INVALID_TRANSITION);
    /* Can't go backwards (except via recovery) */
    ASSERT_EQ(asx_boot_transition_check(ASX_BOOT_PHASE_READY, ASX_BOOT_PHASE_ABI_CHECK),
              ASX_E_INVALID_TRANSITION);
    /* Terminated is terminal except for re-bootstrap */
    ASSERT_EQ(asx_boot_transition_check(ASX_BOOT_PHASE_TERMINATED, ASX_BOOT_PHASE_READY),
              ASX_E_INVALID_TRANSITION);
    /* Self-transitions are illegal */
    ASSERT_EQ(asx_boot_transition_check(ASX_BOOT_PHASE_READY, ASX_BOOT_PHASE_READY),
              ASX_E_INVALID_TRANSITION);
}

TEST(boot_transition_matrix_exhaustive) {
    /* Expected legal transitions: bits encoded per-from-state */
    static const uint16_t expected[ASX_BOOT_PHASE_COUNT] = {
        /* UNINIT  */ (1u << 1),
        /* ABI_CHK */ (1u << 2) | (1u << 8),
        /* HOOKS   */ (1u << 3) | (1u << 8),
        /* PROVID  */ (1u << 4) | (1u << 8),
        /* RT_INIT */ (1u << 5) | (1u << 8),
        /* READY   */ (1u << 6),
        /* SHUTDN  */ (1u << 7) | (1u << 8),
        /* TERMED  */ (1u << 0),
        /* FAILED  */ (1u << 0),
    };
    int from, to;
    for (from = 0; from < ASX_BOOT_PHASE_COUNT; from++) {
        for (to = 0; to < ASX_BOOT_PHASE_COUNT; to++) {
            asx_status s = asx_boot_transition_check((asx_boot_phase)from, (asx_boot_phase)to);
            if (expected[from] & (1u << to)) {
                ASSERT_EQ(s, ASX_OK);
            } else {
                ASSERT_EQ(s, ASX_E_INVALID_TRANSITION);
            }
        }
    }
}

TEST(boot_out_of_range) {
    ASSERT_EQ(asx_boot_transition_check((asx_boot_phase)-1, ASX_BOOT_PHASE_ABI_CHECK),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_boot_transition_check(ASX_BOOT_PHASE_COUNT, ASX_BOOT_PHASE_ABI_CHECK),
              ASX_E_INVALID_ARGUMENT);
}

/* ================================================================== */
/* Provider transition matrix                                          */
/* ================================================================== */

TEST(provider_legal_lifecycle) {
    ASSERT_EQ(asx_provider_transition_check(ASX_PROVIDER_UNREGISTERED, ASX_PROVIDER_REGISTERING),
              ASX_OK);
    ASSERT_EQ(asx_provider_transition_check(ASX_PROVIDER_REGISTERING, ASX_PROVIDER_ACTIVE), ASX_OK);
    ASSERT_EQ(asx_provider_transition_check(ASX_PROVIDER_ACTIVE, ASX_PROVIDER_SUSPENDING), ASX_OK);
    ASSERT_EQ(asx_provider_transition_check(ASX_PROVIDER_SUSPENDING, ASX_PROVIDER_SUSPENDED),
              ASX_OK);
    ASSERT_EQ(asx_provider_transition_check(ASX_PROVIDER_SUSPENDED, ASX_PROVIDER_ACTIVE), ASX_OK);
    ASSERT_EQ(asx_provider_transition_check(ASX_PROVIDER_ACTIVE, ASX_PROVIDER_REMOVING), ASX_OK);
    ASSERT_EQ(asx_provider_transition_check(ASX_PROVIDER_REMOVING, ASX_PROVIDER_REMOVED), ASX_OK);
}

TEST(provider_failure_recovery) {
    ASSERT_EQ(asx_provider_transition_check(ASX_PROVIDER_REGISTERING, ASX_PROVIDER_FAILED), ASX_OK);
    ASSERT_EQ(asx_provider_transition_check(ASX_PROVIDER_SUSPENDING, ASX_PROVIDER_FAILED), ASX_OK);
    ASSERT_EQ(asx_provider_transition_check(ASX_PROVIDER_REMOVING, ASX_PROVIDER_FAILED), ASX_OK);
    ASSERT_EQ(asx_provider_transition_check(ASX_PROVIDER_FAILED, ASX_PROVIDER_UNREGISTERED),
              ASX_OK);
}

TEST(provider_removed_is_terminal) {
    int to;
    for (to = 0; to < ASX_PROVIDER_STATE_COUNT; to++) {
        ASSERT_EQ(asx_provider_transition_check(ASX_PROVIDER_REMOVED, (asx_provider_state)to),
                  ASX_E_INVALID_TRANSITION);
    }
}

TEST(provider_exhaustive_matrix) {
    static const uint8_t expected[ASX_PROVIDER_STATE_COUNT] = {
        /* UNREG    */ (1u << 1),
        /* REGING   */ (1u << 2) | (1u << 7),
        /* ACTIVE   */ (1u << 3) | (1u << 5),
        /* SUSPDING */ (1u << 4) | (1u << 7),
        /* SUSPDED  */ (1u << 2) | (1u << 5),
        /* REMOVING */ (1u << 6) | (1u << 7),
        /* REMOVED  */ 0u,
        /* FAILED   */ (1u << 0),
    };
    int from, to;
    for (from = 0; from < ASX_PROVIDER_STATE_COUNT; from++) {
        for (to = 0; to < ASX_PROVIDER_STATE_COUNT; to++) {
            asx_status s =
                asx_provider_transition_check((asx_provider_state)from, (asx_provider_state)to);
            if (expected[from] & (1u << to)) {
                ASSERT_EQ(s, ASX_OK);
            } else {
                ASSERT_EQ(s, ASX_E_INVALID_TRANSITION);
            }
        }
    }
}

/* ================================================================== */
/* Hook transition matrix                                              */
/* ================================================================== */

TEST(hook_legal_lifecycle) {
    ASSERT_EQ(asx_hook_transition_check(ASX_HOOK_UNBOUND, ASX_HOOK_BINDING), ASX_OK);
    ASSERT_EQ(asx_hook_transition_check(ASX_HOOK_BINDING, ASX_HOOK_BOUND), ASX_OK);
    ASSERT_EQ(asx_hook_transition_check(ASX_HOOK_BOUND, ASX_HOOK_REPLACING), ASX_OK);
    ASSERT_EQ(asx_hook_transition_check(ASX_HOOK_REPLACING, ASX_HOOK_BOUND), ASX_OK);
    ASSERT_EQ(asx_hook_transition_check(ASX_HOOK_BOUND, ASX_HOOK_UNBOUND), ASX_OK);
}

TEST(hook_failure_paths) {
    ASSERT_EQ(asx_hook_transition_check(ASX_HOOK_BINDING, ASX_HOOK_UNBOUND_FAILED), ASX_OK);
    ASSERT_EQ(asx_hook_transition_check(ASX_HOOK_REPLACING, ASX_HOOK_UNBOUND_FAILED), ASX_OK);
    ASSERT_EQ(asx_hook_transition_check(ASX_HOOK_UNBOUND_FAILED, ASX_HOOK_BINDING), ASX_OK);
}

TEST(hook_exhaustive_matrix) {
    static const uint8_t expected[ASX_HOOK_STATE_COUNT] = {
        /* UNBOUND       */ (1u << 1),
        /* BINDING       */ (1u << 2) | (1u << 4),
        /* BOUND         */ (1u << 3) | (1u << 0),
        /* REPLACING     */ (1u << 2) | (1u << 4),
        /* UNBOUND_FAIL  */ (1u << 1),
    };
    int from, to;
    for (from = 0; from < ASX_HOOK_STATE_COUNT; from++) {
        for (to = 0; to < ASX_HOOK_STATE_COUNT; to++) {
            asx_status s = asx_hook_transition_check((asx_hook_state)from, (asx_hook_state)to);
            if (expected[from] & (1u << to)) {
                ASSERT_EQ(s, ASX_OK);
            } else {
                ASSERT_EQ(s, ASX_E_INVALID_TRANSITION);
            }
        }
    }
}

/* ================================================================== */
/* String helpers                                                      */
/* ================================================================== */

TEST(boot_phase_str_all) {
    ASSERT_TRUE(strcmp(asx_boot_phase_str(ASX_BOOT_PHASE_UNINITIALIZED), "uninitialized") == 0);
    ASSERT_TRUE(strcmp(asx_boot_phase_str(ASX_BOOT_PHASE_READY), "ready") == 0);
    ASSERT_TRUE(strcmp(asx_boot_phase_str(ASX_BOOT_PHASE_FAILED), "failed") == 0);
    ASSERT_TRUE(strcmp(asx_boot_phase_str((asx_boot_phase)99), "unknown") == 0);
}

TEST(provider_state_str_all) {
    ASSERT_TRUE(strcmp(asx_provider_state_str(ASX_PROVIDER_UNREGISTERED), "unregistered") == 0);
    ASSERT_TRUE(strcmp(asx_provider_state_str(ASX_PROVIDER_ACTIVE), "active") == 0);
    ASSERT_TRUE(strcmp(asx_provider_state_str(ASX_PROVIDER_REMOVED), "removed") == 0);
    ASSERT_TRUE(strcmp(asx_provider_state_str((asx_provider_state)99), "unknown") == 0);
}

TEST(hook_state_str_all) {
    ASSERT_TRUE(strcmp(asx_hook_state_str(ASX_HOOK_UNBOUND), "unbound") == 0);
    ASSERT_TRUE(strcmp(asx_hook_state_str(ASX_HOOK_BOUND), "bound") == 0);
    ASSERT_TRUE(strcmp(asx_hook_state_str(ASX_HOOK_UNBOUND_FAILED), "unbound_failed") == 0);
    ASSERT_TRUE(strcmp(asx_hook_state_str((asx_hook_state)99), "unknown") == 0);
}

TEST(recoverability_str_all) {
    ASSERT_TRUE(strcmp(asx_recoverability_str(ASX_RECOVER_NONE), "none") == 0);
    ASSERT_TRUE(strcmp(asx_recoverability_str(ASX_RECOVER_RETRY), "retry") == 0);
    ASSERT_TRUE(strcmp(asx_recoverability_str(ASX_RECOVER_DEGRADE), "degrade") == 0);
    ASSERT_TRUE(strcmp(asx_recoverability_str(ASX_RECOVER_RECONNECT), "reconnect") == 0);
    ASSERT_TRUE(strcmp(asx_recoverability_str((asx_recoverability)99), "unknown") == 0);
}

/* ================================================================== */
/* Boundary type layout sanity                                         */
/* ================================================================== */

TEST(abi_bytes_layout) {
    asx_abi_bytes b;
    b.data = (const uint8_t *)"hello";
    b.len = 5;
    ASSERT_TRUE(b.data != NULL);
    ASSERT_EQ(b.len, 5u);
}

TEST(abi_request_layout) {
    asx_abi_request req;
    memset(&req, 0, sizeof(req));
    req.request_id = 42;
    req.method = 1;
    req.timeout_ms = 5000;
    ASSERT_EQ(req.request_id, 42u);
    ASSERT_EQ(req.method, 1u);
}

TEST(abi_response_layout) {
    asx_abi_response resp;
    memset(&resp, 0, sizeof(resp));
    resp.request_id = 42;
    resp.status = ASX_OK;
    resp.recoverability = ASX_RECOVER_NONE;
    ASSERT_EQ(resp.request_id, 42u);
    ASSERT_EQ(resp.status, ASX_OK);
}

TEST(abi_task_context_layout) {
    asx_abi_task_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.task_id = 1;
    ctx.region_id = 2;
    ctx.budget_poll = 100;
    ctx.abi_version = ASX_WASM_ABI_VERSION;
    ASSERT_EQ(ctx.task_id, 1u);
    ASSERT_EQ(ctx.abi_version, ASX_WASM_ABI_VERSION);
}

TEST(error_envelope_layout) {
    asx_abi_error_envelope env;
    memset(&env, 0, sizeof(env));
    env.status = ASX_E_INVALID_TRANSITION;
    env.recoverability = ASX_RECOVER_DEGRADE;
    env.boot_phase = ASX_BOOT_PHASE_HOOKS_BIND;
    env.message = "hook bind failed";
    ASSERT_EQ(env.status, ASX_E_INVALID_TRANSITION);
    ASSERT_EQ(env.recoverability, ASX_RECOVER_DEGRADE);
    ASSERT_TRUE(env.message != NULL);
}

/* ================================================================== */
/* Main                                                                */
/* ================================================================== */

int main(void) {
    /* ABI version/signature */
    RUN_TEST(abi_version_current_returns_valid);
    RUN_TEST(abi_signature_encodes_version);
    RUN_TEST(abi_signature_is_deterministic);

    /* Compatibility */
    RUN_TEST(compat_same_version_is_compatible);
    RUN_TEST(compat_major_mismatch_is_incompatible);
    RUN_TEST(compat_older_minor_is_degraded);
    RUN_TEST(compat_missing_features_is_degraded);
    RUN_TEST(compat_extra_features_still_compatible);
    RUN_TEST(compat_null_args_return_error);
    RUN_TEST(compat_str_all_values);

    /* Bootstrap transitions */
    RUN_TEST(boot_legal_forward_transitions);
    RUN_TEST(boot_failure_transitions);
    RUN_TEST(boot_recovery_transitions);
    RUN_TEST(boot_illegal_transitions);
    RUN_TEST(boot_transition_matrix_exhaustive);
    RUN_TEST(boot_out_of_range);

    /* Provider transitions */
    RUN_TEST(provider_legal_lifecycle);
    RUN_TEST(provider_failure_recovery);
    RUN_TEST(provider_removed_is_terminal);
    RUN_TEST(provider_exhaustive_matrix);

    /* Hook transitions */
    RUN_TEST(hook_legal_lifecycle);
    RUN_TEST(hook_failure_paths);
    RUN_TEST(hook_exhaustive_matrix);

    /* String helpers */
    RUN_TEST(boot_phase_str_all);
    RUN_TEST(provider_state_str_all);
    RUN_TEST(hook_state_str_all);
    RUN_TEST(recoverability_str_all);

    /* Boundary type layouts */
    RUN_TEST(abi_bytes_layout);
    RUN_TEST(abi_request_layout);
    RUN_TEST(abi_response_layout);
    RUN_TEST(abi_task_context_layout);
    RUN_TEST(error_envelope_layout);

    TEST_REPORT();
    return test_failures;
}
