/*
 * test_overload_catalog.c — per-profile overload policy catalog tests (bd-j4m.8)
 *
 * Validates catalog completeness, structural consistency, per-profile
 * policy correctness, decision consistency, and fixture traceability.
 *
 * SPDX-License-Identifier: MIT
 */

#include "test_log.h"
#include "test_harness.h"
#include <asx/runtime/overload_catalog.h>
#include <asx/runtime/hft_instrument.h>
#include <asx/runtime/profile_compat.h>
#include <string.h>

/* ===================================================================
 * Catalog structure tests
 * =================================================================== */

TEST(catalog_version_nonzero)
{
    ASSERT_TRUE(asx_overload_catalog_version() > 0);
}

TEST(catalog_count_matches_profiles)
{
    ASSERT_EQ(asx_overload_catalog_count(), (uint32_t)ASX_PROFILE_ID_COUNT);
}

TEST(catalog_every_profile_has_entry)
{
    int i;
    for (i = 0; i < ASX_PROFILE_ID_COUNT; i++) {
        const asx_overload_catalog_entry *entry = NULL;
        asx_status s = asx_overload_catalog_get((asx_profile_id)i, &entry);
        ASSERT_EQ(s, ASX_OK);
        ASSERT_TRUE(entry != NULL);
        ASSERT_EQ((int)entry->profile, i);
    }
}

TEST(catalog_invalid_profile_rejected)
{
    const asx_overload_catalog_entry *entry = NULL;
    asx_status s = asx_overload_catalog_get((asx_profile_id)99, &entry);
    ASSERT_EQ(s, ASX_E_INVALID_ARGUMENT);
}

TEST(catalog_null_out_rejected)
{
    asx_status s = asx_overload_catalog_get(ASX_PROFILE_ID_CORE, NULL);
    ASSERT_EQ(s, ASX_E_INVALID_ARGUMENT);
}

TEST(catalog_full_validation_passes)
{
    asx_catalog_validation_result result;
    asx_overload_catalog_validate(&result);
    if (!result.valid) {
        fprintf(stderr, "    validation failure: %s\n", result.first_violation);
    }
    ASSERT_TRUE(result.valid);
    ASSERT_EQ(result.violation_count, 0u);
}

TEST(catalog_every_entry_structurally_valid)
{
    int i;
    for (i = 0; i < ASX_PROFILE_ID_COUNT; i++) {
        const asx_overload_catalog_entry *entry = NULL;
        asx_overload_catalog_get((asx_profile_id)i, &entry);
        ASSERT_TRUE(asx_overload_catalog_entry_valid(entry));
    }
}

TEST(catalog_null_entry_invalid)
{
    ASSERT_TRUE(!asx_overload_catalog_entry_valid(NULL));
}

/* ===================================================================
 * Per-profile policy tests
 * =================================================================== */

TEST(core_profile_rejects_at_90)
{
    const asx_overload_catalog_entry *entry = NULL;
    asx_overload_catalog_get(ASX_PROFILE_ID_CORE, &entry);
    ASSERT_EQ((int)entry->mode, (int)ASX_OVERLOAD_REJECT);
    ASSERT_EQ(entry->threshold_pct, 90u);
    ASSERT_EQ(entry->shed_max, 0u);
    ASSERT_EQ((int)entry->degrade, (int)ASX_DEGRADE_NONE);
}

TEST(hft_profile_sheds_oldest)
{
    const asx_overload_catalog_entry *entry = NULL;
    asx_overload_catalog_get(ASX_PROFILE_ID_HFT, &entry);
    ASSERT_EQ((int)entry->mode, (int)ASX_OVERLOAD_SHED_OLDEST);
    ASSERT_TRUE(entry->threshold_pct <= 90u);
    ASSERT_TRUE(entry->shed_max > 0u);
    ASSERT_EQ((int)entry->degrade, (int)ASX_DEGRADE_SHED_TAIL);
}

TEST(automotive_profile_backpressure)
{
    const asx_overload_catalog_entry *entry = NULL;
    asx_overload_catalog_get(ASX_PROFILE_ID_AUTOMOTIVE, &entry);
    ASSERT_EQ((int)entry->mode, (int)ASX_OVERLOAD_BACKPRESSURE);
    ASSERT_EQ(entry->shed_max, 0u);
    ASSERT_EQ((int)entry->degrade, (int)ASX_DEGRADE_WATCHDOG_TRIP);
}

TEST(embedded_router_rejects_aggressively)
{
    const asx_overload_catalog_entry *entry = NULL;
    asx_overload_catalog_get(ASX_PROFILE_ID_EMBEDDED_ROUTER, &entry);
    ASSERT_EQ((int)entry->mode, (int)ASX_OVERLOAD_REJECT);
    ASSERT_TRUE(entry->threshold_pct < 90u);
}

