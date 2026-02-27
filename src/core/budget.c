/*
 * budget.c — budget algebra implementation
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/core/budget.h>

asx_budget asx_budget_infinite(void) {
    asx_budget b;
    b.deadline    = 0;           /* 0 = unconstrained */
    b.poll_quota  = UINT32_MAX;
    b.cost_quota  = UINT64_MAX;  /* unconstrained */
    b.priority    = 255;
    return b;
}

asx_budget asx_budget_zero(void) {
    asx_budget b;
    b.deadline    = 1;  /* earliest possible (non-zero = has deadline) */
    b.poll_quota  = 0;
    b.cost_quota  = 0;
    b.priority    = 0;
    return b;
}

static uint64_t min_u64(uint64_t a, uint64_t b) { return a < b ? a : b; }
static uint32_t min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }
static uint8_t  min_u8(uint8_t a, uint8_t b)    { return a < b ? a : b; }

/* deadline meet: earliest finite deadline wins; 0 means unconstrained */
static asx_time min_deadline(asx_time a, asx_time b) {
    if (a == 0) return b;
    if (b == 0) return a;
    return a < b ? a : b;
}

asx_budget asx_budget_meet(const asx_budget *a, const asx_budget *b) {
    asx_budget result;
    result.deadline   = min_deadline(a->deadline, b->deadline);
    result.poll_quota = min_u32(a->poll_quota, b->poll_quota);
    result.cost_quota = min_u64(a->cost_quota, b->cost_quota);
    result.priority   = min_u8(a->priority, b->priority);
    return result;
}

uint32_t asx_budget_consume_poll(asx_budget *b) {
    if (b->poll_quota == 0) return 0;
    return b->poll_quota--;
}

int asx_budget_consume_cost(asx_budget *b, uint64_t cost) {
    if (b->cost_quota == UINT64_MAX) return 1; /* unconstrained */
    if (cost > b->cost_quota) return 0;        /* insufficient, no mutation */
    b->cost_quota -= cost;
    return 1;
}

int asx_budget_is_exhausted(const asx_budget *b) {
    return b->poll_quota == 0
        || (b->cost_quota != UINT64_MAX && b->cost_quota == 0);
}

int asx_budget_is_past_deadline(const asx_budget *b, asx_time now) {
    if (b->deadline == 0) return 0; /* no deadline */
    return now >= b->deadline;
}

uint32_t asx_budget_polls(const asx_budget *b) {
    return b->poll_quota;
}

asx_budget asx_budget_from_polls(uint32_t polls) {
    asx_budget b;
    b.deadline   = 0;
    b.poll_quota = polls;
    b.cost_quota = UINT64_MAX;
    b.priority   = 255;
    return b;
}
