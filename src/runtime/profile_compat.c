/*
 * profile_compat.c — profile compatibility baseline and semantic parity enforcement
 *
 * Implements runtime-queryable profile identity, operational descriptors,
 * semantic rule enforcement, and digest-based parity checking.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/runtime/profile_compat.h>
#include <asx/runtime/telemetry.h>
#include <asx/runtime/runtime.h>
#include <asx/time/timer_wheel.h>
#include <stddef.h>

/* -------------------------------------------------------------------
 * Profile identity
 * ------------------------------------------------------------------- */

asx_profile_id asx_profile_active(void)
{
#if defined(ASX_PROFILE_PARALLEL)
    return ASX_PROFILE_ID_PARALLEL;
#elif defined(ASX_PROFILE_HFT)
    return ASX_PROFILE_ID_HFT;
#elif defined(ASX_PROFILE_AUTOMOTIVE)
    return ASX_PROFILE_ID_AUTOMOTIVE;
#elif defined(ASX_PROFILE_EMBEDDED_ROUTER)
    return ASX_PROFILE_ID_EMBEDDED_ROUTER;
#elif defined(ASX_PROFILE_FREESTANDING)
    return ASX_PROFILE_ID_FREESTANDING;
#elif defined(ASX_PROFILE_WIN32)
    return ASX_PROFILE_ID_WIN32;
#elif defined(ASX_PROFILE_POSIX)
    return ASX_PROFILE_ID_POSIX;
#else
    return ASX_PROFILE_ID_CORE;
#endif
}

/* -------------------------------------------------------------------
 * Profile name lookup
 * ------------------------------------------------------------------- */

static const char *g_profile_names[ASX_PROFILE_ID_COUNT] = {
    "CORE",
    "POSIX",
    "WIN32",
    "FREESTANDING",
    "EMBEDDED_ROUTER",
    "HFT",
    "AUTOMOTIVE",
    "PARALLEL"
};

const char *asx_profile_name(asx_profile_id id)
{
    if ((int)id < 0 || (int)id >= ASX_PROFILE_ID_COUNT) {
        return "UNKNOWN";
    }
    return g_profile_names[(int)id];
}

/* -------------------------------------------------------------------
 * Profile descriptors
 *
 * Operational parameters for each profile. These are the properties
 * that MAY differ between profiles without violating parity.
 * ------------------------------------------------------------------- */

/* -------------------------------------------------------------------
 * Resource class scaling
 *
 * Base descriptors use R2 (balanced) limits. R1 halves and R3 doubles
 * the resource limits while leaving wait policy and feature flags
 * unchanged.
 * ------------------------------------------------------------------- */

/* Scale factor table: {regions, tasks, obligations, timers, trace_capacity}
 * Indexed by asx_resource_class. Expressed as {numerator, denominator}. */
static const uint32_t g_class_scale_num[ASX_CLASS_COUNT] = { 1, 1, 2 };
static const uint32_t g_class_scale_den[ASX_CLASS_COUNT] = { 2, 1, 1 };

