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
    asx_time deadline; /* 0 = unconstrained */
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

#endif /* ASX_CORE_BUDGET_H */
