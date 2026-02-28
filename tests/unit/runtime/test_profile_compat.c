/*
 * test_profile_compat.c — tests for profile compatibility baseline (bd-j4m.1)
 *
 * Verifies:
 *   - Profile identity queries
 *   - Operational descriptor correctness
 *   - Property classification (all operational)
 *   - Semantic rule enforcement (all enforced)
 *   - Digest-based parity comparison
 *   - Self-parity (current build digest matches itself)
 *
 * SPDX-License-Identifier: MIT
 */

#include "test_harness.h"
#include <asx/runtime/profile_compat.h>
#include <asx/runtime/telemetry.h>
#include <asx/runtime/runtime.h>
#include <asx/time/timer_wheel.h>
#include <string.h>

/* -------------------------------------------------------------------
 * Profile identity tests
 * ------------------------------------------------------------------- */

TEST(profile_active_is_core)
{
    /* Default build uses ASX_PROFILE_CORE */
    asx_profile_id id = asx_profile_active();
    ASSERT_EQ((int)id, (int)ASX_PROFILE_ID_CORE);
}

TEST(profile_name_all_valid)
{
    int i;
    for (i = 0; i < (int)ASX_PROFILE_ID_COUNT; i++) {
        const char *name = asx_profile_name((asx_profile_id)i);
        ASSERT_TRUE(name != NULL);
        ASSERT_TRUE(strlen(name) > 0);
    }
}

TEST(profile_name_core)
{
    const char *name = asx_profile_name(ASX_PROFILE_ID_CORE);
    ASSERT_STR_EQ(name, "CORE");
}

TEST(profile_name_hft)
{
    const char *name = asx_profile_name(ASX_PROFILE_ID_HFT);
    ASSERT_STR_EQ(name, "HFT");
}

TEST(profile_name_automotive)
{
    const char *name = asx_profile_name(ASX_PROFILE_ID_AUTOMOTIVE);
    ASSERT_STR_EQ(name, "AUTOMOTIVE");
}

TEST(profile_name_out_of_range)
{
    const char *name = asx_profile_name((asx_profile_id)99);
    ASSERT_STR_EQ(name, "UNKNOWN");
}

/* -------------------------------------------------------------------
 * Profile descriptor tests
 * ------------------------------------------------------------------- */

TEST(descriptor_null_returns_error)
{
    asx_status s = asx_profile_get_descriptor(ASX_PROFILE_ID_CORE, NULL);
    ASSERT_EQ((int)s, (int)ASX_E_INVALID_ARGUMENT);
}

TEST(descriptor_invalid_id_returns_error)
{
    asx_profile_descriptor desc;
    asx_status s = asx_profile_get_descriptor((asx_profile_id)99, &desc);
    ASSERT_EQ((int)s, (int)ASX_E_INVALID_ARGUMENT);
}

TEST(descriptor_core_values)
{
    asx_profile_descriptor desc;
    asx_status s = asx_profile_get_descriptor(ASX_PROFILE_ID_CORE, &desc);
    ASSERT_EQ((int)s, (int)ASX_OK);
    ASSERT_EQ((int)desc.id, (int)ASX_PROFILE_ID_CORE);
    ASSERT_STR_EQ(desc.name, "CORE");
    ASSERT_EQ((int)desc.default_wait, (int)ASX_WAIT_YIELD);
    ASSERT_TRUE(desc.max_regions > 0);
    ASSERT_TRUE(desc.max_tasks > 0);
    ASSERT_TRUE(desc.max_obligations > 0);
    ASSERT_TRUE(desc.max_timers > 0);
}

TEST(descriptor_hft_busy_spin)
{
    asx_profile_descriptor desc;
    asx_status s = asx_profile_get_descriptor(ASX_PROFILE_ID_HFT, &desc);
    ASSERT_EQ((int)s, (int)ASX_OK);
    ASSERT_EQ((int)desc.default_wait, (int)ASX_WAIT_BUSY_SPIN);
    /* HFT disables ghost monitors for latency */
    ASSERT_EQ(desc.ghost_monitors, 0);
}