TEST(freestanding_rejects_earlier_than_core)
{
    const asx_overload_catalog_entry *core = NULL;
    const asx_overload_catalog_entry *free_entry = NULL;
    asx_overload_catalog_get(ASX_PROFILE_ID_CORE, &core);
    asx_overload_catalog_get(ASX_PROFILE_ID_FREESTANDING, &free_entry);
    ASSERT_TRUE(free_entry->threshold_pct < core->threshold_pct);
}

TEST(parallel_inherits_core_policy)
{
    const asx_overload_catalog_entry *core = NULL;
    const asx_overload_catalog_entry *par = NULL;
    asx_overload_catalog_get(ASX_PROFILE_ID_CORE, &core);
    asx_overload_catalog_get(ASX_PROFILE_ID_PARALLEL, &par);
    ASSERT_EQ((int)par->mode, (int)core->mode);
    ASSERT_EQ(par->threshold_pct, core->threshold_pct);
}

/* ===================================================================
 * Forbidden behavior tests
 * =================================================================== */

TEST(all_profiles_forbid_nondeterministic)
{
    int i;
    for (i = 0; i < ASX_PROFILE_ID_COUNT; i++) {
        const asx_overload_catalog_entry *entry = NULL;
        asx_overload_catalog_get((asx_profile_id)i, &entry);
        ASSERT_TRUE(entry->forbidden & ASX_FORBID_NONDETERMINISTIC);
    }
}

TEST(automotive_forbids_deadline_miss)
{
    const asx_overload_catalog_entry *entry = NULL;
    asx_overload_catalog_get(ASX_PROFILE_ID_AUTOMOTIVE, &entry);
    ASSERT_TRUE(entry->forbidden & ASX_FORBID_DEADLINE_MISS);
}

TEST(hft_forbids_latency_spike)
{
    const asx_overload_catalog_entry *entry = NULL;
    asx_overload_catalog_get(ASX_PROFILE_ID_HFT, &entry);
    ASSERT_TRUE(entry->forbidden & ASX_FORBID_LATENCY_SPIKE);
}

TEST(embedded_forbids_unbounded_queue)
{
    const asx_overload_catalog_entry *entry = NULL;
    asx_overload_catalog_get(ASX_PROFILE_ID_EMBEDDED_ROUTER, &entry);
    ASSERT_TRUE(entry->forbidden & ASX_FORBID_UNBOUNDED_QUEUE);
}

/* ===================================================================
 * to_policy conversion tests
 * =================================================================== */

TEST(to_policy_core_matches_catalog)
{
    asx_overload_policy pol;
    const asx_overload_catalog_entry *entry = NULL;
    asx_overload_catalog_get(ASX_PROFILE_ID_CORE, &entry);
    asx_overload_catalog_to_policy(ASX_PROFILE_ID_CORE, &pol);
    ASSERT_EQ((int)pol.mode, (int)entry->mode);
    ASSERT_EQ(pol.threshold_pct, entry->threshold_pct);
    ASSERT_EQ(pol.shed_max, entry->shed_max);
}

TEST(to_policy_hft_matches_catalog)
{
    asx_overload_policy pol;
    const asx_overload_catalog_entry *entry = NULL;
    asx_overload_catalog_get(ASX_PROFILE_ID_HFT, &entry);
    asx_overload_catalog_to_policy(ASX_PROFILE_ID_HFT, &pol);
    ASSERT_EQ((int)pol.mode, (int)entry->mode);
    ASSERT_EQ(pol.threshold_pct, entry->threshold_pct);
    ASSERT_EQ(pol.shed_max, entry->shed_max);
}

TEST(to_policy_null_rejected)
{
    asx_status s = asx_overload_catalog_to_policy(ASX_PROFILE_ID_CORE, NULL);
    ASSERT_EQ(s, ASX_E_INVALID_ARGUMENT);
}

TEST(to_policy_invalid_profile_rejected)
{
    asx_overload_policy pol;
    asx_status s = asx_overload_catalog_to_policy((asx_profile_id)99, &pol);
    ASSERT_EQ(s, ASX_E_INVALID_ARGUMENT);
}

/* ===================================================================
 * Decision consistency tests
 * =================================================================== */

TEST(decision_consistent_reject_below_threshold)
{
    const asx_overload_catalog_entry *entry = NULL;
    asx_overload_policy pol;
    asx_overload_decision dec;

    asx_overload_catalog_get(ASX_PROFILE_ID_CORE, &entry);
    asx_overload_catalog_to_policy(ASX_PROFILE_ID_CORE, &pol);
    asx_overload_evaluate(&pol, 50, 100, &dec);

    ASSERT_TRUE(asx_overload_catalog_decision_consistent(entry, &dec));
}

