/*
 * budget.c — budget algebra implementation
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/core/budget.h>
#include <stddef.h>

/* Priority follows Rust types/budget.rs: meet takes the MAX (the more
 * urgent constraint wins), so INFINITE has priority 0 (the identity) and
 * ZERO has priority 255 (absorbing). Ordinary budgets default to 128. */

asx_budget asx_budget_infinite(void) {
    asx_budget b;
    b.deadline = ASX_TIME_INFINITE; /* unconstrained (Rust None) */
    b.poll_quota = UINT32_MAX;
    b.cost_quota = UINT64_MAX; /* unconstrained */
    b.priority = 0;
    return b;
}

asx_budget asx_budget_zero(void) {
    asx_budget b;
    b.deadline = ASX_TIME_ZERO; /* Rust ZERO: Some(Time::ZERO), due at once */
    b.poll_quota = 0;
    b.cost_quota = 0;
    b.priority = 255;
    return b;
}

asx_budget asx_budget_new(void) {
    asx_budget b = asx_budget_infinite();
    b.priority = ASX_BUDGET_DEFAULT_PRIORITY;
    return b;
}

static uint64_t min_u64(uint64_t a, uint64_t b) { return a < b ? a : b; }
static uint32_t min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }
static uint8_t max_u8(uint8_t a, uint8_t b) { return a > b ? a : b; }

asx_budget asx_budget_meet(const asx_budget *a, const asx_budget *b) {
    asx_budget result;
    if (a == NULL) return b != NULL ? *b : asx_budget_infinite();
    if (b == NULL) return *a;
    /* The earlier deadline wins; unconstrained is ASX_TIME_INFINITE, so
     * the minimum needs no special case. */
    result.deadline = min_u64(a->deadline, b->deadline);
    result.poll_quota = min_u32(a->poll_quota, b->poll_quota);
    result.cost_quota = min_u64(a->cost_quota, b->cost_quota);
#ifdef ASX_MUTANT_BUDGET_CHILD_WINS /* M3 self-test mutant: a later deadline replaces */
    if (b->deadline != ASX_TIME_INFINITE) result.deadline = b->deadline;
#endif
#ifdef ASX_MUTANT_BUDGET_PRIORITY_MIN
    /* Oracle self-test mutant (tests/gates/test_oracle_mutants.sh): the
     * reversed priority semiring, min for max. Never in a release build. */
    result.priority = (uint8_t)(a->priority + b->priority - max_u8(a->priority, b->priority));
#else
    result.priority = max_u8(a->priority, b->priority);
#endif
    return result;
}

uint32_t asx_budget_consume_poll(asx_budget *b) {
    if (b == NULL || b->poll_quota == 0) return 0;
    return b->poll_quota--;
}

int asx_budget_consume_cost(asx_budget *b, uint64_t cost) {
    if (b == NULL) return 0;
    if (b->cost_quota == UINT64_MAX) return 1; /* unconstrained */
    if (cost > b->cost_quota) return 0;        /* insufficient, no mutation */
    b->cost_quota -= cost;
    return 1;
}

int asx_budget_is_exhausted(const asx_budget *b) {
    if (b == NULL) return 1;
    return b->poll_quota == 0 || (b->cost_quota != UINT64_MAX && b->cost_quota == 0);
}

int asx_budget_is_past_deadline(const asx_budget *b, asx_time now) {
    if (b == NULL || b->deadline == ASX_TIME_INFINITE) return 0; /* no deadline */
    return now >= b->deadline;
}

uint32_t asx_budget_polls(const asx_budget *b) {
    if (b == NULL) return 0;
    return b->poll_quota;
}

asx_budget asx_budget_from_polls(uint32_t polls) {
    /* Budget::new().with_poll_quota(polls) */
    asx_budget b = asx_budget_new();
    b.poll_quota = polls;
    return b;
}

/* ------------------------------------------------------------------ */
/* Capability budget (Rust types/budget.rs:788-1103)                   */
/* ------------------------------------------------------------------ */

asx_capability_budget asx_capability_budget_unspecified(void) {
    asx_capability_budget b = {0};
    return b;
}