TEST(descriptor_automotive_sleep_wait)
{
    asx_profile_descriptor desc;
    asx_status s = asx_profile_get_descriptor(ASX_PROFILE_ID_AUTOMOTIVE, &desc);
    ASSERT_EQ((int)s, (int)ASX_OK);
    ASSERT_EQ((int)desc.default_wait, (int)ASX_WAIT_SLEEP);
}

TEST(descriptor_freestanding_defaults)
{
    asx_profile_descriptor desc;
    asx_status s = asx_profile_get_descriptor(ASX_PROFILE_ID_FREESTANDING, &desc);
    ASSERT_EQ((int)s, (int)ASX_OK);
    ASSERT_EQ((int)desc.default_wait, (int)ASX_WAIT_YIELD);
    ASSERT_EQ(desc.allocator_sealable, 1);
}

TEST(descriptor_embedded_router_defaults)
{
    asx_profile_descriptor desc;
    asx_status s = asx_profile_get_descriptor(ASX_PROFILE_ID_EMBEDDED_ROUTER, &desc);
    ASSERT_EQ((int)s, (int)ASX_OK);
    ASSERT_EQ((int)desc.default_wait, (int)ASX_WAIT_BUSY_SPIN);
    ASSERT_EQ(desc.allocator_sealable, 1);
}

TEST(descriptor_limits_align_runtime_capacities)
{
    int i;
    for (i = 0; i < (int)ASX_PROFILE_ID_COUNT; i++) {
        asx_profile_descriptor desc;
        asx_status s = asx_profile_get_descriptor((asx_profile_id)i, &desc);
        ASSERT_EQ((int)s, (int)ASX_OK);
        ASSERT_EQ(desc.max_regions, (uint32_t)ASX_MAX_REGIONS);
        ASSERT_EQ(desc.max_tasks, (uint32_t)ASX_MAX_TASKS);
        ASSERT_EQ(desc.max_obligations, (uint32_t)ASX_MAX_OBLIGATIONS);
        ASSERT_EQ(desc.max_timers, (uint32_t)ASX_MAX_TIMERS);
    }
}

TEST(active_profile_wait_matches_runtime_config_default)
{
    asx_profile_descriptor desc;
    asx_runtime_config cfg;

    ASSERT_EQ((int)asx_profile_get_descriptor(asx_profile_active(), &desc), (int)ASX_OK);
    asx_runtime_config_init(&cfg);
    ASSERT_EQ((int)cfg.wait_policy, (int)desc.default_wait);
}

TEST(descriptor_all_profiles_nonzero_limits)
{
    int i;
    for (i = 0; i < (int)ASX_PROFILE_ID_COUNT; i++) {
        asx_profile_descriptor desc;
        asx_status s = asx_profile_get_descriptor((asx_profile_id)i, &desc);
        ASSERT_EQ((int)s, (int)ASX_OK);
        ASSERT_TRUE(desc.max_regions > 0);
        ASSERT_TRUE(desc.max_tasks > 0);
        ASSERT_TRUE(desc.max_obligations > 0);
        ASSERT_TRUE(desc.max_timers > 0);
        ASSERT_TRUE(desc.name != NULL);
    }
}

/* -------------------------------------------------------------------
 * Property classification tests
 * ------------------------------------------------------------------- */

TEST(all_properties_are_operational)
{
    int i;
    for (i = 0; i < (int)ASX_PPROP_COUNT; i++) {
        asx_property_class cls = asx_profile_property_class((asx_profile_property)i);
        ASSERT_EQ((int)cls, (int)ASX_PROP_OPERATIONAL);
    }
}

TEST(property_names_all_valid)
{
    int i;
    for (i = 0; i < (int)ASX_PPROP_COUNT; i++) {
        const char *name = asx_profile_property_name((asx_profile_property)i);
        ASSERT_TRUE(name != NULL);
        ASSERT_TRUE(strlen(name) > 0);
    }
}

