/*
 * test_config_reload.c — Hot-reload config boundary tests (bd-3vt.9)
 *
 * Validates:
 * - Field classification (reloadable, frozen, restart-required)
 * - Atomic reload with validation
 * - Rollback on invalid updates
 * - Rejection of frozen field changes
 * - Rejection of restart-required field changes
 * - Reloadable field changes succeed
 * - Field descriptor table consistency
 *
 * SPDX-License-Identifier: MIT
 */

/* ASX_CHECKPOINT_WAIVER_FILE() -- config reload test, no checkpoint coverage needed */

#include "test_harness.h"
#include <asx/asx.h>
#include <asx/runtime/config_reload.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static asx_config_state g_state;
static asx_runtime_config g_cfg;

static void cr_setup(void) {
    asx_config_state_init(&g_state);
    asx_runtime_config_init(&g_cfg);
}

/* ------------------------------------------------------------------ */
/* Test: field classification                                          */
/* ------------------------------------------------------------------ */

TEST(field_class_size_is_frozen) {
    ASSERT_EQ(asx_config_field_class("size"), ASX_CONFIG_FROZEN_INIT);
}

TEST(field_class_wait_policy_is_reloadable) {
    ASSERT_EQ(asx_config_field_class("wait_policy"), ASX_CONFIG_RELOADABLE);
}

TEST(field_class_leak_response_is_reloadable) {
    ASSERT_EQ(asx_config_field_class("leak_response"), ASX_CONFIG_RELOADABLE);
}

TEST(field_class_finalizer_poll_budget_is_reloadable) {
    ASSERT_EQ(asx_config_field_class("finalizer_poll_budget"), ASX_CONFIG_RELOADABLE);
}

TEST(field_class_finalizer_time_budget_is_reloadable) {
    ASSERT_EQ(asx_config_field_class("finalizer_time_budget_ns"), ASX_CONFIG_RELOADABLE);
}

TEST(field_class_finalizer_escalation_is_reloadable) {
    ASSERT_EQ(asx_config_field_class("finalizer_escalation"), ASX_CONFIG_RELOADABLE);
}

TEST(field_class_cancel_chain_depth_is_restart_required) {
    ASSERT_EQ(asx_config_field_class("max_cancel_chain_depth"), ASX_CONFIG_RESTART_REQUIRED);
}

TEST(field_class_cancel_chain_memory_is_restart_required) {
    ASSERT_EQ(asx_config_field_class("max_cancel_chain_memory"), ASX_CONFIG_RESTART_REQUIRED);
}

TEST(field_class_unknown_is_frozen) {
    ASSERT_EQ(asx_config_field_class("nonexistent_field"), ASX_CONFIG_FROZEN_COMPILE);
}

/* ------------------------------------------------------------------ */
/* Test: initial load                                                  */
/* ------------------------------------------------------------------ */

TEST(load_succeeds) {
    cr_setup();
    ASSERT_EQ(asx_config_load(&g_state, &g_cfg), ASX_OK);
    ASSERT_TRUE(asx_config_active(&g_state) != NULL);
}

TEST(load_null_state_fails) {
    cr_setup();
    ASSERT_EQ(asx_config_load(NULL, &g_cfg), ASX_E_INVALID_ARGUMENT);
}