asx_capability_budget asx_capability_budget_with_memory_bytes(asx_capability_budget b,
                                                              uint64_t bytes) {
    b.has_memory_bytes = 1u;
    b.memory_bytes = bytes;
    return b;
}

asx_capability_budget asx_capability_budget_with_cpu_units(asx_capability_budget b,
                                                           uint64_t units) {
    b.has_cpu_units = 1u;
    b.cpu_units = units;
    return b;
}

asx_capability_budget asx_capability_budget_with_io_bytes(asx_capability_budget b, uint64_t bytes) {
    b.has_io_bytes = 1u;
    b.io_bytes = bytes;
    return b;
}

asx_capability_budget asx_capability_budget_with_cleanup_budget(asx_capability_budget b,
                                                                const asx_budget *cleanup) {
    if (cleanup == NULL) return b;
    b.has_cleanup_budget = 1u;
    b.cleanup_budget = *cleanup;
    return b;
}

asx_capability_budget asx_capability_budget_with_artifact_bytes(asx_capability_budget b,
                                                                uint64_t bytes) {
    b.has_artifact_bytes = 1u;
    b.artifact_bytes = bytes;
    return b;
}

/* meet_optional_u64 (budget.rs:1065): a dimension only one side has is kept;
 * both give the smaller. */
static void cap_meet_u64(uint8_t has_a, uint64_t a, uint8_t has_b, uint64_t b, uint8_t *has_out,
                         uint64_t *out) {
    if (has_a && has_b) {
        *has_out = 1u;
#ifdef ASX_MUTANT_CAPABILITY_MEET_LOOSENS
        /* Gate mutant (tests/gates/test_oracle_mutants.sh): the larger
         * envelope wins, so a child could widen its parent's. */
        *out = a > b ? a : b;
#else
        *out = a < b ? a : b;
#endif
    } else if (has_a) {
        *has_out = 1u;
        *out = a;
    } else if (has_b) {
        *has_out = 1u;
        *out = b;
    } else {
        *has_out = 0u;
        *out = 0u;
    }
}

asx_capability_budget asx_capability_budget_meet(const asx_capability_budget *parent,
                                                 const asx_capability_budget *child) {
    asx_capability_budget p = asx_capability_budget_unspecified();
    asx_capability_budget c = asx_capability_budget_unspecified();
    asx_capability_budget out = asx_capability_budget_unspecified();

    if (parent != NULL) p = *parent;
    if (child != NULL) c = *child;

    cap_meet_u64(p.has_memory_bytes, p.memory_bytes, c.has_memory_bytes, c.memory_bytes,
                 &out.has_memory_bytes, &out.memory_bytes);
    cap_meet_u64(p.has_cpu_units, p.cpu_units, c.has_cpu_units, c.cpu_units, &out.has_cpu_units,
                 &out.cpu_units);
    cap_meet_u64(p.has_io_bytes, p.io_bytes, c.has_io_bytes, c.io_bytes, &out.has_io_bytes,
                 &out.io_bytes);
    if (p.has_cleanup_budget && c.has_cleanup_budget) {
        out.has_cleanup_budget = 1u;
        out.cleanup_budget = asx_budget_meet(&p.cleanup_budget, &c.cleanup_budget);
    } else if (p.has_cleanup_budget || c.has_cleanup_budget) {
        out.has_cleanup_budget = 1u;
        out.cleanup_budget = p.has_cleanup_budget ? p.cleanup_budget : c.cleanup_budget;
    }
    cap_meet_u64(p.has_artifact_bytes, p.artifact_bytes, c.has_artifact_bytes, c.artifact_bytes,
                 &out.has_artifact_bytes, &out.artifact_bytes);
    return out;
}

/* validate_required_u64 / validate_required_cleanup (budget.rs:1067-1103):
 * 0 when admitted, else the refusal kind. */