/* Base descriptors at R2 (balanced) resource class */
static const asx_profile_descriptor g_descriptors[ASX_PROFILE_ID_COUNT] = {
    /* CORE */
    {
        ASX_PROFILE_ID_CORE, "CORE",
        ASX_WAIT_YIELD,
        ASX_MAX_REGIONS, ASX_MAX_TASKS, ASX_MAX_OBLIGATIONS, ASX_MAX_TIMERS,
        1, 0,
        ASX_CLASS_R2, 1024
    },
    /* POSIX */
    {
        ASX_PROFILE_ID_POSIX, "POSIX",
        ASX_WAIT_YIELD,
        ASX_MAX_REGIONS, ASX_MAX_TASKS, ASX_MAX_OBLIGATIONS, ASX_MAX_TIMERS,
        1, 0,
        ASX_CLASS_R2, 1024
    },
    /* WIN32 */
    {
        ASX_PROFILE_ID_WIN32, "WIN32",
        ASX_WAIT_YIELD,
        ASX_MAX_REGIONS, ASX_MAX_TASKS, ASX_MAX_OBLIGATIONS, ASX_MAX_TIMERS,
        1, 0,
        ASX_CLASS_R2, 1024
    },
    /* FREESTANDING */
    {
        ASX_PROFILE_ID_FREESTANDING, "FREESTANDING",
        ASX_WAIT_YIELD,
        ASX_MAX_REGIONS, ASX_MAX_TASKS, ASX_MAX_OBLIGATIONS, ASX_MAX_TIMERS,
        1, 1,
        ASX_CLASS_R2, 256
    },
    /* EMBEDDED_ROUTER */
    {
        ASX_PROFILE_ID_EMBEDDED_ROUTER, "EMBEDDED_ROUTER",
        ASX_WAIT_BUSY_SPIN,
        ASX_MAX_REGIONS, ASX_MAX_TASKS, ASX_MAX_OBLIGATIONS, ASX_MAX_TIMERS,
        1, 1,
        ASX_CLASS_R2, 256
    },
    /* HFT */
    {
        ASX_PROFILE_ID_HFT, "HFT",
        ASX_WAIT_BUSY_SPIN,
        ASX_MAX_REGIONS, ASX_MAX_TASKS, ASX_MAX_OBLIGATIONS, ASX_MAX_TIMERS,
        0, 0,
        ASX_CLASS_R2, 1024
    },
    /* AUTOMOTIVE */
    {
        ASX_PROFILE_ID_AUTOMOTIVE, "AUTOMOTIVE",
        ASX_WAIT_SLEEP,
        ASX_MAX_REGIONS, ASX_MAX_TASKS, ASX_MAX_OBLIGATIONS, ASX_MAX_TIMERS,
        1, 0,
        ASX_CLASS_R2, 1024
    },
    /* PARALLEL */
    {
        ASX_PROFILE_ID_PARALLEL, "PARALLEL",
        ASX_WAIT_YIELD,
        ASX_MAX_REGIONS, ASX_MAX_TASKS, ASX_MAX_OBLIGATIONS, ASX_MAX_TIMERS,
        1, 0,
        ASX_CLASS_R2, 1024
    }
};

static void descriptor_apply_class(asx_profile_descriptor *desc,
                                    asx_resource_class cls)
{
    uint32_t num = g_class_scale_num[(int)cls];
    uint32_t den = g_class_scale_den[(int)cls];

    /* Scale resource limits, ensuring minimum of 1 */
    desc->max_regions     = (desc->max_regions * num / den) > 0
                            ? (desc->max_regions * num / den) : 1;
    desc->max_tasks       = (desc->max_tasks * num / den) > 0
                            ? (desc->max_tasks * num / den) : 1;
    desc->max_obligations = (desc->max_obligations * num / den) > 0
                            ? (desc->max_obligations * num / den) : 1;
    desc->max_timers      = (desc->max_timers * num / den) > 0
                            ? (desc->max_timers * num / den) : 1;
    desc->trace_capacity  = (desc->trace_capacity * num / den) > 0
                            ? (desc->trace_capacity * num / den) : 1;
    desc->resource_class  = cls;
}

asx_status asx_profile_get_descriptor(asx_profile_id id,
                                       asx_profile_descriptor *desc)
{
    if (desc == NULL) return ASX_E_INVALID_ARGUMENT;
    if ((int)id < 0 || (int)id >= ASX_PROFILE_ID_COUNT) {
        return ASX_E_INVALID_ARGUMENT;
    }
    *desc = g_descriptors[(int)id];
    return ASX_OK;
}

asx_status asx_profile_get_descriptor_for_class(
    asx_profile_id id,
    asx_resource_class cls,
    asx_profile_descriptor *desc)
{
    if (desc == NULL) return ASX_E_INVALID_ARGUMENT;
    if ((int)id < 0 || (int)id >= ASX_PROFILE_ID_COUNT) {
        return ASX_E_INVALID_ARGUMENT;
    }
    if ((int)cls < 0 || (int)cls >= ASX_CLASS_COUNT) {
        return ASX_E_INVALID_ARGUMENT;
    }
    *desc = g_descriptors[(int)id];
    descriptor_apply_class(desc, cls);
    return ASX_OK;
}

/* -------------------------------------------------------------------
 * Resource class name lookup
 * ------------------------------------------------------------------- */

static const char *g_class_names[ASX_CLASS_COUNT] = {
    "R1", "R2", "R3"
};

const char *asx_resource_class_name(asx_resource_class cls)
{
    if ((int)cls < 0 || (int)cls >= ASX_CLASS_COUNT) {
        return "UNKNOWN";
    }
    return g_class_names[(int)cls];
}

/* -------------------------------------------------------------------
 * Trace config defaults per resource class
 * ------------------------------------------------------------------- */