TEST(property_name_out_of_range)
{
    const char *name = asx_profile_property_name((asx_profile_property)99);
    ASSERT_STR_EQ(name, "unknown");
}

/* -------------------------------------------------------------------
 * Semantic rule enforcement tests
 * ------------------------------------------------------------------- */

TEST(all_semantic_rules_enforced)
{
    int i;
    for (i = 0; i < (int)ASX_SRULE_COUNT; i++) {
        int enforced = asx_profile_semantic_rule_enforced((asx_semantic_rule)i);
        ASSERT_EQ(enforced, 1);
    }
}

TEST(semantic_rule_count)
{
    uint32_t count = asx_profile_semantic_rule_count();
    ASSERT_EQ((int)count, (int)ASX_SRULE_COUNT);
    ASSERT_EQ((int)count, 8);
}

TEST(semantic_rule_names_all_valid)
{
    int i;
    for (i = 0; i < (int)ASX_SRULE_COUNT; i++) {
        const char *name = asx_semantic_rule_name((asx_semantic_rule)i);
        ASSERT_TRUE(name != NULL);
        ASSERT_TRUE(strlen(name) > 0);
    }
}

TEST(semantic_rule_name_out_of_range)
{
    const char *name = asx_semantic_rule_name((asx_semantic_rule)99);
    ASSERT_STR_EQ(name, "unknown");
}

TEST(semantic_rule_lifecycle)
{
    const char *name = asx_semantic_rule_name(ASX_SRULE_LIFECYCLE_TRANSITIONS);
    ASSERT_STR_EQ(name, "lifecycle_transitions");
}

TEST(semantic_rule_cancel)
{
    const char *name = asx_semantic_rule_name(ASX_SRULE_CANCEL_PROTOCOL);
    ASSERT_STR_EQ(name, "cancel_protocol");
}

/* -------------------------------------------------------------------
 * Digest comparison tests
 * ------------------------------------------------------------------- */

TEST(digest_compare_same_passes)
{
    asx_parity_result result;
    int ok = asx_profile_digest_compare(
        0x1234567890ABCDEFULL, ASX_PROFILE_ID_CORE,
        0x1234567890ABCDEFULL, ASX_PROFILE_ID_HFT,
        &result);
    ASSERT_EQ(ok, 1);
    ASSERT_EQ(result.pass, 1);
    ASSERT_EQ((int)result.profile_a, (int)ASX_PROFILE_ID_CORE);
    ASSERT_EQ((int)result.profile_b, (int)ASX_PROFILE_ID_HFT);
}

TEST(digest_compare_different_fails)
{
    asx_parity_result result;
    int ok = asx_profile_digest_compare(
        0xAAAAAAAAAAAAAAAAULL, ASX_PROFILE_ID_CORE,
        0xBBBBBBBBBBBBBBBBULL, ASX_PROFILE_ID_AUTOMOTIVE,
        &result);
    ASSERT_EQ(ok, 0);
    ASSERT_EQ(result.pass, 0);
    ASSERT_TRUE(result.digest_a != result.digest_b);
}

TEST(digest_compare_null_out_returns_zero)
{
    int ok = asx_profile_digest_compare(
        0x1111ULL, ASX_PROFILE_ID_CORE,
        0x1111ULL, ASX_PROFILE_ID_CORE,
        NULL);
    ASSERT_EQ(ok, 0);
}

TEST(digest_compare_zero_digests_pass)
{
    asx_parity_result result;
    int ok = asx_profile_digest_compare(
        0, ASX_PROFILE_ID_CORE,
        0, ASX_PROFILE_ID_POSIX,
        &result);
    ASSERT_EQ(ok, 1);
    ASSERT_EQ(result.pass, 1);
}

/* -------------------------------------------------------------------
 * Self-parity test (via telemetry)
 * ------------------------------------------------------------------- */

