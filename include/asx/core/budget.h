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

#include <stdint.h>
#include <asx/asx_export.h>
#include <asx/asx_status.h>

/* Time representation (nanoseconds since epoch, 0 = unset) */
typedef uint64_t asx_time;

#define ASX_TIME_ZERO     ((asx_time)0)
#define ASX_TIME_INFINITE ((asx_time)UINT64_MAX)

/* Budget structure */
typedef struct {
    asx_time deadline;       /* 0 = unconstrained */
    uint32_t poll_quota;
    uint64_t cost_quota;     /* UINT64_MAX = unconstrained */
    uint8_t  priority;       /* lower = tighter */
} asx_budget;

/* Identity element for meet (unconstrained) */
ASX_API asx_budget asx_budget_infinite(void);

/* Absorbing element (maximally constrained) */
ASX_API asx_budget asx_budget_zero(void);

/* Componentwise tightening: min deadline, min quota, min priority */
ASX_API asx_budget asx_budget_meet(const asx_budget *a, const asx_budget *b);

/* Consume one poll from quota. Returns old quota or 0 if exhausted. */
ASX_API uint32_t asx_budget_consume_poll(asx_budget *b);

/* Consume cost from quota. Returns nonzero on success, 0 if insufficient (no mutation). */
ASX_API int asx_budget_consume_cost(asx_budget *b, uint64_t cost);

/* Check if structurally exhausted (poll=0 or cost=0) */
ASX_API int asx_budget_is_exhausted(const asx_budget *b);

/* Check if deadline exceeded */
ASX_API int asx_budget_is_past_deadline(const asx_budget *b, asx_time now);

#endif /* ASX_CORE_BUDGET_H */
