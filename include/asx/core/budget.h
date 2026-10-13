/*
 * asx/core/budget.h — budget algebra and exhaustion semantics
 *
 * Budget = (deadline, poll_quota, cost_quota, priority)
 * Meet/combine is componentwise tightening.
 * INFINITE is the meet identity; ZERO is absorbing.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_CORE_BUDGET_H
#define ASX_CORE_BUDGET_H

#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>
#include <stdint.h>

/* asx_time is defined in asx_ids.h. */

#define ASX_TIME_ZERO ((asx_time)0)
#define ASX_TIME_INFINITE ((asx_time)UINT64_MAX)

/* Default priority of an ordinary budget (Rust Budget::new). */
#define ASX_BUDGET_DEFAULT_PRIORITY 128u

/* Budget structure */
typedef struct {
    /* ASX_TIME_INFINITE = unconstrained (Rust None); any other value is a
     * deadline, 0 included (Rust Some(Time::ZERO): due at once). */
    asx_time deadline;
    uint32_t poll_quota;
    uint64_t cost_quota; /* UINT64_MAX = unconstrained */
    uint8_t priority;    /* higher = more urgent; meet takes the max */
} asx_budget;

/* Identity element for meet: unconstrained, priority 0 (Rust INFINITE) */
ASX_API asx_budget asx_budget_infinite(void);

/* Absorbing element: maximally constrained, priority 255 (Rust ZERO) */
ASX_API asx_budget asx_budget_zero(void);

/* Ordinary budget: unconstrained, priority 128 (Rust Budget::new) */
ASX_API asx_budget asx_budget_new(void);

/* Meet: the tighter constraint wins componentwise. Min deadline, min
 * poll quota, min cost quota, MAX priority (Rust Budget::combine). */
ASX_API asx_budget asx_budget_meet(const asx_budget *a, const asx_budget *b);

/* Consume one poll from quota. Returns old quota or 0 if exhausted. */
ASX_API uint32_t asx_budget_consume_poll(asx_budget *b);

/* Consume cost from quota. Returns nonzero on success, 0 if insufficient (no mutation). */
ASX_API int asx_budget_consume_cost(asx_budget *b, uint64_t cost);

/* Check if structurally exhausted (poll=0 or cost=0) */
ASX_API int asx_budget_is_exhausted(const asx_budget *b);

/* Check if deadline exceeded */
ASX_API int asx_budget_is_past_deadline(const asx_budget *b, asx_time now);

/* Query remaining poll quota */
ASX_API uint32_t asx_budget_polls(const asx_budget *b);

/* Construct an ordinary budget with a poll quota (Rust
 * Budget::new().with_poll_quota): priority 128, other fields unconstrained */
ASX_API asx_budget asx_budget_from_polls(uint32_t polls);

/* ------------------------------------------------------------------ */
/* Capability budget (Rust types/budget.rs CapabilityBudget, pinned    */
/* rev 5e60b1c: dimension 788-815, requirements 817-889, refusal       */
/* 891-916, CapabilityBudget 918-1062, validation 1064-1103)           */
/* ------------------------------------------------------------------ */

/* The resource envelopes a region and its Cx carry: separate from the
 * poll/cost Budget above. Each dimension is optional ("has_" 0 = Rust's
 * None: no explicit envelope). A child inherits its parent's envelopes and
 * can only tighten them (meet takes the smaller of each). A zeroed value is
 * UNSPECIFIED. Admission can fail closed by requiring dimensions. */

typedef enum {
    ASX_CAPABILITY_MEMORY_BYTES = 0,   /* resident memory in bytes */
    ASX_CAPABILITY_CPU_UNITS = 1,      /* abstract CPU units */
    ASX_CAPABILITY_IO_BYTES = 2,       /* I/O bytes */
    ASX_CAPABILITY_CLEANUP = 3,        /* cleanup budget of drain/finalizer paths */
    ASX_CAPABILITY_ARTIFACT_BYTES = 4, /* bytes emitted as proof, trace or evidence */
    ASX_CAPABILITY_DIMENSION_COUNT = 5
} asx_capability_dimension;

/* The presence flags are grouped after the values: 64 bytes, where a flag
 * between each pair of 8-byte fields made it 96 (a region slot, a Cx and a
 * queued region command each hold one). */