TEST(decision_consistent_reject_above_threshold)
{
    const asx_overload_catalog_entry *entry = NULL;
    asx_overload_policy pol;
    asx_overload_decision dec;

    asx_overload_catalog_get(ASX_PROFILE_ID_CORE, &entry);
    asx_overload_catalog_to_policy(ASX_PROFILE_ID_CORE, &pol);
    asx_overload_evaluate(&pol, 95, 100, &dec);

    ASSERT_TRUE(dec.triggered);
    ASSERT_EQ(dec.admit_status, ASX_E_ADMISSION_CLOSED);
    ASSERT_TRUE(asx_overload_catalog_decision_consistent(entry, &dec));
}

TEST(decision_consistent_shed_oldest)
{
    const asx_overload_catalog_entry *entry = NULL;
    asx_overload_policy pol;
    asx_overload_decision dec;

    asx_overload_catalog_get(ASX_PROFILE_ID_HFT, &entry);
    asx_overload_catalog_to_policy(ASX_PROFILE_ID_HFT, &pol);
    asx_overload_evaluate(&pol, 60, 64, &dec);

    ASSERT_TRUE(dec.triggered);
    ASSERT_TRUE(dec.shed_count > 0);
    ASSERT_TRUE(asx_overload_catalog_decision_consistent(entry, &dec));
}

TEST(decision_consistent_backpressure)
{
    const asx_overload_catalog_entry *entry = NULL;
    asx_overload_policy pol;
    asx_overload_decision dec;

    asx_overload_catalog_get(ASX_PROFILE_ID_AUTOMOTIVE, &entry);
    asx_overload_catalog_to_policy(ASX_PROFILE_ID_AUTOMOTIVE, &pol);
    asx_overload_evaluate(&pol, 95, 100, &dec);

    ASSERT_TRUE(dec.triggered);
    ASSERT_EQ(dec.admit_status, ASX_E_WOULD_BLOCK);
    ASSERT_TRUE(asx_overload_catalog_decision_consistent(entry, &dec));
}

TEST(decision_inconsistent_wrong_mode)
{
    const asx_overload_catalog_entry *entry = NULL;
    asx_overload_decision dec;

    asx_overload_catalog_get(ASX_PROFILE_ID_CORE, &entry);

    memset(&dec, 0, sizeof(dec));
    dec.triggered = 1;
    dec.mode = ASX_OVERLOAD_BACKPRESSURE;
    dec.load_pct = 95;
    dec.shed_count = 0;
    dec.admit_status = ASX_E_WOULD_BLOCK;

    ASSERT_TRUE(!asx_overload_catalog_decision_consistent(entry, &dec));
}

TEST(decision_null_safety)
{
    asx_overload_decision dec;
    memset(&dec, 0, sizeof(dec));
    dec.mode = ASX_OVERLOAD_REJECT;
    dec.admit_status = ASX_OK;

    ASSERT_TRUE(!asx_overload_catalog_decision_consistent(NULL, &dec));
    ASSERT_TRUE(!asx_overload_catalog_decision_consistent(NULL, NULL));
}

/* ===================================================================
 * Fixture traceability tests
 * =================================================================== */

TEST(every_entry_has_fixtures)
{
    int i;
    for (i = 0; i < ASX_PROFILE_ID_COUNT; i++) {
        const asx_overload_catalog_entry *entry = NULL;
        asx_overload_catalog_get((asx_profile_id)i, &entry);
        ASSERT_TRUE(entry->fixtures.fixture_count >= 1u);
        ASSERT_TRUE(entry->fixtures.fixture_count <= ASX_CATALOG_MAX_FIXTURES);
        ASSERT_TRUE(entry->fixtures.parity_gate != NULL);
    }
}

TEST(every_entry_has_rationale)
{
    int i;
    for (i = 0; i < ASX_PROFILE_ID_COUNT; i++) {
        const asx_overload_catalog_entry *entry = NULL;
        asx_overload_catalog_get((asx_profile_id)i, &entry);
        ASSERT_TRUE(entry->rationale != NULL);
        ASSERT_TRUE(entry->rationale[0] != '\0');
    }
}

TEST(hft_fixtures_include_microburst)
{
    const asx_overload_catalog_entry *entry = NULL;
    uint32_t i;
    int found = 0;

    asx_overload_catalog_get(ASX_PROFILE_ID_HFT, &entry);
    for (i = 0; i < entry->fixtures.fixture_count; i++) {
        if (entry->fixtures.fixture_ids[i] &&
            strstr(entry->fixtures.fixture_ids[i], "microburst")) {
            found = 1;
        }
    }
    ASSERT_TRUE(found);
}