static asx_status noop_poll(void *data, asx_task_id self)
{
    (void)data;
    (void)self;
    return ASX_OK;
}

TEST(self_parity_after_scenario)
{
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    asx_parity_result result;
    uint64_t digest;
    asx_status rc;

    /* Reset telemetry to get a clean digest */
    asx_telemetry_reset();
    asx_runtime_reset();

    /* Run a minimal scenario: open region, spawn task, schedule, drain */
    ASSERT_EQ((int)asx_region_open(&rid), (int)ASX_OK);
    ASSERT_EQ((int)asx_task_spawn(rid, noop_poll, NULL, &tid), (int)ASX_OK);

    budget = asx_budget_from_polls(100);

    rc = asx_scheduler_run(rid, &budget);
    ASSERT_EQ((int)rc, (int)ASX_OK);
    rc = asx_region_close(rid);
    ASSERT_EQ((int)rc, (int)ASX_OK);

    /* Capture the digest */
    digest = asx_telemetry_digest();

    /* Self-parity: digest should match itself */
    ASSERT_EQ(asx_profile_check_parity(digest, &result), 1);
    ASSERT_EQ(result.pass, 1);
}

TEST(self_parity_divergence_detected)
{
    asx_parity_result result;
    uint64_t wrong_digest;

    asx_telemetry_reset();

    /* Emit some events to get a non-initial digest */
    asx_telemetry_emit(ASX_TRACE_SCHED_POLL, 1, 0);
    asx_telemetry_emit(ASX_TRACE_SCHED_COMPLETE, 1, 0);

    /* Intentionally wrong digest */
    wrong_digest = asx_telemetry_digest() ^ 0xFFFF;

    ASSERT_EQ(asx_profile_check_parity(wrong_digest, &result), 0);
    ASSERT_EQ(result.pass, 0);
}

TEST(self_parity_null_out_returns_zero)
{
    ASSERT_EQ(asx_profile_check_parity(0, NULL), 0);
}

/* -------------------------------------------------------------------
 * Resource class scaling tests (bd-j4m.2)
 *
 * Verifies that asx_profile_get_descriptor_for_class() correctly scales
 * resource limits by class: R1 = half, R2 = baseline, R3 = double.
 * ------------------------------------------------------------------- */

TEST(descriptor_for_class_null_returns_error)
{
    asx_status s = asx_profile_get_descriptor_for_class(
        ASX_PROFILE_ID_CORE, ASX_CLASS_R2, NULL);
    ASSERT_EQ((int)s, (int)ASX_E_INVALID_ARGUMENT);
}

TEST(descriptor_for_class_invalid_id_returns_error)
{
    asx_profile_descriptor desc;
    asx_status s = asx_profile_get_descriptor_for_class(
        (asx_profile_id)99, ASX_CLASS_R2, &desc);
    ASSERT_EQ((int)s, (int)ASX_E_INVALID_ARGUMENT);
}

TEST(descriptor_for_class_invalid_class_returns_error)
{
    asx_profile_descriptor desc;
    asx_status s = asx_profile_get_descriptor_for_class(
        ASX_PROFILE_ID_CORE, (asx_resource_class)99, &desc);
    ASSERT_EQ((int)s, (int)ASX_E_INVALID_ARGUMENT);
}

TEST(descriptor_for_class_r2_matches_baseline)
{
    asx_profile_descriptor base, scaled;
    asx_status s1, s2;

    s1 = asx_profile_get_descriptor(ASX_PROFILE_ID_CORE, &base);
    s2 = asx_profile_get_descriptor_for_class(
             ASX_PROFILE_ID_CORE, ASX_CLASS_R2, &scaled);

    ASSERT_EQ((int)s1, (int)ASX_OK);
    ASSERT_EQ((int)s2, (int)ASX_OK);

    /* R2 is the baseline — limits should match */
    ASSERT_EQ(scaled.max_regions, base.max_regions);
    ASSERT_EQ(scaled.max_tasks, base.max_tasks);
    ASSERT_EQ(scaled.max_obligations, base.max_obligations);
    ASSERT_EQ(scaled.max_timers, base.max_timers);
    ASSERT_EQ(scaled.trace_capacity, base.trace_capacity);
    ASSERT_EQ((int)scaled.resource_class, (int)ASX_CLASS_R2);
}