typedef struct {
    uint64_t memory_bytes;
    uint64_t cpu_units;
    uint64_t io_bytes;
    uint64_t artifact_bytes;
    asx_budget cleanup_budget;
    uint8_t has_memory_bytes;
    uint8_t has_cpu_units;
    uint8_t has_io_bytes;
    uint8_t has_cleanup_budget;
    uint8_t has_artifact_bytes;
} asx_capability_budget;

/* Dimensions admission requires (Rust CapabilityBudgetRequirements): a
 * nonzero field requires that envelope to be present and not spent. A zeroed
 * value requires nothing. */
typedef struct {
    uint8_t memory_bytes;
    uint8_t cpu_units;
    uint8_t io_bytes;
    uint8_t cleanup;
    uint8_t artifact_bytes;
} asx_capability_requirements;

/* Why admission was refused (Rust CapabilityBudgetRefusal). */
typedef enum {
    ASX_CAPABILITY_REFUSAL_NONE = 0,
    ASX_CAPABILITY_MISSING_REQUIRED = 1, /* no inherited or child limit */
    ASX_CAPABILITY_EXHAUSTED = 2         /* present but already spent (0, or an exhausted budget) */
} asx_capability_refusal_kind;

typedef struct {
    asx_capability_refusal_kind kind;
    asx_capability_dimension dimension;
} asx_capability_refusal;

/* No explicit envelope in any dimension (Rust CapabilityBudget::UNSPECIFIED). */
ASX_API asx_capability_budget asx_capability_budget_unspecified(void);

/* Builders (Rust with_memory_bytes, with_cpu_units, with_io_bytes,
 * with_cleanup_budget, with_artifact_bytes): return `b` with that envelope
 * set. */
ASX_API asx_capability_budget asx_capability_budget_with_memory_bytes(asx_capability_budget b,
                                                                      uint64_t bytes);
ASX_API asx_capability_budget asx_capability_budget_with_cpu_units(asx_capability_budget b,
                                                                   uint64_t units);
ASX_API asx_capability_budget asx_capability_budget_with_io_bytes(asx_capability_budget b,
                                                                  uint64_t bytes);
ASX_API asx_capability_budget asx_capability_budget_with_cleanup_budget(asx_capability_budget b,
                                                                        const asx_budget *cleanup);
ASX_API asx_capability_budget asx_capability_budget_with_artifact_bytes(asx_capability_budget b,
                                                                        uint64_t bytes);

/* Parent and child envelopes, the tightest of each dimension (Rust
 * CapabilityBudget::meet): a dimension only one side has is kept, both sides
 * give the smaller, the cleanup budgets meet as asx_budget_meet. NULL reads
 * as UNSPECIFIED. */
ASX_API asx_capability_budget asx_capability_budget_meet(const asx_capability_budget *parent,
                                                         const asx_capability_budget *child);

/* Checks `b` against `req` (Rust validate): ASX_OK, or
 * ASX_E_CAPABILITY_BUDGET_REFUSED with *refusal (if non-NULL) naming the
 * first dimension in memory, cpu, io, cleanup, artifact order that is
 * required and missing or spent. */
ASX_API ASX_MUST_USE asx_status asx_capability_budget_validate(
    const asx_capability_budget *b, const asx_capability_requirements *req,
    asx_capability_refusal *refusal);

/* The effective child envelope and its admission (Rust plan_child): the meet
 * of parent and child, validated against `req`. On ASX_OK *out is the value
 * to carry forward; on a refusal *out is not written. NULL parent or child
 * reads as UNSPECIFIED, NULL req as no requirements. */
ASX_API ASX_MUST_USE asx_status asx_capability_budget_plan_child(
    const asx_capability_budget *parent, const asx_capability_budget *child,
    const asx_capability_requirements *req, asx_capability_budget *out,
    asx_capability_refusal *refusal);

/* Stable lower-case name of a dimension (Rust as_str): "memory_bytes",
 * "cpu_units", "io_bytes", "cleanup", "artifact_bytes"; "unknown" for a value
 * that is not one. */
ASX_API const char *asx_capability_dimension_str(asx_capability_dimension dimension);

#endif /* ASX_CORE_BUDGET_H */