asx_status asx_trace_config_init(asx_trace_config *cfg,
                                  asx_resource_class cls)
{
    if (cfg == NULL) return ASX_E_INVALID_ARGUMENT;
    if ((int)cls < 0 || (int)cls >= ASX_CLASS_COUNT) {
        return ASX_E_INVALID_ARGUMENT;
    }

    cfg->mode = ASX_TRACE_MODE_RAM_RING;
    cfg->wear_safe = 1;
    cfg->flush_interval_ms = 0;

    switch (cls) {
    case ASX_CLASS_R1:
        cfg->ring_capacity = 64;
        break;
    case ASX_CLASS_R2:
        cfg->ring_capacity = 256;
        break;
    case ASX_CLASS_R3:
        cfg->ring_capacity = 1024;
        break;
    case ASX_CLASS_COUNT:
        cfg->ring_capacity = 256;
        break;
    }

    return ASX_OK;
}

/* -------------------------------------------------------------------
 * Property classification
 *
 * All profile properties are OPERATIONAL by definition — they exist
 * in the descriptor precisely because they are allowed to vary.
 * Semantic invariants are not properties of profiles; they are
 * universal rules that all profiles must satisfy.
 * ------------------------------------------------------------------- */

asx_property_class asx_profile_property_class(asx_profile_property prop)
{
    (void)prop;
    /* All profile properties are operational by design.
     * Semantic rules are enforced separately via asx_semantic_rule. */
    return ASX_PROP_OPERATIONAL;
}

static const char *g_property_names[ASX_PPROP_COUNT] = {
    "wait_policy",
    "max_regions",
    "max_tasks",
    "max_obligations",
    "max_timers",
    "ghost_monitors",
    "trace_capacity",
    "allocator_sealable",
    "resource_class"
};

const char *asx_profile_property_name(asx_profile_property prop)
{
    if ((int)prop < 0 || (int)prop >= ASX_PPROP_COUNT) {
        return "unknown";
    }
    return g_property_names[(int)prop];
}

/* -------------------------------------------------------------------
 * Semantic rule enforcement
 * ------------------------------------------------------------------- */

static const char *g_rule_names[ASX_SRULE_COUNT] = {
    "lifecycle_transitions",
    "cancel_protocol",
    "obligation_linearity",
    "deterministic_ordering",
    "handle_validation",
    "error_codes",
    "quiescence_definition",
    "budget_exhaustion"
};

int asx_profile_semantic_rule_enforced(asx_semantic_rule rule)
{
    /* All semantic rules are always enforced. This is a compile-time
     * guarantee — the state machines, handle validation, and error
     * codes are identical across all profile builds. */
    (void)rule;
    return 1;
}

const char *asx_semantic_rule_name(asx_semantic_rule rule)
{
    if ((int)rule < 0 || (int)rule >= ASX_SRULE_COUNT) {
        return "unknown";
    }
    return g_rule_names[(int)rule];
}

uint32_t asx_profile_semantic_rule_count(void)
{
    return ASX_SRULE_COUNT;
}

/* -------------------------------------------------------------------
 * Digest-based parity checking
 * ------------------------------------------------------------------- */

int asx_profile_digest_compare(uint64_t digest_a,
                                asx_profile_id profile_a,
                                uint64_t digest_b,
                                asx_profile_id profile_b,
                                asx_parity_result *out)
{
    if (out == NULL) return 0;

    out->profile_a = profile_a;
    out->profile_b = profile_b;
    out->digest_a = digest_a;
    out->digest_b = digest_b;

    if (digest_a == digest_b) {
        out->pass = 1;
        out->divergence_rule = ASX_SRULE_LIFECYCLE_TRANSITIONS; /* unused */
        out->divergence_index = 0;
        return 1;
    }

    out->pass = 0;
    /* Without event-level detail we can only report the top-level
     * digest mismatch. The CI runner's JSONL diff provides the
     * event-level divergence detail. */
    out->divergence_rule = ASX_SRULE_DETERMINISTIC_ORDERING;
    out->divergence_index = 0;
    return 0;
}

int asx_profile_check_parity(uint64_t expected_digest,
                              asx_parity_result *out)
{
    uint64_t current;

    if (out == NULL) return 0;

    current = asx_telemetry_digest();

    out->profile_a = asx_profile_active();
    out->profile_b = asx_profile_active();
    out->digest_a = current;
    out->digest_b = expected_digest;

    if (current == expected_digest) {
        out->pass = 1;
        out->divergence_rule = ASX_SRULE_LIFECYCLE_TRANSITIONS;
        out->divergence_index = 0;
        return 1;
    }

    out->pass = 0;
    out->divergence_rule = ASX_SRULE_DETERMINISTIC_ORDERING;
    out->divergence_index = 0;
    return 0;
}