TEST(descriptor_for_class_r1_halves_limits)
{
    asx_profile_descriptor base, r1;
    asx_status s1, s2;

    s1 = asx_profile_get_descriptor(ASX_PROFILE_ID_CORE, &base);
    s2 = asx_profile_get_descriptor_for_class(
             ASX_PROFILE_ID_CORE, ASX_CLASS_R1, &r1);

    ASSERT_EQ((int)s1, (int)ASX_OK);
    ASSERT_EQ((int)s2, (int)ASX_OK);

    /* R1 halves the limits (floor division, minimum 1) */
    ASSERT_EQ(r1.max_regions, base.max_regions / 2);
    ASSERT_EQ(r1.max_tasks, base.max_tasks / 2);
    ASSERT_EQ(r1.max_obligations, base.max_obligations / 2);
    ASSERT_EQ(r1.max_timers, base.max_timers / 2);
    ASSERT_EQ(r1.trace_capacity, base.trace_capacity / 2);
    ASSERT_EQ((int)r1.resource_class, (int)ASX_CLASS_R1);

    /* All limits must remain > 0 */
    ASSERT_TRUE(r1.max_regions > 0);
    ASSERT_TRUE(r1.max_tasks > 0);
    ASSERT_TRUE(r1.max_obligations > 0);
    ASSERT_TRUE(r1.max_timers > 0);
    ASSERT_TRUE(r1.trace_capacity > 0);
}

TEST(descriptor_for_class_r3_doubles_limits)
{
    asx_profile_descriptor base, r3;
    asx_status s1, s2;

    s1 = asx_profile_get_descriptor(ASX_PROFILE_ID_CORE, &base);
    s2 = asx_profile_get_descriptor_for_class(
             ASX_PROFILE_ID_CORE, ASX_CLASS_R3, &r3);

    ASSERT_EQ((int)s1, (int)ASX_OK);
    ASSERT_EQ((int)s2, (int)ASX_OK);

    /* R3 doubles the limits */
    ASSERT_EQ(r3.max_regions, base.max_regions * 2);
    ASSERT_EQ(r3.max_tasks, base.max_tasks * 2);
    ASSERT_EQ(r3.max_obligations, base.max_obligations * 2);
    ASSERT_EQ(r3.max_timers, base.max_timers * 2);
    ASSERT_EQ(r3.trace_capacity, base.trace_capacity * 2);
    ASSERT_EQ((int)r3.resource_class, (int)ASX_CLASS_R3);
}

TEST(descriptor_for_class_preserves_non_limit_fields)
{
    asx_profile_descriptor base, r1;

    ASSERT_EQ((int)asx_profile_get_descriptor(ASX_PROFILE_ID_HFT, &base), (int)ASX_OK);
    ASSERT_EQ((int)asx_profile_get_descriptor_for_class(
        ASX_PROFILE_ID_HFT, ASX_CLASS_R1, &r1), (int)ASX_OK);

    /* Resource class scaling must not change non-limit fields */
    ASSERT_EQ((int)r1.id, (int)base.id);
    ASSERT_STR_EQ(r1.name, base.name);
    ASSERT_EQ((int)r1.default_wait, (int)base.default_wait);
    ASSERT_EQ(r1.ghost_monitors, base.ghost_monitors);
    ASSERT_EQ(r1.allocator_sealable, base.allocator_sealable);
}