TEST(load_null_cfg_fails) {
    cr_setup();
    ASSERT_EQ(asx_config_load(&g_state, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(load_invalid_cfg_rejected) {
    asx_runtime_config bad_cfg;

    cr_setup();
    memcpy(&bad_cfg, &g_cfg, sizeof(bad_cfg));
    bad_cfg.size = 0u;
    ASSERT_EQ(asx_config_load(&g_state, &bad_cfg), ASX_E_INVALID_ARGUMENT);
    ASSERT_TRUE(asx_config_active(&g_state) == NULL);
}

/* ------------------------------------------------------------------ */
/* Test: reload reloadable fields succeeds                             */
/* ------------------------------------------------------------------ */

TEST(reload_wait_policy_succeeds) {
    asx_runtime_config new_cfg;

    cr_setup();
    ASSERT_EQ(asx_config_load(&g_state, &g_cfg), ASX_OK);

    memcpy(&new_cfg, &g_cfg, sizeof(new_cfg));
    new_cfg.wait_policy = ASX_WAIT_BUSY_SPIN;

    ASSERT_EQ(asx_config_reload(&g_state, &new_cfg), ASX_OK);
    ASSERT_EQ(asx_config_active(&g_state)->wait_policy, ASX_WAIT_BUSY_SPIN);
}

TEST(reload_leak_response_succeeds) {
    asx_runtime_config new_cfg;

    cr_setup();
    ASSERT_EQ(asx_config_load(&g_state, &g_cfg), ASX_OK);

    memcpy(&new_cfg, &g_cfg, sizeof(new_cfg));
    new_cfg.leak_response = ASX_LEAK_PANIC;

    ASSERT_EQ(asx_config_reload(&g_state, &new_cfg), ASX_OK);
    ASSERT_EQ(asx_config_active(&g_state)->leak_response, ASX_LEAK_PANIC);
}

TEST(reload_finalizer_budget_succeeds) {
    asx_runtime_config new_cfg;

    cr_setup();
    ASSERT_EQ(asx_config_load(&g_state, &g_cfg), ASX_OK);

    memcpy(&new_cfg, &g_cfg, sizeof(new_cfg));
    new_cfg.finalizer_poll_budget = 500;
    new_cfg.finalizer_time_budget_ns = 10000000000ULL; /* 10s */

    ASSERT_EQ(asx_config_reload(&g_state, &new_cfg), ASX_OK);
    ASSERT_EQ(asx_config_active(&g_state)->finalizer_poll_budget, (uint32_t)500);
}

/* ------------------------------------------------------------------ */
/* Test: reload frozen fields rejected                                 */
/* ------------------------------------------------------------------ */

TEST(reload_size_field_rejected) {
    asx_runtime_config new_cfg;
    const char *rejected = NULL;

    cr_setup();
    ASSERT_EQ(asx_config_load(&g_state, &g_cfg), ASX_OK);

    memcpy(&new_cfg, &g_cfg, sizeof(new_cfg));
    new_cfg.size = 999; /* Tamper with frozen size field */

    ASSERT_EQ(asx_config_reload(&g_state, &new_cfg), ASX_E_CONFIG_FROZEN);

    /* Validate also produces rejection info */
    ASSERT_EQ(asx_config_validate_reload(&g_state, &new_cfg, &rejected), ASX_E_CONFIG_FROZEN);
    ASSERT_TRUE(rejected != NULL);
    ASSERT_STR_EQ(rejected, "size");
}

/* ------------------------------------------------------------------ */
/* Test: reload restart-required fields rejected                       */
/* ------------------------------------------------------------------ */

TEST(reload_cancel_depth_rejected) {
    asx_runtime_config new_cfg;
    const char *rejected = NULL;

    cr_setup();
    ASSERT_EQ(asx_config_load(&g_state, &g_cfg), ASX_OK);

    memcpy(&new_cfg, &g_cfg, sizeof(new_cfg));
    new_cfg.max_cancel_chain_depth = 32; /* Change restart-required field */

    ASSERT_EQ(asx_config_reload(&g_state, &new_cfg), ASX_E_CONFIG_RESTART_REQ);

    ASSERT_EQ(asx_config_validate_reload(&g_state, &new_cfg, &rejected), ASX_E_CONFIG_RESTART_REQ);
    ASSERT_TRUE(rejected != NULL);
    ASSERT_STR_EQ(rejected, "max_cancel_chain_depth");
}

TEST(reload_cancel_memory_rejected) {
    asx_runtime_config new_cfg;
    const char *rejected = NULL;

    cr_setup();
    ASSERT_EQ(asx_config_load(&g_state, &g_cfg), ASX_OK);

    memcpy(&new_cfg, &g_cfg, sizeof(new_cfg));
    new_cfg.max_cancel_chain_memory = 8192;

    ASSERT_EQ(asx_config_reload(&g_state, &new_cfg), ASX_E_CONFIG_RESTART_REQ);

    ASSERT_EQ(asx_config_validate_reload(&g_state, &new_cfg, &rejected), ASX_E_CONFIG_RESTART_REQ);
    ASSERT_STR_EQ(rejected, "max_cancel_chain_memory");
}

/* ------------------------------------------------------------------ */
/* Test: rollback on invalid update                                    */
/* ------------------------------------------------------------------ */

TEST(rollback_preserves_active_config) {
    asx_runtime_config new_cfg;
    asx_wait_policy original_policy;

    cr_setup();
    ASSERT_EQ(asx_config_load(&g_state, &g_cfg), ASX_OK);

    original_policy = asx_config_active(&g_state)->wait_policy;

    /* Try to change both a reloadable and a frozen field */
    memcpy(&new_cfg, &g_cfg, sizeof(new_cfg));
    new_cfg.wait_policy = ASX_WAIT_BUSY_SPIN; /* Reloadable */
    new_cfg.size = 42;                        /* Frozen — should reject */

    ASSERT_EQ(asx_config_reload(&g_state, &new_cfg), ASX_E_CONFIG_FROZEN);

    /* Active config must be unchanged (rollback) */
    ASSERT_EQ(asx_config_active(&g_state)->wait_policy, original_policy);
}

/* ------------------------------------------------------------------ */
/* Test: reload before load fails                                      */
/* ------------------------------------------------------------------ */

TEST(reload_before_load_fails) {
    asx_runtime_config new_cfg;

    cr_setup();
    asx_runtime_config_init(&new_cfg);

    ASSERT_EQ(asx_config_reload(&g_state, &new_cfg), ASX_E_INVALID_STATE);
}

/* ------------------------------------------------------------------ */
/* Test: validate before load fails                                    */
/* ------------------------------------------------------------------ */

TEST(validate_before_load_fails) {
    asx_runtime_config new_cfg;

    cr_setup();
    asx_runtime_config_init(&new_cfg);

    ASSERT_EQ(asx_config_validate_reload(&g_state, &new_cfg, NULL), ASX_E_INVALID_STATE);
}

TEST(validate_rejects_structurally_invalid_proposed_config) {
    asx_runtime_config bad_cfg;

    cr_setup();
    ASSERT_EQ(asx_config_load(&g_state, &g_cfg), ASX_OK);

    memcpy(&bad_cfg, &g_cfg, sizeof(bad_cfg));
    bad_cfg.finalizer_poll_budget = 0u;
    ASSERT_EQ(asx_config_validate_reload(&g_state, &bad_cfg, NULL), ASX_E_INVALID_ARGUMENT);
}

/* ------------------------------------------------------------------ */
/* Test: identical config reload succeeds                              */
/* ------------------------------------------------------------------ */

TEST(identical_reload_succeeds) {
    cr_setup();
    ASSERT_EQ(asx_config_load(&g_state, &g_cfg), ASX_OK);

    /* Reload with identical config — should always succeed */
    ASSERT_EQ(asx_config_reload(&g_state, &g_cfg), ASX_OK);
}

/* ------------------------------------------------------------------ */
/* Test: field table consistency                                       */
/* ------------------------------------------------------------------ */

TEST(field_table_has_all_config_fields) {
    uint32_t count = 0;
    const asx_config_field_desc *table = asx_config_field_table(&count);

    ASSERT_TRUE(table != NULL);
    ASSERT_EQ(count, (uint32_t)9); /* 9 fields in asx_runtime_config */
}

TEST(field_table_covers_struct_range) {
    uint32_t count = 0;
    uint32_t i;
    const asx_config_field_desc *table = asx_config_field_table(&count);

    /* Every field should have valid offset and size */
    for (i = 0; i < count; i++) {
        ASSERT_TRUE(table[i].name != NULL);
        ASSERT_TRUE(table[i].field_size > 0);
        ASSERT_TRUE(table[i].offset + table[i].field_size <= sizeof(asx_runtime_config));
    }
}

/* ------------------------------------------------------------------ */
/* Test: null safety                                                   */
/* ------------------------------------------------------------------ */

TEST(null_safety) {
    asx_config_state_init(NULL); /* should not crash */
    ASSERT_EQ(asx_config_load(NULL, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_config_reload(NULL, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_TRUE(asx_config_active(NULL) == NULL);
    ASSERT_EQ(asx_config_field_class(NULL), ASX_CONFIG_FROZEN_COMPILE);
}

/* ------------------------------------------------------------------ */
/* Test: multiple sequential reloads                                   */
/* ------------------------------------------------------------------ */

TEST(sequential_reloads) {
    asx_runtime_config new_cfg;

    cr_setup();
    ASSERT_EQ(asx_config_load(&g_state, &g_cfg), ASX_OK);

    /* Reload 1: change wait_policy */
    memcpy(&new_cfg, &g_cfg, sizeof(new_cfg));
    new_cfg.wait_policy = ASX_WAIT_BUSY_SPIN;
    ASSERT_EQ(asx_config_reload(&g_state, &new_cfg), ASX_OK);
    ASSERT_EQ(asx_config_active(&g_state)->wait_policy, ASX_WAIT_BUSY_SPIN);

    /* Reload 2: change finalizer budget (from reloaded state) */
    memcpy(&new_cfg, asx_config_active(&g_state), sizeof(new_cfg));
    new_cfg.finalizer_poll_budget = 200;
    ASSERT_EQ(asx_config_reload(&g_state, &new_cfg), ASX_OK);
    ASSERT_EQ(asx_config_active(&g_state)->finalizer_poll_budget, (uint32_t)200);
    ASSERT_EQ(asx_config_active(&g_state)->wait_policy, ASX_WAIT_BUSY_SPIN);

    /* Reload 3: change leak_response */
    memcpy(&new_cfg, asx_config_active(&g_state), sizeof(new_cfg));
    new_cfg.leak_response = ASX_LEAK_SILENT;
    ASSERT_EQ(asx_config_reload(&g_state, &new_cfg), ASX_OK);
    ASSERT_EQ(asx_config_active(&g_state)->leak_response, ASX_LEAK_SILENT);
    ASSERT_EQ(asx_config_active(&g_state)->wait_policy, ASX_WAIT_BUSY_SPIN);
    ASSERT_EQ(asx_config_active(&g_state)->finalizer_poll_budget, (uint32_t)200);
}

/* ------------------------------------------------------------------ */
/* Test: error code string coverage                                    */
/* ------------------------------------------------------------------ */

TEST(error_code_strings) {
    ASSERT_STR_EQ(asx_status_str(ASX_E_CONFIG_FROZEN), "config field is frozen");
    ASSERT_STR_EQ(asx_status_str(ASX_E_CONFIG_RESTART_REQ), "config field requires restart");
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    fprintf(stderr, "=== config reload tests (bd-3vt.9) ===\n");

    /* Field classification */
    RUN_TEST(field_class_size_is_frozen);
    RUN_TEST(field_class_wait_policy_is_reloadable);
    RUN_TEST(field_class_leak_response_is_reloadable);
    RUN_TEST(field_class_finalizer_poll_budget_is_reloadable);
    RUN_TEST(field_class_finalizer_time_budget_is_reloadable);
    RUN_TEST(field_class_finalizer_escalation_is_reloadable);
    RUN_TEST(field_class_cancel_chain_depth_is_restart_required);
    RUN_TEST(field_class_cancel_chain_memory_is_restart_required);
    RUN_TEST(field_class_unknown_is_frozen);

    /* Load */
    RUN_TEST(load_succeeds);
    RUN_TEST(load_null_state_fails);
    RUN_TEST(load_null_cfg_fails);
    RUN_TEST(load_invalid_cfg_rejected);

    /* Reload success */
    RUN_TEST(reload_wait_policy_succeeds);
    RUN_TEST(reload_leak_response_succeeds);
    RUN_TEST(reload_finalizer_budget_succeeds);

    /* Reload rejection */
    RUN_TEST(reload_size_field_rejected);
    RUN_TEST(reload_cancel_depth_rejected);
    RUN_TEST(reload_cancel_memory_rejected);

    /* Rollback */
    RUN_TEST(rollback_preserves_active_config);

    /* Edge cases */
    RUN_TEST(reload_before_load_fails);
    RUN_TEST(validate_before_load_fails);
    RUN_TEST(validate_rejects_structurally_invalid_proposed_config);
    RUN_TEST(identical_reload_succeeds);

    /* Field table */
    RUN_TEST(field_table_has_all_config_fields);
    RUN_TEST(field_table_covers_struct_range);

    /* Safety and sequences */
    RUN_TEST(null_safety);
    RUN_TEST(sequential_reloads);
    RUN_TEST(error_code_strings);

    TEST_REPORT();
    return test_failures;
}