TEST(automotive_fixtures_include_watchdog)
{
    const asx_overload_catalog_entry *entry = NULL;
    uint32_t i;
    int found = 0;

    asx_overload_catalog_get(ASX_PROFILE_ID_AUTOMOTIVE, &entry);
    for (i = 0; i < entry->fixtures.fixture_count; i++) {
        if (entry->fixtures.fixture_ids[i] &&
            strstr(entry->fixtures.fixture_ids[i], "watchdog")) {
            found = 1;
        }
    }
    ASSERT_TRUE(found);
}

/* ===================================================================
 * String helper tests
 * =================================================================== */

TEST(degrade_class_str_all_variants)
{
    ASSERT_TRUE(strcmp(asx_degrade_class_str(ASX_DEGRADE_NONE), "NONE") == 0);
    ASSERT_TRUE(strcmp(asx_degrade_class_str(ASX_DEGRADE_SHED_TAIL), "SHED_TAIL") == 0);
    ASSERT_TRUE(strcmp(asx_degrade_class_str(ASX_DEGRADE_BACKPRESSURE), "BACKPRESSURE") == 0);
    ASSERT_TRUE(strcmp(asx_degrade_class_str(ASX_DEGRADE_WATCHDOG_TRIP), "WATCHDOG_TRIP") == 0);
    ASSERT_TRUE(strcmp(asx_degrade_class_str((asx_degrade_class)99), "UNKNOWN") == 0);
}

/* ===================================================================
 * Deterministic replay test
 * =================================================================== */

TEST(overload_decisions_are_deterministic)
{
    int i;
    for (i = 0; i < ASX_PROFILE_ID_COUNT; i++) {
        asx_overload_policy pol;
        asx_overload_decision dec1, dec2;
        uint32_t used = 55;
        uint32_t capacity = 64;

        asx_overload_catalog_to_policy((asx_profile_id)i, &pol);
        asx_overload_evaluate(&pol, used, capacity, &dec1);
        asx_overload_evaluate(&pol, used, capacity, &dec2);

        ASSERT_EQ(dec1.triggered, dec2.triggered);
        ASSERT_EQ((int)dec1.mode, (int)dec2.mode);
        ASSERT_EQ(dec1.load_pct, dec2.load_pct);
        ASSERT_EQ(dec1.shed_count, dec2.shed_count);
        ASSERT_EQ(dec1.admit_status, dec2.admit_status);
    }
}

/* ===================================================================
 * main
 * =================================================================== */

int main(void)
{
    test_log_open("unit", "runtime/overload_catalog",
                  "overload-catalog");

    /* Catalog structure */
    RUN_TEST(catalog_version_nonzero);
    RUN_TEST(catalog_count_matches_profiles);
    RUN_TEST(catalog_every_profile_has_entry);
    RUN_TEST(catalog_invalid_profile_rejected);
    RUN_TEST(catalog_null_out_rejected);
    RUN_TEST(catalog_full_validation_passes);
    RUN_TEST(catalog_every_entry_structurally_valid);
    RUN_TEST(catalog_null_entry_invalid);

    /* Per-profile policies */
    RUN_TEST(core_profile_rejects_at_90);
    RUN_TEST(hft_profile_sheds_oldest);
    RUN_TEST(automotive_profile_backpressure);
    RUN_TEST(embedded_router_rejects_aggressively);
    RUN_TEST(freestanding_rejects_earlier_than_core);
    RUN_TEST(parallel_inherits_core_policy);

    /* Forbidden behaviors */
    RUN_TEST(all_profiles_forbid_nondeterministic);
    RUN_TEST(automotive_forbids_deadline_miss);
    RUN_TEST(hft_forbids_latency_spike);
    RUN_TEST(embedded_forbids_unbounded_queue);

    /* to_policy conversion */
    RUN_TEST(to_policy_core_matches_catalog);
    RUN_TEST(to_policy_hft_matches_catalog);
    RUN_TEST(to_policy_null_rejected);
    RUN_TEST(to_policy_invalid_profile_rejected);

    /* Decision consistency */
    RUN_TEST(decision_consistent_reject_below_threshold);
    RUN_TEST(decision_consistent_reject_above_threshold);
    RUN_TEST(decision_consistent_shed_oldest);
    RUN_TEST(decision_consistent_backpressure);
    RUN_TEST(decision_inconsistent_wrong_mode);
    RUN_TEST(decision_null_safety);

    /* Fixture traceability */
    RUN_TEST(every_entry_has_fixtures);
    RUN_TEST(every_entry_has_rationale);
    RUN_TEST(hft_fixtures_include_microburst);
    RUN_TEST(automotive_fixtures_include_watchdog);

    /* String helpers */
    RUN_TEST(degrade_class_str_all_variants);

    /* Deterministic replay */
    RUN_TEST(overload_decisions_are_deterministic);

    TEST_REPORT();
    test_log_close();
    return test_failures;
}