TEST(descriptor_for_class_embedded_router_r1)
{
    asx_profile_descriptor base, r1;

    ASSERT_EQ((int)asx_profile_get_descriptor(
        ASX_PROFILE_ID_EMBEDDED_ROUTER, &base), (int)ASX_OK);
    ASSERT_EQ((int)asx_profile_get_descriptor_for_class(
        ASX_PROFILE_ID_EMBEDDED_ROUTER, ASX_CLASS_R1, &r1), (int)ASX_OK);

    /* Embedded router R1: very constrained but all limits > 0 */
    ASSERT_EQ(r1.max_regions, base.max_regions / 2);
    ASSERT_TRUE(r1.max_regions >= 1);
    ASSERT_EQ(r1.max_tasks, base.max_tasks / 2);
    ASSERT_TRUE(r1.max_tasks >= 1);
    ASSERT_EQ((int)r1.default_wait, (int)ASX_WAIT_BUSY_SPIN);
    ASSERT_EQ(r1.allocator_sealable, 1);
}

TEST(descriptor_for_class_all_profiles_r1_nonzero)
{
    int i;
    for (i = 0; i < (int)ASX_PROFILE_ID_COUNT; i++) {
        asx_profile_descriptor desc;
        asx_status s = asx_profile_get_descriptor_for_class(
            (asx_profile_id)i, ASX_CLASS_R1, &desc);
        ASSERT_EQ((int)s, (int)ASX_OK);
        /* Even at R1, no limit may be zero */
        ASSERT_TRUE(desc.max_regions > 0);
        ASSERT_TRUE(desc.max_tasks > 0);
        ASSERT_TRUE(desc.max_obligations > 0);
        ASSERT_TRUE(desc.max_timers > 0);
        ASSERT_TRUE(desc.trace_capacity > 0);
    }
}

TEST(resource_class_name_r1)
{
    ASSERT_STR_EQ(asx_resource_class_name(ASX_CLASS_R1), "R1");
}

TEST(resource_class_name_r2)
{
    ASSERT_STR_EQ(asx_resource_class_name(ASX_CLASS_R2), "R2");
}

TEST(resource_class_name_r3)
{
    ASSERT_STR_EQ(asx_resource_class_name(ASX_CLASS_R3), "R3");
}

TEST(resource_class_name_out_of_range)
{
    ASSERT_STR_EQ(asx_resource_class_name((asx_resource_class)99), "UNKNOWN");
}

/* -------------------------------------------------------------------
 * Trace config initialization tests (bd-j4m.2)
 *
 * Verifies per-resource-class trace defaults.
 * ------------------------------------------------------------------- */

TEST(trace_config_null_returns_error)
{
    asx_status s = asx_trace_config_init(NULL, ASX_CLASS_R2);
    ASSERT_EQ((int)s, (int)ASX_E_INVALID_ARGUMENT);
}

TEST(trace_config_invalid_class_returns_error)
{
    asx_trace_config cfg;
    asx_status s = asx_trace_config_init(&cfg, (asx_resource_class)99);
    ASSERT_EQ((int)s, (int)ASX_E_INVALID_ARGUMENT);
}

TEST(trace_config_r1_defaults)
{
    asx_trace_config cfg;
    ASSERT_EQ((int)asx_trace_config_init(&cfg, ASX_CLASS_R1), (int)ASX_OK);
    ASSERT_EQ((int)cfg.mode, (int)ASX_TRACE_MODE_RAM_RING);
    ASSERT_EQ(cfg.ring_capacity, (uint32_t)64);
    ASSERT_EQ(cfg.wear_safe, 1);
    ASSERT_EQ(cfg.flush_interval_ms, (uint32_t)0);
}

TEST(trace_config_r2_defaults)
{
    asx_trace_config cfg;
    ASSERT_EQ((int)asx_trace_config_init(&cfg, ASX_CLASS_R2), (int)ASX_OK);
    ASSERT_EQ((int)cfg.mode, (int)ASX_TRACE_MODE_RAM_RING);
    ASSERT_EQ(cfg.ring_capacity, (uint32_t)256);
    ASSERT_EQ(cfg.wear_safe, 1);
    ASSERT_EQ(cfg.flush_interval_ms, (uint32_t)0);
}