static asx_capability_refusal_kind cap_require_u64(int required, uint8_t has, uint64_t value) {
    if (!required) return ASX_CAPABILITY_REFUSAL_NONE;
    if (!has) return ASX_CAPABILITY_MISSING_REQUIRED;
#ifndef ASX_MUTANT_CAPABILITY_SPENT_ADMITS /* gate mutant: a spent envelope admits */
    if (value == 0u) return ASX_CAPABILITY_EXHAUSTED;
#endif
    return ASX_CAPABILITY_REFUSAL_NONE;
}

static asx_status cap_refuse(asx_capability_refusal *refusal, asx_capability_refusal_kind kind,
                             asx_capability_dimension dimension) {
    if (refusal != NULL) {
        refusal->kind = kind;
        refusal->dimension = dimension;
    }
    return ASX_E_CAPABILITY_BUDGET_REFUSED;
}

asx_status asx_capability_budget_validate(const asx_capability_budget *b,
                                          const asx_capability_requirements *req,
                                          asx_capability_refusal *refusal) {
    asx_capability_budget none = asx_capability_budget_unspecified();
    asx_capability_requirements no_req = {0};
    asx_capability_refusal_kind kind;

    if (refusal != NULL) {
        refusal->kind = ASX_CAPABILITY_REFUSAL_NONE;
        refusal->dimension = ASX_CAPABILITY_MEMORY_BYTES;
    }
    if (b == NULL) b = &none;
    if (req == NULL) req = &no_req;

    /* The order is Rust's: memory, cpu, io, cleanup, artifact. */
    kind = cap_require_u64(req->memory_bytes != 0u, b->has_memory_bytes, b->memory_bytes);
    if (kind != ASX_CAPABILITY_REFUSAL_NONE) {
        return cap_refuse(refusal, kind, ASX_CAPABILITY_MEMORY_BYTES);
    }
    kind = cap_require_u64(req->cpu_units != 0u, b->has_cpu_units, b->cpu_units);
    if (kind != ASX_CAPABILITY_REFUSAL_NONE) {
        return cap_refuse(refusal, kind, ASX_CAPABILITY_CPU_UNITS);
    }
    kind = cap_require_u64(req->io_bytes != 0u, b->has_io_bytes, b->io_bytes);
    if (kind != ASX_CAPABILITY_REFUSAL_NONE) {
        return cap_refuse(refusal, kind, ASX_CAPABILITY_IO_BYTES);
    }
    if (req->cleanup != 0u) {
        if (!b->has_cleanup_budget) {
            return cap_refuse(refusal, ASX_CAPABILITY_MISSING_REQUIRED, ASX_CAPABILITY_CLEANUP);
        }
        if (asx_budget_is_exhausted(&b->cleanup_budget)) {
            return cap_refuse(refusal, ASX_CAPABILITY_EXHAUSTED, ASX_CAPABILITY_CLEANUP);
        }
    }
    kind = cap_require_u64(req->artifact_bytes != 0u, b->has_artifact_bytes, b->artifact_bytes);
    if (kind != ASX_CAPABILITY_REFUSAL_NONE) {
        return cap_refuse(refusal, kind, ASX_CAPABILITY_ARTIFACT_BYTES);
    }
    return ASX_OK;
}

asx_status asx_capability_budget_plan_child(const asx_capability_budget *parent,
                                            const asx_capability_budget *child,
                                            const asx_capability_requirements *req,
                                            asx_capability_budget *out,
                                            asx_capability_refusal *refusal) {
    asx_capability_budget effective = asx_capability_budget_meet(parent, child);
    asx_status st = asx_capability_budget_validate(&effective, req, refusal);
    if (st != ASX_OK) return st;
    if (out != NULL) *out = effective;
    return ASX_OK;
}

const char *asx_capability_dimension_str(asx_capability_dimension dimension) {
    switch (dimension) {
    case ASX_CAPABILITY_MEMORY_BYTES: return "memory_bytes";
    case ASX_CAPABILITY_CPU_UNITS: return "cpu_units";
    case ASX_CAPABILITY_IO_BYTES: return "io_bytes";
    case ASX_CAPABILITY_CLEANUP: return "cleanup";
    case ASX_CAPABILITY_ARTIFACT_BYTES: return "artifact_bytes";
    case ASX_CAPABILITY_DIMENSION_COUNT: break;
    }
    return "unknown";
}