TEST(trace_config_r3_defaults)
{
    asx_trace_config cfg;
    ASSERT_EQ((int)asx_trace_config_init(&cfg, ASX_CLASS_R3), (int)ASX_OK);
    ASSERT_EQ((int)cfg.mode, (int)ASX_TRACE_MODE_RAM_RING);
    ASSERT_EQ(cfg.ring_capacity, (uint32_t)1024);
    ASSERT_EQ(cfg.wear_safe, 1);
    ASSERT_EQ(cfg.flush_interval_ms, (uint32_t)0);
}

TEST(trace_config_r1_smallest_ring)
{
    asx_trace_config r1, r2, r3;

    ASSERT_EQ((int)asx_trace_config_init(&r1, ASX_CLASS_R1), (int)ASX_OK);
    ASSERT_EQ((int)asx_trace_config_init(&r2, ASX_CLASS_R2), (int)ASX_OK);
    ASSERT_EQ((int)asx_trace_config_init(&r3, ASX_CLASS_R3), (int)ASX_OK);

    /* R1 < R2 < R3 in ring capacity */
    ASSERT_TRUE(r1.ring_capacity < r2.ring_capacity);
    ASSERT_TRUE(r2.ring_capacity < r3.ring_capacity);
}

TEST(trace_config_all_classes_ram_ring)
{
    int i;
    for (i = 0; i < ASX_CLASS_COUNT; i++) {
        asx_trace_config cfg;
        ASSERT_EQ((int)asx_trace_config_init(&cfg, (asx_resource_class)i), (int)ASX_OK);
        /* Default mode is always RAM ring (not persistent spill) */
        ASSERT_EQ((int)cfg.mode, (int)ASX_TRACE_MODE_RAM_RING);
        /* Always wear-safe by default */
        ASSERT_EQ(cfg.wear_safe, 1);
    }
}

/* -------------------------------------------------------------------
 * Cross-profile descriptor checks
 *
 * Wait policy intentionally differs by profile; runtime capacities are
 * currently shared across profiles and must stay aligned with runtime
 * compile-time limits.
 * ------------------------------------------------------------------- */

TEST(profiles_differ_on_wait_policy)
{
    asx_profile_descriptor core, hft, auto_desc;

    ASSERT_EQ((int)asx_profile_get_descriptor(ASX_PROFILE_ID_CORE, &core), (int)ASX_OK);
    ASSERT_EQ((int)asx_profile_get_descriptor(ASX_PROFILE_ID_HFT, &hft), (int)ASX_OK);
    ASSERT_EQ((int)asx_profile_get_descriptor(ASX_PROFILE_ID_AUTOMOTIVE, &auto_desc), (int)ASX_OK);

    ASSERT_TRUE((int)core.default_wait != (int)hft.default_wait);
    ASSERT_TRUE((int)core.default_wait != (int)auto_desc.default_wait);
    ASSERT_TRUE((int)hft.default_wait != (int)auto_desc.default_wait);
}

TEST(profiles_share_runtime_capacity_limits)
{
    asx_profile_descriptor core, freestanding;

    ASSERT_EQ((int)asx_profile_get_descriptor(ASX_PROFILE_ID_CORE, &core), (int)ASX_OK);
    ASSERT_EQ((int)asx_profile_get_descriptor(ASX_PROFILE_ID_FREESTANDING, &freestanding), (int)ASX_OK);

    ASSERT_EQ(freestanding.max_regions, core.max_regions);
    ASSERT_EQ(freestanding.max_tasks, core.max_tasks);
    ASSERT_EQ(freestanding.max_obligations, core.max_obligations);
    ASSERT_EQ(freestanding.max_timers, core.max_timers);
}

TEST(all_profiles_share_semantic_rule_count)
{
    /* Semantic rules are universal — count is the same regardless
     * of which profile is active. */
    uint32_t count = asx_profile_semantic_rule_count();
    ASSERT_EQ((int)count, 8);
}

/* -------------------------------------------------------------------
 * Runner
 * ------------------------------------------------------------------- */

int main(void)
{
    /* Identity */
    RUN_TEST(profile_active_is_core);
    RUN_TEST(profile_name_all_valid);
    RUN_TEST(profile_name_core);
    RUN_TEST(profile_name_hft);
    RUN_TEST(profile_name_automotive);
    RUN_TEST(profile_name_out_of_range);

    /* Descriptors */
    RUN_TEST(descriptor_null_returns_error);
    RUN_TEST(descriptor_invalid_id_returns_error);
    RUN_TEST(descriptor_core_values);
    RUN_TEST(descriptor_hft_busy_spin);
    RUN_TEST(descriptor_automotive_sleep_wait);
    RUN_TEST(descriptor_freestanding_defaults);
    RUN_TEST(descriptor_embedded_router_defaults);
    RUN_TEST(descriptor_limits_align_runtime_capacities);
    RUN_TEST(active_profile_wait_matches_runtime_config_default);
    RUN_TEST(descriptor_all_profiles_nonzero_limits);

    /* Properties */
    RUN_TEST(all_properties_are_operational);
    RUN_TEST(property_names_all_valid);
    RUN_TEST(property_name_out_of_range);

    /* Semantic rules */
    RUN_TEST(all_semantic_rules_enforced);
    RUN_TEST(semantic_rule_count);
    RUN_TEST(semantic_rule_names_all_valid);
    RUN_TEST(semantic_rule_name_out_of_range);
    RUN_TEST(semantic_rule_lifecycle);
    RUN_TEST(semantic_rule_cancel);

    /* Digest comparison */
    RUN_TEST(digest_compare_same_passes);
    RUN_TEST(digest_compare_different_fails);
    RUN_TEST(digest_compare_null_out_returns_zero);
    RUN_TEST(digest_compare_zero_digests_pass);

    /* Self-parity */
    RUN_TEST(self_parity_after_scenario);
    RUN_TEST(self_parity_divergence_detected);
    RUN_TEST(self_parity_null_out_returns_zero);

    /* Resource class scaling (bd-j4m.2) */
    RUN_TEST(descriptor_for_class_null_returns_error);
    RUN_TEST(descriptor_for_class_invalid_id_returns_error);
    RUN_TEST(descriptor_for_class_invalid_class_returns_error);
    RUN_TEST(descriptor_for_class_r2_matches_baseline);
    RUN_TEST(descriptor_for_class_r1_halves_limits);
    RUN_TEST(descriptor_for_class_r3_doubles_limits);
    RUN_TEST(descriptor_for_class_preserves_non_limit_fields);
    RUN_TEST(descriptor_for_class_embedded_router_r1);
    RUN_TEST(descriptor_for_class_all_profiles_r1_nonzero);
    RUN_TEST(resource_class_name_r1);
    RUN_TEST(resource_class_name_r2);
    RUN_TEST(resource_class_name_r3);
    RUN_TEST(resource_class_name_out_of_range);

    /* Trace config (bd-j4m.2) */
    RUN_TEST(trace_config_null_returns_error);
    RUN_TEST(trace_config_invalid_class_returns_error);
    RUN_TEST(trace_config_r1_defaults);
    RUN_TEST(trace_config_r2_defaults);
    RUN_TEST(trace_config_r3_defaults);
    RUN_TEST(trace_config_r1_smallest_ring);
    RUN_TEST(trace_config_all_classes_ram_ring);

    /* Cross-profile divergence */
    RUN_TEST(profiles_differ_on_wait_policy);
    RUN_TEST(profiles_share_runtime_capacity_limits);
    RUN_TEST(all_profiles_share_semantic_rule_count);

    TEST_REPORT();
    return test_failures > 0 ? 1 : 0;
}
