/*
 * test_budget_obligation.c — task/region budgets (deadline, poll quota,
 * cost quota) and obligation kinds, holders, and leak policies.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../../src/runtime/runtime_internal.h"
#include "test_harness.h"
#include "test_task.h"
#include <asx/asx.h>
#include <asx/runtime/rt.h>
#include <asx/runtime/runtime.h>
#include <string.h>

#define MS ((uint64_t)1000000u)

static asx_runtime g_rt;

/* Virtual-clock runtime (stub hooks) with the given leak policy. */
static asx_status setup_with_policy(asx_leak_response response,
                                    asx_leak_escalation_config *escalation) {
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    asx_status st;
    if (asx_runtime_is_initialized(&g_rt)) asx_runtime_shutdown(&g_rt);
    asx_runtime_config_init(&cfg);
    cfg.leak_response = response;
    cfg.leak_escalation = escalation;
    st = asx_runtime_hooks_init(&hooks);
    if (st == ASX_OK) st = asx_runtime_init(&g_rt, &cfg, &hooks);
    return st;
}

static void setup(void) {
    asx_status st = setup_with_policy(ASX_LEAK_LOG, NULL);
    (void)st;
}

/* -------------------------------------------------------------------
 * Fixtures
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t polls;
    int saw_cancel;
    asx_cancel_kind kind;
    int park;           /* park between polls (else yield) */
    uint64_t cost;      /* cost charged per poll (0 = none) */
    int cost_exhausted; /* asx_task_consume_cost reported exhaustion */
} probe_state;

/* Runs until cancelled, then records the cancel kind and completes. */
static asx_status poll_probe(void *ud, asx_task_id self) {
    probe_state *p = (probe_state *)ud;
    asx_checkpoint_result cp;
    p->polls++;
    if (asx_checkpoint(self, &cp) == ASX_OK && cp.cancelled) {
        p->saw_cancel = 1;
        p->kind = cp.kind;
        return ASX_OK;
    }
    if (p->cost != 0u && asx_task_consume_cost(self, p->cost) == ASX_E_COST_QUOTA_EXHAUSTED) {
        p->cost_exhausted = 1;
    }
    if (p->park && asx_task_park(self) != ASX_OK) return ASX_E_INVALID_STATE;
    return ASX_E_PENDING;
}

typedef struct {
    asx_region_id region;
    asx_obligation_id ob;
    asx_status reserve_status;
    int park; /* park after reserving instead of completing */
} reserver_state;

/* Reserves one obligation on the first poll; completes unless `park`. */
static asx_status poll_reserver(void *ud, asx_task_id self) {
    reserver_state *s = (reserver_state *)ud;
    asx_checkpoint_result cp;
    if (s->ob == ASX_INVALID_ID) { s->reserve_status = asx_obligation_reserve(s->region, &s->ob); }
    if (asx_checkpoint(self, &cp) == ASX_OK && cp.cancelled) return ASX_OK;
    if (!s->park) return ASX_OK;
    if (asx_task_park(self) != ASX_OK) return ASX_E_INVALID_STATE;
    return ASX_E_PENDING;
}

static asx_status poll_complete(void *ud, asx_task_id self) {
    (void)ud;
    (void)self;
    return ASX_OK;
}

static asx_status poll_commit_existing(void *ud, asx_task_id self) {
    (void)self;
    return asx_obligation_commit(*(asx_obligation_id *)ud);
}

typedef struct {
    asx_region_id region;
    asx_task_id owner;
    asx_obligation_id obligation;
    asx_status commit_status;
    asx_status abort_status;
    asx_status drop_status;
    asx_status owner_status;
    asx_obligation_info before;
    asx_obligation_info after;
    uint64_t trace_before;
    uint64_t trace_after;
    int inspected;
} authority_probe;

static asx_status poll_authority_owner(void *ud, asx_task_id self) {
    authority_probe *p = (authority_probe *)ud;
    if (p->obligation == ASX_INVALID_ID) {
        asx_status st = asx_obligation_reserve(p->region, &p->obligation);
        if (st != ASX_OK) return st;
    }
    if (!p->inspected) return asx_task_park(self) == ASX_OK ? ASX_E_PENDING : ASX_E_INVALID_STATE;
    p->owner_status = asx_obligation_commit(p->obligation);
    return ASX_OK; /* the test asserts the observed resolution separately */
}

static asx_status poll_authority_thief(void *ud, asx_task_id self) {
    authority_probe *p = (authority_probe *)ud;
    asx_status st;
    (void)self;
    if (p->obligation == ASX_INVALID_ID) return ASX_E_PENDING;
    st = asx_obligation_get_info(p->obligation, &p->before);
    if (st != ASX_OK) return st;
    p->trace_before = asx_trace_digest();
    p->commit_status = asx_obligation_commit(p->obligation);
    p->abort_status = asx_obligation_abort(p->obligation);
    p->drop_status = asx_obligation_drop(p->obligation);
    p->trace_after = asx_trace_digest();
    st = asx_obligation_get_info(p->obligation, &p->after);
    if (st != ASX_OK) return st;
    p->inspected = 1;
    return asx_task_wake(p->owner);
}

TEST(non_holder_cannot_resolve_an_obligation_but_its_owner_can) {
    authority_probe p;
    asx_task_id thief;
    asx_budget run;

    setup();
    memset(&p, 0, sizeof(p));
    p.owner_status = ASX_E_PENDING;
    ASSERT_EQ(asx_region_open(&p.region), ASX_OK);
    ASSERT_EQ(asx_task_spawn(p.region, poll_authority_owner, &p, &p.owner), ASX_OK);
    ASSERT_EQ(asx_task_spawn(p.region, poll_authority_thief, &p, &thief), ASX_OK);
    run = asx_budget_from_polls(20u);
    ASSERT_EQ(asx_scheduler_run(p.region, &run), ASX_OK);
    ASSERT_EQ(p.commit_status, ASX_E_PERMISSION_DENIED);
    ASSERT_EQ(p.abort_status, ASX_E_PERMISSION_DENIED);
    ASSERT_EQ(p.drop_status, ASX_E_PERMISSION_DENIED);
    ASSERT_EQ(p.before.state, ASX_OBLIGATION_RESERVED);
    ASSERT_EQ(p.after.state, ASX_OBLIGATION_RESERVED);
    ASSERT_EQ(p.before.holder, p.after.holder);
    ASSERT_EQ(p.after.holder, p.owner);
    ASSERT_EQ(p.before.region, p.after.region);
    ASSERT_EQ(p.trace_before, p.trace_after);
    ASSERT_EQ(p.owner_status, ASX_OK);
    ASSERT_EQ(asx_obligation_leak_count(), UINT64_C(0));
}

typedef struct {
    asx_region_id source_region;
    asx_region_id destination_region;
    asx_task_id destination;
    asx_obligation_id obligation;
    asx_status transfer_status;
    asx_status source_status;
    asx_status source_abort_status;
    asx_status source_drop_status;
    asx_status exhausted_status;
    asx_status destination_status;
    asx_status destination_twice;
    asx_obligation_info before;
    asx_obligation_info after;
    uint64_t trace_before;
    uint64_t trace_after;
    uint64_t denial_trace_before;
    uint64_t denial_trace_after;
    uint64_t denial_total_before;
    uint64_t denial_total_after;
    uint32_t count_before;
    uint32_t count_after;
    int probe_full_arena;
    int hold_blocker; /* the destination task fills its region's quota first */
    asx_obligation_id blocker;
    asx_status blocker_status;
    int transferred;
} handoff_probe;

static struct {
    asx_trace_event_kind order[8];
    uint32_t count;
    asx_obligation_info reservation;
    asx_trace_handoff handoff;
    int missing_metadata;
} g_handoff_trace;

static void capture_handoff_trace(void *ctx, const asx_trace_event *event,
                                  const asx_trace_payload *payload) {
    (void)ctx;
    if (event->kind != ASX_TRACE_OBLIGATION_RESERVE &&
        event->kind != ASX_TRACE_OBLIGATION_HANDOFF && event->kind != ASX_TRACE_OBLIGATION_COMMIT &&
        event->kind != ASX_TRACE_USER) {
        return;
    }
    if (g_handoff_trace.count < 8u) g_handoff_trace.order[g_handoff_trace.count++] = event->kind;
    if (event->kind == ASX_TRACE_OBLIGATION_RESERVE) {
        if (payload->obligation != NULL) {
            g_handoff_trace.reservation = *payload->obligation;
        } else if (asx_obligation_get_info((asx_obligation_id)event->entity_id,
                                           &g_handoff_trace.reservation) != ASX_OK) {
            g_handoff_trace.missing_metadata = 1;
        }
    } else if (event->kind == ASX_TRACE_OBLIGATION_HANDOFF) {
        if (payload->handoff == NULL)
            g_handoff_trace.missing_metadata = 1;
        else
            g_handoff_trace.handoff = *payload->handoff;
    }
}

static void check_raw_refusals(asx_obligation_id id, asx_region_id source,
                               asx_region_id destination, asx_task_id target);

static asx_status poll_handoff_source(void *ud, asx_task_id self) {
    handoff_probe *p = (handoff_probe *)ud;
    asx_status st = asx_obligation_reserve(p->source_region, &p->obligation);
    (void)self;
    if (st != ASX_OK) return st;
    st = asx_obligation_get_info(p->obligation, &p->before);
    if (st != ASX_OK) return st;
    if (p->probe_full_arena) {
        asx_obligation_id extra;
        p->exhausted_status = asx_obligation_reserve(p->source_region, &extra);
    }
    p->count_before = asx_runtime_obligation_count(&g_rt);
    p->trace_before = asx_trace_digest();
    p->transfer_status = asx_obligation_handoff(p->obligation, p->destination);
    p->trace_after = asx_trace_digest();
    st = asx_obligation_get_info(p->obligation, &p->after);
    if (st != ASX_OK) return st;
    p->count_after = asx_runtime_obligation_count(&g_rt);
    p->denial_trace_before = asx_trace_digest();
    p->denial_total_before = asx_trace_emitted_total();
    p->transferred = p->transfer_status == ASX_OK;
    if (p->transferred) {
        check_raw_refusals(p->obligation, p->source_region, p->destination_region, p->destination);
        if (test_current_failed) return ASX_E_INVALID_STATE;
    }
    p->source_status = asx_obligation_commit(p->obligation);
    if (p->transferred) {
        p->source_abort_status = asx_obligation_abort(p->obligation);
        p->source_drop_status = asx_obligation_drop(p->obligation);
        p->denial_trace_after = asx_trace_digest();
        p->denial_total_after = asx_trace_emitted_total();
    }
    if (p->transferred) asx_trace_user(self, "handoff-after-transfer");
    return asx_task_wake(p->destination);
}

static asx_status poll_handoff_destination(void *ud, asx_task_id self) {
    handoff_probe *p = (handoff_probe *)ud;
    if (p->hold_blocker && p->blocker == ASX_INVALID_ID) {
        /* Fills the region's quota with a credit this task holds. */
        asx_status st = asx_obligation_reserve(p->destination_region, &p->blocker);
        if (st != ASX_OK) return st;
    }
    if (p->obligation == ASX_INVALID_ID) {
        return asx_task_park(self) == ASX_OK ? ASX_E_PENDING : ASX_E_INVALID_STATE;
    }
    if (!p->transferred) {
        if (p->blocker != ASX_INVALID_ID) p->blocker_status = asx_obligation_abort(p->blocker);
        return ASX_OK;
    }
    p->destination_status = asx_obligation_commit(p->obligation);
    p->destination_twice = asx_obligation_commit(p->obligation);
    return ASX_OK;
}

static void check_handoff(int different_region, int full_destination, int lab) {
    handoff_probe p;
    asx_region_id root;
    asx_task_id source;
    asx_region_limits limits;
    asx_budget run;
    asx_status run_status;

    setup();
    if (lab) ASSERT_EQ(asx_scheduler_use_lab_dispatch(42u), ASX_OK);
    memset(&p, 0, sizeof(p));
    p.destination_status = ASX_E_PENDING;
    p.blocker = ASX_INVALID_ID;
    p.hold_blocker = full_destination;
    ASSERT_EQ(asx_region_open(&root), ASX_OK);
    ASSERT_EQ(asx_region_open_child(root, &p.source_region), ASX_OK);
    if (different_region) {
        ASSERT_EQ(asx_region_open_child(root, &p.destination_region), ASX_OK);
    } else {
        p.destination_region = p.source_region;
    }
    ASSERT_EQ(asx_region_get_limits(p.destination_region, &limits), ASX_OK);
    limits.max_obligations = 1u;
    ASSERT_EQ(asx_region_set_limits(p.destination_region, &limits), ASX_OK);
    /* The destination is spawned first when it holds the blocker, so the
     * quota is full before the source's poll hands off. */
    if (full_destination) {
        ASSERT_EQ(asx_task_spawn(p.destination_region, poll_handoff_destination, &p,
                                 &p.destination),
                  ASX_OK);
    }
    ASSERT_EQ(asx_task_spawn(p.source_region, poll_handoff_source, &p, &source), ASX_OK);
    if (!full_destination) {
        ASSERT_EQ(asx_task_spawn(p.destination_region, poll_handoff_destination, &p,
                                 &p.destination),
                  ASX_OK);
    }
    run = asx_budget_from_polls(20u);
    memset(&g_handoff_trace, 0, sizeof(g_handoff_trace));
    asx_trace_set_observer(capture_handoff_trace, NULL);
    run_status = asx_scheduler_run(root, &run);
    asx_trace_set_observer(NULL, NULL);
    ASSERT_EQ(run_status, ASX_OK);
    if (full_destination) {
        ASSERT_EQ(p.transfer_status, ASX_E_ADMISSION_LIMIT);
        ASSERT_EQ(p.source_status, ASX_OK);
        ASSERT_EQ(p.before.holder, p.after.holder);
        ASSERT_EQ(p.before.region, p.after.region);
        ASSERT_EQ(p.before.state, p.after.state);
        ASSERT_EQ(p.trace_before, p.trace_after);
        ASSERT_EQ(p.destination_status, ASX_E_PENDING);
        /* The blocker's holder released it when it found nothing moved. */
        ASSERT_EQ(p.blocker_status, ASX_OK);
    } else {
        ASSERT_EQ(p.transfer_status, ASX_OK);
        ASSERT_EQ(p.source_status, ASX_E_PERMISSION_DENIED);
        ASSERT_EQ(p.source_abort_status, ASX_E_PERMISSION_DENIED);
        ASSERT_EQ(p.source_drop_status, ASX_E_PERMISSION_DENIED);
        ASSERT_EQ(p.denial_trace_before, p.denial_trace_after);
        ASSERT_EQ(p.denial_total_before, p.denial_total_after);
        ASSERT_EQ(asx_handle_index(p.after.holder), asx_handle_index(p.destination));
        ASSERT_EQ(asx_handle_index(p.after.region), asx_handle_index(p.destination_region));
        ASSERT_EQ(p.destination_status, ASX_OK);
        ASSERT_EQ(p.destination_twice, ASX_E_OBLIGATION_ALREADY_RESOLVED);
        ASSERT_EQ(asx_trace_kind_total(ASX_TRACE_OBLIGATION_HANDOFF), UINT64_C(1));
        ASSERT_EQ(g_handoff_trace.missing_metadata, 0);
        ASSERT_EQ(g_handoff_trace.count, 4u);
        ASSERT_EQ(asx_handle_index(g_handoff_trace.reservation.holder), asx_handle_index(source));
        ASSERT_EQ(asx_handle_index(g_handoff_trace.reservation.region),
                  asx_handle_index(p.source_region));
        ASSERT_EQ(asx_handle_index(g_handoff_trace.handoff.from_task), asx_handle_index(source));
        ASSERT_EQ(asx_handle_index(g_handoff_trace.handoff.to_task),
                  asx_handle_index(p.destination));
        if (lab) {
            ASSERT_EQ(g_handoff_trace.order[0], ASX_TRACE_USER);
            ASSERT_EQ(g_handoff_trace.order[1], ASX_TRACE_OBLIGATION_RESERVE);
            ASSERT_EQ(g_handoff_trace.order[2], ASX_TRACE_OBLIGATION_HANDOFF);
        } else {
            ASSERT_EQ(g_handoff_trace.order[0], ASX_TRACE_OBLIGATION_RESERVE);
            ASSERT_EQ(g_handoff_trace.order[1], ASX_TRACE_OBLIGATION_HANDOFF);
            ASSERT_EQ(g_handoff_trace.order[2], ASX_TRACE_USER);
        }
        ASSERT_EQ(g_handoff_trace.order[3], ASX_TRACE_OBLIGATION_COMMIT);
    }
    ASSERT_EQ(p.count_before, p.count_after);
    ASSERT_EQ(asx_obligation_leak_count(), UINT64_C(0));
    run = asx_budget_from_polls(20u);
    ASSERT_EQ(asx_region_drain(root, &run), ASX_OK);
}

TEST(handoff_same_region_reuses_the_full_quota) { check_handoff(0, 0, 0); }
TEST(handoff_cross_region_moves_the_credit_and_holder) { check_handoff(1, 0, 0); }
TEST(handoff_full_destination_is_failure_atomic) { check_handoff(1, 1, 0); }
TEST(handoff_lab_posts_keep_original_reservation_and_transfer_order) { check_handoff(1, 0, 1); }
TEST(handoff_lab_same_region_reuses_the_full_quota) { check_handoff(0, 0, 1); }

typedef struct {
    asx_obligation_info info;
    asx_drain_progress source;
    asx_drain_progress destination;
    asx_task_state target_state;
    uint32_t count;
    uint64_t emitted;
    uint64_t digest;
} obligation_observation;

static void observe_obligation(asx_obligation_id id, asx_region_id source,
                               asx_region_id destination, asx_task_id target,
                               obligation_observation *out) {
    ASSERT_EQ(asx_obligation_get_info(id, &out->info), ASX_OK);
    ASSERT_EQ(asx_region_drain_progress(source, &out->source), ASX_OK);
    ASSERT_EQ(asx_region_drain_progress(destination, &out->destination), ASX_OK);
    ASSERT_EQ(asx_task_get_state(target, &out->target_state), ASX_OK);
    out->count = asx_runtime_obligation_count(&g_rt);
    out->emitted = asx_trace_emitted_total();
    out->digest = asx_trace_digest();
}

static void check_same_progress(const asx_drain_progress *a, const asx_drain_progress *b) {
    ASSERT_EQ(a->region_state, b->region_state);
    ASSERT_EQ(a->tasks_total, b->tasks_total);
    ASSERT_EQ(a->tasks_live, b->tasks_live);
    ASSERT_EQ(a->tasks_completed, b->tasks_completed);
    ASSERT_EQ(a->tasks_cancelled, b->tasks_cancelled);
    ASSERT_EQ(a->obligations_total, b->obligations_total);
    ASSERT_EQ(a->obligations_reserved, b->obligations_reserved);
    ASSERT_EQ(a->obligations_resolved, b->obligations_resolved);
    ASSERT_EQ(a->cleanup_drained, b->cleanup_drained);
    ASSERT_EQ(a->poisoned, b->poisoned);
}

static void check_same_obligation_observation(const obligation_observation *a,
                                              const obligation_observation *b) {
    ASSERT_EQ(a->info.state, b->info.state);
    ASSERT_EQ(a->info.kind, b->info.kind);
    ASSERT_EQ(a->info.holder, b->info.holder);
    ASSERT_EQ(a->info.region, b->info.region);
    ASSERT_EQ(a->info.abort_reason, b->info.abort_reason);
    ASSERT_EQ(a->target_state, b->target_state);
    ASSERT_EQ(a->count, b->count);
    ASSERT_EQ(a->emitted, b->emitted);
    ASSERT_EQ(a->digest, b->digest);
    check_same_progress(&a->source, &b->source);
    if (test_current_failed) return;
    check_same_progress(&a->destination, &b->destination);
}

typedef enum {
    REFUSE_SAME_HOLDER,
    REFUSE_UNCHECKED_SOURCE,
    REFUSE_INVALID_TARGET,
    REFUSE_WRONG_TYPE_TARGET,
    REFUSE_STALE_TARGET,
    REFUSE_COMPLETED_TARGET,
    REFUSE_CANCELLED_TARGET,
    REFUSE_CLOSING_TARGET,
    REFUSE_POISONED_TARGET,
    REFUSE_ZERO_TARGET_QUOTA,
    REFUSE_FULL_TARGET_QUOTA,
    REFUSE_UNKNOWN_OBLIGATION,
    REFUSE_WRONG_TYPE_OBLIGATION,
    REFUSE_STALE_OBLIGATION,
    REFUSE_RESOLVED_OBLIGATION
} handoff_refusal_kind;

typedef struct {
    asx_region_id source_region;
    asx_region_id destination_region;
    asx_task_id destination;
    asx_task_id stale_destination;
    asx_obligation_id obligation;
    asx_obligation_id stale_obligation;
    asx_obligation_id blocker;
    handoff_refusal_kind kind;
    asx_status expected;
    asx_status cleanup;
    asx_status blocker_status;
    int release_blocker;
    int executed;
} handoff_refusal_probe;

static asx_status poll_park_only(void *ud, asx_task_id self) {
    (void)ud;
    return asx_task_park(self) == ASX_OK ? ASX_E_PENDING : ASX_E_INVALID_STATE;
}

/* The destination of a full-quota refusal: it holds the credit that fills
 * its region's quota, and aborts it once released. */
static asx_status poll_refusal_blocker_holder(void *ud, asx_task_id self) {
    handoff_refusal_probe *p = (handoff_refusal_probe *)ud;
    if (p->blocker == ASX_INVALID_ID) {
        asx_status st = asx_obligation_reserve(p->destination_region, &p->blocker);
        if (st != ASX_OK) return st;
    }
    if (!p->release_blocker) {
        return asx_task_park(self) == ASX_OK ? ASX_E_PENDING : ASX_E_INVALID_STATE;
    }
    p->blocker_status = asx_obligation_abort(p->blocker);
    return ASX_OK;
}

static void check_handoff_refusal_poll(handoff_refusal_probe *p, asx_task_id self) {
    obligation_observation before;
    obligation_observation after;
    asx_task_id target = p->destination;
    asx_obligation_id submitted;
    asx_obligation_info final_info;
    asx_region_limits limits;
    if (p->kind == REFUSE_UNCHECKED_SOURCE) {
        ASSERT_EQ(asx_obligation_register(p->source_region, ASX_OBLIGATION_KIND_LEASE, self,
                                          &p->obligation),
                  ASX_OK);
    } else {
        ASSERT_EQ(asx_obligation_reserve(p->source_region, &p->obligation), ASX_OK);
    }
    submitted = p->obligation;
    switch (p->kind) {
    case REFUSE_SAME_HOLDER: target = self; break;
    case REFUSE_INVALID_TARGET: target = ASX_INVALID_ID; break;
    case REFUSE_WRONG_TYPE_TARGET: target = (asx_task_id)p->destination_region; break;
    case REFUSE_STALE_TARGET: target = p->stale_destination; break;
    case REFUSE_CANCELLED_TARGET:
        ASSERT_EQ(asx_task_cancel(p->destination, ASX_CANCEL_USER), ASX_OK);
        break;
    case REFUSE_CLOSING_TARGET: ASSERT_EQ(asx_region_close(p->destination_region), ASX_OK); break;
    case REFUSE_POISONED_TARGET: ASSERT_EQ(asx_region_poison(p->destination_region), ASX_OK); break;
    case REFUSE_ZERO_TARGET_QUOTA:
        ASSERT_EQ(asx_region_get_limits(p->destination_region, &limits), ASX_OK);
        limits.max_obligations = 0u;
        ASSERT_EQ(asx_region_set_limits(p->destination_region, &limits), ASX_OK);
        break;
    case REFUSE_UNKNOWN_OBLIGATION: submitted = ASX_INVALID_ID; break;
    case REFUSE_WRONG_TYPE_OBLIGATION: submitted = (asx_obligation_id)self; break;
    case REFUSE_STALE_OBLIGATION:
        submitted = p->stale_obligation;
        ASSERT_EQ(asx_handle_slot(submitted), asx_handle_slot(p->obligation));
        ASSERT_NE(asx_handle_index(submitted), asx_handle_index(p->obligation));
        break;
    case REFUSE_RESOLVED_OBLIGATION: ASSERT_EQ(asx_obligation_commit(p->obligation), ASX_OK); break;
    case REFUSE_UNCHECKED_SOURCE:
    case REFUSE_COMPLETED_TARGET:
    case REFUSE_FULL_TARGET_QUOTA: break;
    }
    observe_obligation(p->obligation, p->source_region, p->destination_region, p->destination,
                       &before);
    if (test_current_failed) return;
    ASSERT_EQ(asx_obligation_handoff(submitted, target), p->expected);
    observe_obligation(p->obligation, p->source_region, p->destination_region, p->destination,
                       &after);
    if (test_current_failed) return;
    check_same_obligation_observation(&before, &after);
    if (test_current_failed) return;
    p->cleanup =
        p->kind == REFUSE_RESOLVED_OBLIGATION ? ASX_OK : asx_obligation_commit(p->obligation);
    ASSERT_EQ(p->cleanup, ASX_OK);
    ASSERT_EQ(asx_obligation_get_info(p->obligation, &final_info), ASX_OK);
    ASSERT_EQ(final_info.state, ASX_OBLIGATION_COMMITTED);
    p->executed = 1;
}

static asx_status poll_handoff_refusal(void *ud, asx_task_id self) {
    check_handoff_refusal_poll((handoff_refusal_probe *)ud, self);
    return test_current_failed ? ASX_E_INVALID_STATE : ASX_OK;
}

/* Fills the obligation arena with resolved records from one task's poll,
 * keeping the first record's handle. */
typedef struct {
    asx_region_id region;
    asx_obligation_id first;
} arena_fill;

static void fill_arena_with_resolved(void *ctx) {
    arena_fill *f = (arena_fill *)ctx;
    uint32_t i;
    for (i = 0u; i < ASX_MAX_OBLIGATIONS; i++) {
        asx_obligation_id resolved;
        ASSERT_EQ(asx_obligation_reserve(f->region, &resolved), ASX_OK);
        if (i == 0u) f->first = resolved;
        ASSERT_EQ(asx_obligation_commit(resolved), ASX_OK);
    }
}

static void run_handoff_refusal(handoff_refusal_kind kind, asx_status expected) {
    handoff_refusal_probe p;
    asx_region_id root;
    asx_region_id old_region;
    asx_task_id source;
    asx_budget run;
    asx_region_limits limits;
    setup();
    memset(&p, 0, sizeof(p));
    p.kind = kind;
    p.expected = expected;
    p.cleanup = ASX_E_PENDING;
    p.blocker = ASX_INVALID_ID;
    p.blocker_status = ASX_E_PENDING;
    if (kind == REFUSE_STALE_OBLIGATION) {
        arena_fill fill;
        ASSERT_EQ(asx_region_open(&old_region), ASX_OK);
        /* Fill the arena with resolved records. The real owner's next
         * reservation reclaims and reuses slot zero without resetting. */
        fill.region = old_region;
        fill.first = ASX_INVALID_ID;
        ASSERT_EQ(test_run_in_task(old_region, fill_arena_with_resolved, &fill), ASX_OK);
        p.stale_obligation = fill.first;
        ASSERT_EQ(asx_runtime_obligation_count(&g_rt), ASX_MAX_OBLIGATIONS);
    }
    ASSERT_EQ(asx_region_open(&root), ASX_OK);
    ASSERT_EQ(asx_region_open_child(root, &p.source_region), ASX_OK);
    ASSERT_EQ(asx_region_open_child(root, &p.destination_region), ASX_OK);
    if (kind == REFUSE_FULL_TARGET_QUOTA) {
        ASSERT_EQ(asx_region_get_limits(p.destination_region, &limits), ASX_OK);
        limits.max_obligations = 1u;
        ASSERT_EQ(asx_region_set_limits(p.destination_region, &limits), ASX_OK);
        /* The destination task holds the credit that fills the quota; it
         * reserves it on its first poll and parks until released. */
        ASSERT_EQ(asx_task_spawn(p.destination_region, poll_refusal_blocker_holder, &p,
                                 &p.destination),
                  ASX_OK);
        run = asx_budget_from_polls(8u);
        ASSERT_EQ(asx_scheduler_run(p.destination_region, &run), ASX_E_WOULD_BLOCK);
        ASSERT_NE(p.blocker, ASX_INVALID_ID);
    } else if (kind == REFUSE_STALE_TARGET || kind == REFUSE_COMPLETED_TARGET) {
        ASSERT_EQ(asx_task_spawn(p.destination_region, poll_complete, NULL, &p.destination),
                  ASX_OK);
        run = asx_budget_from_polls(8u);
        ASSERT_EQ(asx_scheduler_run(p.destination_region, &run), ASX_OK);
        if (kind == REFUSE_STALE_TARGET) {
            p.stale_destination = p.destination;
            ASSERT_EQ(asx_task_join(p.destination, NULL), ASX_OK);
            ASSERT_EQ(asx_task_spawn(p.destination_region, poll_park_only, NULL, &p.destination),
                      ASX_OK);
            ASSERT_EQ(asx_handle_slot(p.destination), asx_handle_slot(p.stale_destination));
            ASSERT_NE(asx_handle_index(p.destination), asx_handle_index(p.stale_destination));
        }
    } else {
        ASSERT_EQ(asx_task_spawn(p.destination_region, poll_park_only, NULL, &p.destination),
                  ASX_OK);
    }
    ASSERT_EQ(asx_task_spawn(p.source_region, poll_handoff_refusal, &p, &source), ASX_OK);
    run = asx_budget_from_polls(8u);
    ASSERT_EQ(asx_scheduler_run(p.source_region, &run), ASX_OK);
    ASSERT_TRUE(p.executed);
    ASSERT_EQ(p.cleanup, ASX_OK);
    if (p.blocker != ASX_INVALID_ID) {
        /* Its holder's own poll releases the credit. */
        p.release_blocker = 1;
        ASSERT_EQ(asx_task_wake(p.destination), ASX_OK);
        run = asx_budget_from_polls(8u);
        ASSERT_EQ(asx_scheduler_run(p.destination_region, &run), ASX_OK);
        ASSERT_EQ(p.blocker_status, ASX_OK);
    }
    ASSERT_EQ(asx_obligation_leak_count(), UINT64_C(0));
    /* The deliberately invalid/poisoned destination fixture is torn down;
     * the source credit was settled by its actual owner's poll. */
    asx_runtime_shutdown(&g_rt);
}

TEST(handoff_same_holder_is_failure_atomic) {
    run_handoff_refusal(REFUSE_SAME_HOLDER, ASX_E_INVALID_ARGUMENT);
}
TEST(handoff_unchecked_source_is_failure_atomic) {
    run_handoff_refusal(REFUSE_UNCHECKED_SOURCE, ASX_E_INVALID_STATE);
}
TEST(handoff_invalid_destination_is_failure_atomic) {
    run_handoff_refusal(REFUSE_INVALID_TARGET, ASX_E_NOT_FOUND);
}
TEST(handoff_wrong_type_destination_is_failure_atomic) {
    run_handoff_refusal(REFUSE_WRONG_TYPE_TARGET, ASX_E_NOT_FOUND);
}
TEST(handoff_stale_reused_destination_is_failure_atomic) {
    run_handoff_refusal(REFUSE_STALE_TARGET, ASX_E_STALE_HANDLE);
}
TEST(handoff_completed_destination_is_failure_atomic) {
    run_handoff_refusal(REFUSE_COMPLETED_TARGET, ASX_E_INVALID_STATE);
}
TEST(handoff_cancelled_destination_is_failure_atomic) {
    run_handoff_refusal(REFUSE_CANCELLED_TARGET, ASX_E_CANCELLED);
}
TEST(handoff_closing_destination_is_failure_atomic) {
    run_handoff_refusal(REFUSE_CLOSING_TARGET, ASX_E_REGION_CLOSED);
}
TEST(handoff_poisoned_destination_is_failure_atomic) {
    run_handoff_refusal(REFUSE_POISONED_TARGET, ASX_E_REGION_POISONED);
}
TEST(handoff_zero_destination_quota_is_failure_atomic) {
    run_handoff_refusal(REFUSE_ZERO_TARGET_QUOTA, ASX_E_ADMISSION_LIMIT);
}
TEST(handoff_full_destination_preserves_both_region_counts) {
    run_handoff_refusal(REFUSE_FULL_TARGET_QUOTA, ASX_E_ADMISSION_LIMIT);
}
TEST(handoff_unknown_obligation_is_failure_atomic) {
    run_handoff_refusal(REFUSE_UNKNOWN_OBLIGATION, ASX_E_NOT_FOUND);
}
TEST(handoff_wrong_type_obligation_is_failure_atomic) {
    run_handoff_refusal(REFUSE_WRONG_TYPE_OBLIGATION, ASX_E_NOT_FOUND);
}
TEST(handoff_stale_reused_obligation_is_failure_atomic) {
    run_handoff_refusal(REFUSE_STALE_OBLIGATION, ASX_E_STALE_HANDLE);
}
TEST(handoff_resolved_obligation_is_failure_atomic) {
    run_handoff_refusal(REFUSE_RESOLVED_OBLIGATION, ASX_E_OBLIGATION_ALREADY_RESOLVED);
}

static void check_raw_refusals(asx_obligation_id id, asx_region_id source,
                               asx_region_id destination, asx_task_id target) {
    obligation_observation before;
    obligation_observation after;
    observe_obligation(id, source, destination, target, &before);
    if (test_current_failed) return;
    ASSERT_EQ(asx_obligation_commit(id), ASX_E_PERMISSION_DENIED);
    observe_obligation(id, source, destination, target, &after);
    if (test_current_failed) return;
    check_same_obligation_observation(&before, &after);
    if (test_current_failed) return;
    ASSERT_EQ(asx_obligation_abort(id), ASX_E_PERMISSION_DENIED);
    observe_obligation(id, source, destination, target, &after);
    if (test_current_failed) return;
    check_same_obligation_observation(&before, &after);
    if (test_current_failed) return;
    ASSERT_EQ(asx_obligation_drop(id), ASX_E_PERMISSION_DENIED);
    observe_obligation(id, source, destination, target, &after);
    if (test_current_failed) return;
    check_same_obligation_observation(&before, &after);
}

TEST(host_raw_resolution_of_a_held_credit_is_failure_atomic) {
    authority_probe p;
    asx_budget run;
    setup();
    memset(&p, 0, sizeof(p));
    p.owner_status = ASX_E_PENDING;
    ASSERT_EQ(asx_region_open(&p.region), ASX_OK);
    ASSERT_EQ(asx_task_spawn(p.region, poll_authority_owner, &p, &p.owner), ASX_OK);
    run = asx_budget_from_polls(8u);
    ASSERT_EQ(asx_scheduler_run(p.region, &run), ASX_E_WOULD_BLOCK);
    check_raw_refusals(p.obligation, p.region, p.region, p.owner);
    if (test_current_failed) return;
    p.inspected = 1;
    ASSERT_EQ(asx_task_wake(p.owner), ASX_OK);
    run = asx_budget_from_polls(8u);
    ASSERT_EQ(asx_scheduler_run(p.region, &run), ASX_OK);
    ASSERT_EQ(p.owner_status, ASX_OK);
    ASSERT_EQ(asx_obligation_leak_count(), UINT64_C(0));
}

typedef struct {
    asx_obligation_id obligation;
    asx_region_id source_region;
    asx_region_id caller_region;
    asx_status holder_status;
    int executed;
} wrong_region_probe;

static asx_status poll_wrong_region_refusals(void *ud, asx_task_id self) {
    wrong_region_probe *p = (wrong_region_probe *)ud;
    check_raw_refusals(p->obligation, p->source_region, p->caller_region, self);
    if (test_current_failed) return ASX_E_INVALID_STATE;
    p->executed = 1;
    return ASX_OK;
}

/* The credit's holder, in its own region: commits it in its poll. */
static asx_status poll_wrong_region_holder(void *ud, asx_task_id self) {
    wrong_region_probe *p = (wrong_region_probe *)ud;
    (void)self;
    p->holder_status = asx_obligation_commit(p->obligation);
    return ASX_OK;
}

TEST(task_raw_resolution_of_another_regions_credit_is_failure_atomic) {
    wrong_region_probe p;
    asx_task_id task;
    asx_task_id holder;
    asx_budget run;
    asx_obligation_info info;
    setup();
    memset(&p, 0, sizeof(p));
    p.holder_status = ASX_E_PENDING;
    ASSERT_EQ(asx_region_open(&p.source_region), ASX_OK);
    ASSERT_EQ(asx_region_open(&p.caller_region), ASX_OK);
    ASSERT_EQ(asx_task_spawn(p.source_region, poll_wrong_region_holder, &p, &holder), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve_ex(p.source_region, ASX_OBLIGATION_KIND_GENERIC, holder,
                                        &p.obligation),
              ASX_OK);
    /* A task of another region, neither holder nor owner, is refused. */
    ASSERT_EQ(asx_task_spawn(p.caller_region, poll_wrong_region_refusals, &p, &task), ASX_OK);
    run = asx_budget_from_polls(8u);
    ASSERT_EQ(asx_scheduler_run(p.caller_region, &run), ASX_OK);
    ASSERT_TRUE(p.executed);
    ASSERT_EQ(asx_obligation_get_info(p.obligation, &info), ASX_OK);
    ASSERT_EQ(info.state, ASX_OBLIGATION_RESERVED);
    /* Its holder settles it from its own poll. */
    run = asx_budget_from_polls(8u);
    ASSERT_EQ(asx_scheduler_run(p.source_region, &run), ASX_OK);
    ASSERT_EQ(p.holder_status, ASX_OK);
    ASSERT_EQ(asx_obligation_get_info(p.obligation, &info), ASX_OK);
    ASSERT_EQ(info.state, ASX_OBLIGATION_COMMITTED);
    ASSERT_EQ(asx_obligation_leak_count(), UINT64_C(0));
}

/* Holds all but one slot of the obligation arena from one poll, parks, and
 * aborts them all once released. */
typedef struct {
    asx_region_id region;
    asx_obligation_id blockers[ASX_MAX_OBLIGATIONS];
    asx_status status;
    int reserved;
    int release;
} arena_holder;

static asx_status poll_arena_holder(void *ud, asx_task_id self) {
    arena_holder *h = (arena_holder *)ud;
    uint32_t i;
    if (!h->reserved) {
        for (i = 0u; i + 1u < ASX_MAX_OBLIGATIONS; i++) {
            asx_status st = asx_obligation_reserve(h->region, &h->blockers[i]);
            if (st != ASX_OK) return st;
        }
        h->reserved = 1;
    }
    if (!h->release) return asx_task_park(self) == ASX_OK ? ASX_E_PENDING : ASX_E_INVALID_STATE;
    for (i = 0u; i + 1u < ASX_MAX_OBLIGATIONS; i++) {
        asx_status st = asx_obligation_abort(h->blockers[i]);
        if (st != ASX_OK) h->status = st;
    }
    return ASX_OK;
}

TEST(handoff_uses_no_extra_slot_when_the_obligation_arena_is_full) {
    static arena_holder holder;
    handoff_probe p;
    asx_region_id root;
    asx_task_id source;
    asx_task_id holder_task;
    asx_budget run;
    setup();
    memset(&p, 0, sizeof(p));
    memset(&holder, 0, sizeof(holder));
    p.probe_full_arena = 1;
    p.blocker = ASX_INVALID_ID;
    ASSERT_EQ(asx_region_open(&root), ASX_OK);
    ASSERT_EQ(asx_region_open_child(root, &p.source_region), ASX_OK);
    ASSERT_EQ(asx_region_open_child(root, &p.destination_region), ASX_OK);
    /* Spawned first, so its reservations fill the arena before the source
     * reserves the last slot. */
    holder.region = root;
    ASSERT_EQ(asx_task_spawn(root, poll_arena_holder, &holder, &holder_task), ASX_OK);
    ASSERT_EQ(asx_task_spawn(p.source_region, poll_handoff_source, &p, &source), ASX_OK);
    ASSERT_EQ(asx_task_spawn(p.destination_region, poll_handoff_destination, &p, &p.destination),
              ASX_OK);
    run = asx_budget_from_polls(8u);
    ASSERT_EQ(asx_scheduler_run(root, &run), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(p.exhausted_status, ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(p.count_before, ASX_MAX_OBLIGATIONS);
    ASSERT_EQ(p.count_after, p.count_before);
    ASSERT_EQ(p.transfer_status, ASX_OK);
    ASSERT_EQ(p.source_status, ASX_E_PERMISSION_DENIED);
    ASSERT_EQ(p.source_abort_status, ASX_E_PERMISSION_DENIED);
    ASSERT_EQ(p.source_drop_status, ASX_E_PERMISSION_DENIED);
    ASSERT_EQ(p.denial_trace_before, p.denial_trace_after);
    ASSERT_EQ(p.denial_total_before, p.denial_total_after);
    ASSERT_EQ(p.destination_status, ASX_OK);
    ASSERT_EQ(p.destination_twice, ASX_E_OBLIGATION_ALREADY_RESOLVED);
    /* The blockers' holder aborts them in its own poll. */
    holder.release = 1;
    ASSERT_EQ(asx_task_wake(holder_task), ASX_OK);
    run = asx_budget_from_polls(8u);
    ASSERT_EQ(asx_scheduler_run(root, &run), ASX_OK);
    ASSERT_EQ(holder.status, ASX_OK);
    ASSERT_EQ(asx_obligation_leak_count(), UINT64_C(0));
    run = asx_budget_from_polls(8u);
    ASSERT_EQ(asx_region_drain(root, &run), ASX_OK);
}

/* -------------------------------------------------------------------
 * Budgets
 * ------------------------------------------------------------------- */

TEST(deadline_cancels_parked_task_on_virtual_time) {
    asx_region_id r;
    asx_task_id t;
    probe_state p;
    asx_budget tb;
    asx_budget run;
    asx_outcome out;

    setup();
    memset(&p, 0, sizeof(p));
    p.park = 1;
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    tb = asx_budget_infinite();
    tb.deadline = asx_runtime_virtual_now() + 5u * MS;
    ASSERT_EQ(asx_task_spawn_with_budget(r, poll_probe, &p, &tb, &t), ASX_OK);

    /* Without the deadline the parked task would block forever; the
     * deadline timer wakes it and the scheduler cancels it (DEADLINE). */
    run = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);
    ASSERT_TRUE(p.saw_cancel);
    ASSERT_EQ(p.kind, ASX_CANCEL_DEADLINE);
    ASSERT_EQ(p.polls, 2u);
    ASSERT_TRUE(asx_runtime_virtual_now() >= tb.deadline);
    ASSERT_EQ(asx_task_get_outcome(t, &out), ASX_OK);
    ASSERT_EQ(asx_outcome_severity_of(&out), ASX_OUTCOME_CANCELLED);
}

TEST(expired_deadline_cancels_before_first_poll) {
    asx_region_id r;
    asx_task_id t;
    probe_state p;
    asx_budget tb;
    asx_budget run;

    setup();
    memset(&p, 0, sizeof(p));
    asx_runtime_virtual_advance(10u * MS);
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    tb = asx_budget_infinite();
    tb.deadline = 1u * MS;
    ASSERT_EQ(asx_task_spawn_with_budget(r, poll_probe, &p, &tb, &t), ASX_OK);
    run = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);
    ASSERT_EQ(p.polls, 1u);
    ASSERT_TRUE(p.saw_cancel);
    ASSERT_EQ(p.kind, ASX_CANCEL_DEADLINE);
}

TEST(poll_quota_cancels_after_quota_polls) {
    asx_region_id r;
    asx_task_id t;
    probe_state p;
    asx_budget tb;
    asx_budget run;
    asx_budget left;

    setup();
    memset(&p, 0, sizeof(p));
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    tb = asx_budget_from_polls(3);
    ASSERT_EQ(asx_task_spawn_with_budget(r, poll_probe, &p, &tb, &t), ASX_OK);
    run = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);
    /* Three budgeted polls: the third spends the last unit before it runs,
     * so its own checkpoint observes the exhausted quota (Rust charges at
     * poll start and Cx::checkpoint reports poll_quota == 0). */
    ASSERT_EQ(p.polls, 3u);
    ASSERT_TRUE(p.saw_cancel);
    ASSERT_EQ(p.kind, ASX_CANCEL_POLL_QUOTA);
    ASSERT_EQ(asx_task_get_budget(t, &left), ASX_OK);
    ASSERT_EQ(left.poll_quota, 0u);
}

TEST(unbounded_poll_quota_is_not_consumed) {
    asx_region_id r;
    asx_task_id t;
    asx_budget run;
    asx_budget left;

    setup();
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_complete, NULL, &t), ASX_OK);
    run = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);
    ASSERT_EQ(asx_task_get_budget(t, &left), ASX_OK);
    ASSERT_EQ(left.poll_quota, UINT32_MAX);
    ASSERT_EQ(left.deadline, ASX_TIME_INFINITE);
}

typedef struct {
    uint32_t polls;
    asx_cancel_kind self_cancel; /* cancel kind it requests on poll 1 */
    int checkpoint;              /* checkpoint on poll 2 (else read the reason) */
    asx_cancel_kind kind;        /* kind seen on poll 2 (the Cx's) */
    asx_cancel_kind record_kind; /* the record's on poll 2, without a checkpoint */
    int cancelled;               /* the poll-2 checkpoint reported a cancel */
} spent_state;

/* Poll 1: requests its own cancel (no checkpoint) and yields. Poll 2:
 * checkpoints, or only reads its cancel reason, and completes. */
static asx_status poll_spent(void *ud, asx_task_id self) {
    spent_state *s = (spent_state *)ud;
    asx_checkpoint_result cp;
    asx_cancel_reason reason;
    s->polls++;
    if (s->polls == 1u) {
        if (asx_task_cancel(self, s->self_cancel) != ASX_OK) return ASX_E_INVALID_STATE;
        return ASX_E_PENDING;
    }
    if (s->checkpoint) {
        if (asx_checkpoint(self, &cp) != ASX_OK) return ASX_E_INVALID_STATE;
        s->cancelled = cp.cancelled;
        s->kind = cp.kind;
    } else {
        if (asx_task_get_cx_cancel_reason_internal(self, &reason) != ASX_OK) {
            return ASX_E_INVALID_STATE;
        }
        s->kind = reason.kind;
        if (asx_task_get_cancel_reason(self, &reason) != ASX_OK) return ASX_E_INVALID_STATE;
        s->record_kind = reason.kind;
    }
    return ASX_OK;
}

static void run_spent(spent_state *s, uint32_t quota) {
    asx_region_id r;
    asx_task_id t;
    asx_budget tb;
    asx_budget run;
    asx_status st;
    setup();
    tb = asx_budget_from_polls(quota);
    run = asx_budget_from_polls(100);
    st = asx_region_open(&r);
    if (st == ASX_OK) st = asx_task_spawn_with_budget(r, poll_spent, s, &tb, &t);
    if (st == ASX_OK) st = asx_scheduler_run(r, &run);
    (void)st;
}

/* A spent poll quota strengthens a weaker cancel already pending, at the
 * checkpoint (Rust Cx::checkpoint, cx.rs:2824-2836) and before a poll (the
 * lab, lab/runtime.rs:4663-4670); fuzz finding gen-1-73 (bd-ij9w). */
TEST(spent_poll_quota_strengthens_pending_cancel_at_checkpoint) {
    spent_state s;
    memset(&s, 0, sizeof(s));
    s.self_cancel = ASX_CANCEL_DEADLINE;
    s.checkpoint = 1;
    /* Poll 2 spends the last unit; its checkpoint sees the quota at 0. */
    run_spent(&s, 2u);
    ASSERT_EQ(s.polls, 2u);
    ASSERT_TRUE(s.cancelled);
    ASSERT_EQ(s.kind, ASX_CANCEL_POLL_QUOTA);
}

TEST(spent_poll_quota_strengthens_pending_cancel_before_poll) {
    /* The lab's check strengthens the Cx's reason only; the record keeps
     * Deadline until a checkpoint acknowledges (fuzz findings gen-113-192,
     * gen-125-92). */
    spent_state s;
    memset(&s, 0, sizeof(s));
    s.self_cancel = ASX_CANCEL_DEADLINE;
    /* Poll 1 spends the only unit; poll 2 is admitted with none left. */
    run_spent(&s, 1u);
    ASSERT_EQ(s.polls, 2u);
    ASSERT_EQ(s.kind, ASX_CANCEL_POLL_QUOTA);
    ASSERT_EQ(s.record_kind, ASX_CANCEL_DEADLINE);
}

TEST(spent_poll_quota_keeps_a_stronger_pending_cancel) {
    spent_state s;
    memset(&s, 0, sizeof(s));
    s.self_cancel = ASX_CANCEL_SHUTDOWN;
    s.checkpoint = 1;
    run_spent(&s, 2u);
    ASSERT_EQ(s.polls, 2u);
    ASSERT_TRUE(s.cancelled);
    ASSERT_EQ(s.kind, ASX_CANCEL_SHUTDOWN);
}

TEST(cost_quota_spent_to_zero_cancels_at_the_next_checkpoint) {
    asx_region_id r;
    asx_task_id t;
    probe_state p;
    asx_budget tb;
    asx_budget run;

    /* 10 -> 5 -> 0: the checkpoint of the third poll finds the cost quota
     * spent to exactly 0 and cancels with COST_BUDGET (Cx::checkpoint,
     * cx/cx.rs:3145). */
    setup();
    memset(&p, 0, sizeof(p));
    p.cost = 5u;
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    tb = asx_budget_infinite();
    tb.cost_quota = 10u;
    ASSERT_EQ(asx_task_spawn_with_budget(r, poll_probe, &p, &tb, &t), ASX_OK);
    run = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);
    ASSERT_FALSE(p.cost_exhausted);
    ASSERT_TRUE(p.saw_cancel);
    ASSERT_EQ(p.kind, ASX_CANCEL_COST_BUDGET);
    ASSERT_EQ(p.polls, 3u);
}

TEST(cost_charge_the_quota_cannot_cover_is_refused_and_cancels_nothing) {
    asx_region_id r;
    asx_task_id t;
    asx_budget tb;
    asx_budget left;
    asx_cancel_reason reason;

    /* Rust's Budget::consume_cost (types/budget.rs:596-625): refused, the
     * quota unchanged, and no cancel (a later checkpoint finds 2, not 0). */
    setup();
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    tb = asx_budget_infinite();
    tb.cost_quota = 2u;
    ASSERT_EQ(asx_task_spawn_with_budget(r, poll_complete, NULL, &tb, &t), ASX_OK);
    ASSERT_EQ(asx_task_consume_cost(t, 3u), ASX_E_COST_QUOTA_EXHAUSTED);
    ASSERT_EQ(asx_task_get_budget(t, &left), ASX_OK);
    ASSERT_EQ(left.cost_quota, (uint64_t)2u);
    ASSERT_NE(asx_task_get_cancel_reason(t, &reason), ASX_OK);
}

TEST(consume_cost_rejects_stale_handle) {
    setup();
    ASSERT_EQ(asx_task_consume_cost(ASX_INVALID_ID, 1u), ASX_E_NOT_FOUND);
}

/* Poll 1: cancels itself, acknowledges at a checkpoint, spawns child A and
 * yields. Poll 2 (the acknowledgement applied): spawns child B. Records
 * the poll quota each child starts with. */
typedef struct {
    asx_region_id region;
    uint32_t polls;
    uint32_t quota_a;
    uint32_t quota_b;
    int cleanup_only;
} inherit_state;

static asx_status poll_inheritor(void *ud, asx_task_id self) {
    inherit_state *s = (inherit_state *)ud;
    asx_checkpoint_result cr;
    asx_task_id child;
    asx_budget b;
    s->polls++;
    if (s->polls == 1u) {
        if (asx_task_cancel(self, ASX_CANCEL_USER) != ASX_OK) return ASX_E_INVALID_STATE;
        if (asx_checkpoint(self, &cr) != ASX_OK || !cr.cancelled) return ASX_E_INVALID_STATE;
        if ((s->cleanup_only ? asx_task_spawn_cleanup(s->region, poll_complete, NULL, &child)
                             : asx_task_spawn(s->region, poll_complete, NULL, &child)) != ASX_OK) {
            return ASX_E_INVALID_STATE;
        }
        if (asx_task_get_budget(child, &b) == ASX_OK) s->quota_a = b.poll_quota;
        return ASX_E_PENDING;
    }
    if ((s->cleanup_only ? asx_task_spawn_cleanup(s->region, poll_complete, NULL, &child)
                         : asx_task_spawn(s->region, poll_complete, NULL, &child)) != ASX_OK)
        return ASX_E_INVALID_STATE;
    if (asx_task_get_budget(child, &b) == ASX_OK) s->quota_b = b.poll_quota;
    return ASX_OK;
}

/* Rust Cx::inherited_budget (cx.rs:2504-2511): new work inherits the
 * spawner's budget, with no poll quota only once the spawner is in its
 * cleanup phase, which starts when the acknowledgement is applied after
 * the poll, not at the acknowledging checkpoint (fuzz gen-7-54). */
TEST(spawned_child_inherits_the_quota_until_the_cleanup_phase_starts) {
    asx_region_id r;
    asx_task_id t;
    asx_budget tb;
    asx_budget run;
    inherit_state s;

    setup();
    memset(&s, 0, sizeof(s));
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    s.region = r;
    tb = asx_budget_from_polls(5);
    ASSERT_EQ(asx_task_spawn_with_budget(r, poll_inheritor, &s, &tb, &t), ASX_OK);
    run = asx_budget_from_polls(20);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);
    ASSERT_EQ(s.polls, 2u);
    ASSERT_EQ(s.quota_a, 4u); /* 5 minus the poll that spawned it */
    ASSERT_EQ(s.quota_b, UINT32_MAX);
}

TEST(cleanup_spawn_inherits_without_the_parents_cleanup_poll_quota) {
    asx_region_id r;
    asx_task_id t;
    asx_budget tb;
    asx_budget run;
    inherit_state s;
    setup();
    memset(&s, 0, sizeof(s));
    s.cleanup_only = 1;
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    s.region = r;
    tb = asx_budget_from_polls(5);
    ASSERT_EQ(asx_task_spawn_with_budget(r, poll_inheritor, &s, &tb, &t), ASX_OK);
    run = asx_budget_from_polls(20);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);
    ASSERT_EQ(s.polls, 2u);
    ASSERT_EQ(s.quota_a, 4u);
    ASSERT_EQ(s.quota_b, UINT32_MAX);
}

static int budget_equal(const asx_budget *a, const asx_budget *b) {
    return a->deadline == b->deadline && a->poll_quota == b->poll_quota &&
           a->cost_quota == b->cost_quota && a->priority == b->priority;
}

/* Metamorphic M3 (bd-9kll.3.1): a region with budget b opened inside one
 * with budget a holds meet(a, b), the budget of one region opened with
 * meet(a, b), for every pair of the edge budgets (INFINITE, ZERO with its
 * due-at-once deadline 0, NEW, quotas, deadlines, priorities); Rust's
 * region table meets a child's budget with its parent's
 * (runtime/region_table.rs:314). */
TEST(nested_region_budgets_equal_their_meet) {
    asx_budget s[7];
    uint32_t i;
    uint32_t j;
    s[0] = asx_budget_infinite();
    s[1] = asx_budget_zero();
    s[2] = asx_budget_new();
    s[3] = asx_budget_from_polls(7u);
    s[4] = asx_budget_new();
    s[4].deadline = 0u;
    s[4].priority = 3u;
    s[5] = asx_budget_infinite();
    s[5].deadline = 500u;
    s[5].cost_quota = 9u;
    s[5].priority = 250u;
    s[6] = asx_budget_from_polls(3u);
    s[6].deadline = 1000u;
    for (i = 0; i < 7u; i++) {
        for (j = 0; j < 7u; j++) {
            asx_region_id root;
            asx_region_id outer;
            asx_region_id nested;
            asx_region_id single;
            asx_budget met = asx_budget_meet(&s[i], &s[j]);
            asx_budget got_nested;
            asx_budget got_single;
            setup();
            ASSERT_EQ(asx_region_open(&root), ASX_OK);
            ASSERT_EQ(asx_region_open_child_with_budget(root, &s[i], &outer), ASX_OK);
            ASSERT_EQ(asx_region_open_child_with_budget(outer, &s[j], &nested), ASX_OK);
            ASSERT_EQ(asx_region_open_child_with_budget(root, &met, &single), ASX_OK);
            ASSERT_EQ(asx_region_get_budget(nested, &got_nested), ASX_OK);
            ASSERT_EQ(asx_region_get_budget(single, &got_single), ASX_OK);
            ASSERT_TRUE(budget_equal(&got_nested, &met));
            ASSERT_TRUE(budget_equal(&got_single, &met));
        }
    }
}

/* Spawns one child with `lower` and one with `higher`, recording the budget
 * each starts with. */
typedef struct {
    asx_region_id region;
    asx_budget lower;
    asx_budget higher;
    asx_budget got_lower;
    asx_budget got_higher;
    asx_status status;
} budget_spawner_state;

static asx_status poll_budget_spawner(void *ud, asx_task_id self) {
    budget_spawner_state *s = (budget_spawner_state *)ud;
    asx_task_id child;
    (void)self;
    s->status = asx_task_spawn_with_budget(s->region, poll_complete, NULL, &s->lower, &child);
    if (s->status == ASX_OK) s->status = asx_task_get_budget(child, &s->got_lower);
    if (s->status == ASX_OK) {
        s->status = asx_task_spawn_with_budget(s->region, poll_complete, NULL, &s->higher, &child);
    }
    if (s->status == ASX_OK) s->status = asx_task_get_budget(child, &s->got_higher);
    return ASX_OK;
}

/* Rust's cx.spawn_in(&cx.scope_with_budget(b)) (cx.rs:4547-4586): b's
 * deadline and quotas are clamped to the spawner's budget, its priority is
 * taken as given, lower or higher than the spawner's (fuzz finding
 * gen-247-192). */
TEST(spawn_budget_from_a_task_keeps_its_priority_and_clamps_the_rest) {
    asx_region_id r;
    asx_task_id t;
    asx_budget tb;
    asx_budget run;
    budget_spawner_state s;

    setup();
    memset(&s, 0, sizeof(s));
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    s.region = r;
    tb = asx_budget_new(); /* priority 128 */
    tb.deadline = asx_runtime_virtual_now() + 10u * MS;
    tb.poll_quota = 50u;
    s.lower = asx_budget_new();
    s.lower.deadline = asx_runtime_virtual_now() + 20u * MS; /* looser */
    s.lower.poll_quota = 7u;                                 /* tighter */
    s.lower.priority = 50u;
    s.higher = asx_budget_new();
    s.higher.priority = 250u;
    ASSERT_EQ(asx_task_spawn_with_budget(r, poll_budget_spawner, &s, &tb, &t), ASX_OK);
    run = asx_budget_from_polls(20);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);
    ASSERT_EQ(s.status, ASX_OK);
    ASSERT_EQ((int)s.got_lower.priority, 50);
    ASSERT_EQ(s.got_lower.deadline, tb.deadline);
    ASSERT_EQ(s.got_lower.poll_quota, 7u);
    ASSERT_EQ((int)s.got_higher.priority, 250);
    ASSERT_EQ(s.got_higher.deadline, tb.deadline);
    ASSERT_EQ(s.got_higher.poll_quota, 49u); /* the spawner's, less the poll */
}

TEST(budgets_tighten_down_the_region_tree) {
    asx_region_id root;
    asx_region_id child;
    asx_region_id grandchild;
    asx_region_id sibling;
    asx_task_id t;
    asx_budget b;
    asx_budget got;

    setup();
    ASSERT_EQ(asx_region_open(&root), ASX_OK);
    ASSERT_EQ(asx_region_get_budget(root, &got), ASX_OK);
    ASSERT_EQ(got.deadline, ASX_TIME_INFINITE);
    ASSERT_EQ(got.poll_quota, UINT32_MAX);

    b = asx_budget_infinite();
    b.deadline = 100u * MS;
    ASSERT_EQ(asx_region_open_child_with_budget(root, &b, &child), ASX_OK);

    /* A looser deadline on the grandchild cannot widen the parent's. */
    b = asx_budget_infinite();
    b.deadline = 200u * MS;
    b.poll_quota = 7u;
    ASSERT_EQ(asx_region_open_child_with_budget(child, &b, &grandchild), ASX_OK);
    ASSERT_EQ(asx_region_get_budget(grandchild, &got), ASX_OK);
    ASSERT_EQ(got.deadline, (asx_time)(100u * MS));
    ASSERT_EQ(got.poll_quota, 7u);

    /* Plain children inherit the parent's budget unchanged. */
    ASSERT_EQ(asx_region_open_child(child, &sibling), ASX_OK);
    ASSERT_EQ(asx_region_get_budget(sibling, &got), ASX_OK);
    ASSERT_EQ(got.deadline, (asx_time)(100u * MS));
    ASSERT_EQ(got.poll_quota, UINT32_MAX);

    /* A task the host creates takes only its own budget, as Rust's
     * RuntimeState::create_task (state.rs:4461): the region's is not met
     * into it (tasks spawned from a task inherit, see below). */
    b = asx_budget_from_polls(50);
    ASSERT_EQ(asx_task_spawn_with_budget(grandchild, poll_complete, NULL, &b, &t), ASX_OK);
    ASSERT_EQ(asx_task_get_budget(t, &got), ASX_OK);
    ASSERT_EQ(got.poll_quota, 50u);
    ASSERT_EQ(got.deadline, ASX_TIME_INFINITE);

    ASSERT_EQ(asx_region_get_budget(grandchild, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_task_get_budget(t, NULL), ASX_E_INVALID_ARGUMENT);
}

typedef struct {
    asx_region_id region; /* the opener's own region */
    asx_budget budget;    /* the child region's requested budget */
    probe_state *probe;   /* the task it spawns into the child */
    asx_task_id spawned;
    asx_status status;
} opener_state;

/* Opens a child region with `budget`, spawns a probe into it, completes. */
static asx_status poll_opener(void *ud, asx_task_id self) {
    opener_state *s = (opener_state *)ud;
    asx_region_id child = ASX_INVALID_ID;
    s->status = asx_region_open_child_poll(self, s->region, &s->budget, &child);
    if (s->status == ASX_OK) s->status = asx_task_spawn(child, poll_probe, s->probe, &s->spawned);
    return ASX_OK;
}

TEST(region_deadline_bounds_tasks_spawned_into_it_from_a_task) {
    asx_region_id root;
    asx_region_id child;
    asx_task_id host_task;
    asx_task_id opener;
    probe_state host_probe;
    probe_state inner_probe;
    opener_state o;
    asx_budget b;
    asx_budget got;
    asx_budget run;

    setup();
    memset(&host_probe, 0, sizeof(host_probe));
    memset(&inner_probe, 0, sizeof(inner_probe));
    memset(&o, 0, sizeof(o));
    host_probe.park = 1;
    inner_probe.park = 1;
    ASSERT_EQ(asx_region_open(&root), ASX_OK);
    b = asx_budget_new();
    b.deadline = asx_runtime_virtual_now() + 3u * MS;

    /* A host-spawned task takes only its own budget, as Rust's
     * RuntimeState::create_task (state.rs:4461): the region's deadline does
     * not reach it. */
    ASSERT_EQ(asx_region_open_child_with_budget(root, &b, &child), ASX_OK);
    ASSERT_EQ(asx_task_spawn(child, poll_probe, &host_probe, &host_task), ASX_OK);
    ASSERT_EQ(asx_task_get_budget(host_task, &got), ASX_OK);
    ASSERT_EQ(got.deadline, ASX_TIME_INFINITE);

    /* A task that opens a region with the deadline and spawns into it
     * (child.cx().spawn) passes it on: that task is cancelled at 3 ms. */
    o.region = root;
    o.budget = b;
    o.probe = &inner_probe;
    o.spawned = ASX_INVALID_ID;
    ASSERT_EQ(asx_task_spawn(root, poll_opener, &o, &opener), ASX_OK);
    /* The host task stays parked past 3 ms: nothing cancels or wakes it. */
    run = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(root, &run), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(o.status, ASX_OK);
    ASSERT_TRUE(inner_probe.saw_cancel);
    ASSERT_EQ(inner_probe.kind, ASX_CANCEL_DEADLINE);
    ASSERT_TRUE(asx_runtime_virtual_now() >= b.deadline);
    ASSERT_FALSE(host_probe.saw_cancel);
}

/* -------------------------------------------------------------------
 * Obligations: kinds and holders
 * ------------------------------------------------------------------- */

TEST(reserve_ex_records_kind_and_holder) {
    asx_region_id r;
    asx_task_id t;
    asx_obligation_id o;
    asx_obligation_info info;
    asx_budget run;

    setup();
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_commit_existing, &o, &t), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve_ex(r, ASX_OBLIGATION_KIND_SEND_PERMIT, t, &o), ASX_OK);
    ASSERT_EQ(asx_obligation_get_info(o, &info), ASX_OK);
    ASSERT_EQ(info.state, ASX_OBLIGATION_RESERVED);
    ASSERT_EQ(info.kind, ASX_OBLIGATION_KIND_SEND_PERMIT);
    ASSERT_EQ(info.holder, t);
    ASSERT_EQ(info.region, r);
    ASSERT_EQ(info.abort_reason, ASX_OBLIGATION_ABORT_NONE);

    /* The host cannot impersonate a holder; its real poll commits it
     * before completing, with no leak. */
    ASSERT_EQ(asx_obligation_commit(o), ASX_E_PERMISSION_DENIED);
    run = asx_budget_from_polls(10);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);
    ASSERT_EQ(asx_obligation_get_info(o, &info), ASX_OK);
    ASSERT_EQ(info.state, ASX_OBLIGATION_COMMITTED);
    ASSERT_EQ(asx_obligation_leak_count(), (uint64_t)0u);
}

TEST(reserve_ex_validates_arguments) {
    asx_region_id r;
    asx_task_id t;
    asx_obligation_id o;
    asx_obligation_info info;
    asx_budget run;

    setup();
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve_ex(r, (asx_obligation_kind)99, ASX_INVALID_ID, &o),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_obligation_reserve_ex(r, ASX_OBLIGATION_KIND_LEASE, ASX_INVALID_ID, NULL),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_obligation_get_info(ASX_INVALID_ID, &info), ASX_E_NOT_FOUND);

    /* A completed task can no longer hold obligations. */
    ASSERT_EQ(asx_task_spawn(r, poll_complete, NULL, &t), ASX_OK);
    run = asx_budget_from_polls(10);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve_ex(r, ASX_OBLIGATION_KIND_ACK, t, &o), ASX_E_INVALID_STATE);

    /* A holder is named: an obligation with none is not representable, for
     * either registration, and a refusal creates nothing. */
    ASSERT_EQ(asx_obligation_reserve_ex(r, ASX_OBLIGATION_KIND_IO_OP, ASX_INVALID_ID, &o),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_obligation_register(r, ASX_OBLIGATION_KIND_IO_OP, ASX_INVALID_ID, &o),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_runtime_obligation_count(&g_rt), 0u);

    ASSERT_EQ(asx_task_spawn(r, poll_park_only, NULL, &t), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve_ex(r, ASX_OBLIGATION_KIND_IO_OP, t, &o), ASX_OK);
    ASSERT_EQ(asx_obligation_get_info(o, &info), ASX_OK);
    ASSERT_EQ(info.holder, t);
    ASSERT_EQ(info.kind, ASX_OBLIGATION_KIND_IO_OP);
    ASSERT_EQ(asx_obligation_get_info(o, NULL), ASX_E_INVALID_ARGUMENT);
}

/* Rust's create_obligation_in refuses a holder owned by another region
 * (ErrorKind::TaskNotOwned, runtime/state.rs:6130-6135), which the
 * vocabulary maps to ASX_E_INVALID_STATE (bd-91rf). */
static asx_region_id g_other_region;
static asx_status g_other_reserve;

static asx_status poll_reserve_in_other_region(void *ud, asx_task_id self) {
    asx_obligation_id o;
    (void)ud;
    (void)self;
    g_other_reserve = asx_obligation_reserve(g_other_region, &o);
    return ASX_OK;
}

TEST(reserve_holder_must_belong_to_the_region) {
    asx_region_id a;
    asx_region_id b;
    asx_task_id t;
    asx_obligation_id o;
    asx_obligation_info info;
    asx_budget run;

    setup();
    ASSERT_EQ(asx_region_open(&a), ASX_OK);
    ASSERT_EQ(asx_region_open(&b), ASX_OK);
    ASSERT_EQ(asx_task_spawn(a, poll_commit_existing, &o, &t), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve_ex(b, ASX_OBLIGATION_KIND_ACK, t, &o), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_obligation_reserve_ex(a, ASX_OBLIGATION_KIND_ACK, t, &o), ASX_OK);
    ASSERT_EQ(asx_obligation_commit(o), ASX_E_PERMISSION_DENIED);

    /* asx_obligation_reserve's implicit holder is the polled task. */
    g_other_region = b;
    g_other_reserve = ASX_OK;
    ASSERT_EQ(asx_task_spawn(a, poll_reserve_in_other_region, NULL, &t), ASX_OK);
    run = asx_budget_from_polls(10);
    ASSERT_EQ(asx_scheduler_run(a, &run), ASX_OK);
    ASSERT_EQ(asx_obligation_get_info(o, &info), ASX_OK);
    ASSERT_EQ(info.state, ASX_OBLIGATION_COMMITTED);
    ASSERT_EQ(asx_obligation_leak_count(), UINT64_C(0));
    ASSERT_EQ(g_other_reserve, ASX_E_INVALID_STATE);
}

/* Rust creates an obligation token only through a task (create_obligation
 * takes the holder's TaskId, runtime/state.rs:6130), so C has no holderless
 * obligation: outside a poll there is no task to hold it, and the reserve is
 * refused. The refusal is failure-atomic: no slot, count, trace event or
 * leak. A host reserves with a named holder (asx_obligation_reserve_ex). */
TEST(reserve_outside_a_poll_is_refused_and_creates_nothing) {
    asx_region_id r;
    asx_obligation_id o = ASX_INVALID_ID;
    uint32_t count_before;
    uint64_t emitted_before;
    uint64_t digest_before;

    setup();
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    count_before = asx_runtime_obligation_count(&g_rt);
    emitted_before = asx_trace_emitted_total();
    digest_before = asx_trace_digest();

    ASSERT_EQ(asx_obligation_reserve(r, &o), ASX_E_PERMISSION_DENIED);
    ASSERT_EQ(o, ASX_INVALID_ID);
    ASSERT_EQ(asx_runtime_obligation_count(&g_rt), count_before);
    ASSERT_EQ(asx_trace_emitted_total(), emitted_before);
    ASSERT_EQ(asx_trace_digest(), digest_before);
    ASSERT_EQ(asx_obligation_leak_count(), UINT64_C(0));

    /* Argument and region errors keep their own codes ahead of it. */
    ASSERT_EQ(asx_obligation_reserve(r, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_obligation_reserve(ASX_INVALID_ID, &o), ASX_E_NOT_FOUND);
}

/* -------------------------------------------------------------------
 * Obligations: leak resolution at holder completion
 * ------------------------------------------------------------------- */

TEST(reserve_in_poll_binds_current_task_and_leaks_under_log) {
    asx_region_id r;
    asx_task_id t;
    reserver_state s;
    asx_obligation_info info;
    asx_budget run;

    ASSERT_EQ(setup_with_policy(ASX_LEAK_LOG, NULL), ASX_OK);
    memset(&s, 0, sizeof(s));
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    s.region = r;
    s.ob = ASX_INVALID_ID;
    ASSERT_EQ(asx_task_spawn(r, poll_reserver, &s, &t), ASX_OK);
    run = asx_budget_from_polls(10);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);
    ASSERT_EQ(s.reserve_status, ASX_OK);
    ASSERT_EQ(asx_obligation_get_info(s.ob, &info), ASX_OK);
    ASSERT_EQ(info.holder, t);
    ASSERT_EQ(info.state, ASX_OBLIGATION_LEAKED);
    ASSERT_EQ(asx_obligation_leak_count(), (uint64_t)1u);

    /* A leaked obligation is terminal: the region can still close. */
    run = asx_budget_from_polls(10);
    ASSERT_EQ(asx_region_drain(r, &run), ASX_OK);
}

/* Reserves an obligation, then drops it unresolved in the same poll, as a
 * Rust body dropping its ObligationToken does. */
typedef struct {
    asx_region_id region;
    asx_obligation_id ob;
    asx_status drop_status;
    asx_obligation_state after_drop;
} dropper_state;

static asx_status poll_dropper(void *ud, asx_task_id self) {
    dropper_state *s = (dropper_state *)ud;
    asx_obligation_info info;
    (void)self;
    if (asx_obligation_reserve(s->region, &s->ob) != ASX_OK) return ASX_E_INVALID_STATE;
    s->drop_status = asx_obligation_drop(s->ob);
    if (asx_obligation_get_info(s->ob, &info) == ASX_OK) s->after_drop = info.state;
    return ASX_OK;
}

TEST(drop_leaks_the_obligation_at_once_not_at_completion) {
    asx_region_id r;
    asx_task_id t;
    dropper_state s;
    asx_budget run;

    ASSERT_EQ(setup_with_policy(ASX_LEAK_LOG, NULL), ASX_OK);
    memset(&s, 0, sizeof(s));
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    s.region = r;
    ASSERT_EQ(asx_task_spawn(r, poll_dropper, &s, &t), ASX_OK);
    run = asx_budget_from_polls(10);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);
    ASSERT_EQ(s.drop_status, ASX_OK);
    /* Leaked inside the poll, before the holder completed. */
    ASSERT_EQ((int)s.after_drop, (int)ASX_OBLIGATION_LEAKED);
    /* Unlinked from its holder: its completion leaks nothing more. */
    ASSERT_EQ(asx_obligation_leak_count(), (uint64_t)1u);
}

TEST(drop_under_recover_aborts_with_error) {
    /* Rust's Recover aborts the leak with ObligationAbortReason::Error
     * (runtime/state.rs:5781-5797); it still counts as a leak. */
    asx_region_id r;
    asx_task_id t;
    dropper_state s;
    asx_obligation_info info;
    asx_budget run;

    ASSERT_EQ(setup_with_policy(ASX_LEAK_RECOVER, NULL), ASX_OK);
    memset(&s, 0, sizeof(s));
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    s.region = r;
    ASSERT_EQ(asx_task_spawn(r, poll_dropper, &s, &t), ASX_OK);
    run = asx_budget_from_polls(10);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);
    ASSERT_EQ(s.drop_status, ASX_OK);
    ASSERT_EQ(asx_obligation_get_info(s.ob, &info), ASX_OK);
    ASSERT_EQ((int)info.state, (int)ASX_OBLIGATION_ABORTED);
    ASSERT_EQ((int)info.abort_reason, (int)ASX_OBLIGATION_ABORT_ERROR);
    ASSERT_EQ(asx_obligation_leak_count(), (uint64_t)1u);
}

static void drop_resolved_or_unknown(void *ctx) {
    asx_region_id r = *(asx_region_id *)ctx;
    asx_obligation_id ob;

    ASSERT_EQ(asx_obligation_reserve(r, &ob), ASX_OK);
    ASSERT_EQ(asx_obligation_commit(ob), ASX_OK);
    ASSERT_EQ(asx_obligation_drop(ob), ASX_E_INVALID_TRANSITION);
    ASSERT_EQ(asx_obligation_drop(ASX_INVALID_ID), ASX_E_NOT_FOUND);
}

TEST(drop_rejects_a_resolved_or_unknown_obligation) {
    asx_region_id r;

    setup();
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(test_run_in_task(r, drop_resolved_or_unknown, &r), ASX_OK);
    ASSERT_EQ(asx_obligation_leak_count(), (uint64_t)0u);
}

TEST(cancelled_holder_leaks_its_unresolved_obligation) {
    asx_region_id r;
    asx_task_id t;
    reserver_state s;
    asx_obligation_info info;
    asx_budget run;

    setup();
    memset(&s, 0, sizeof(s));
    s.park = 1;
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    s.region = r;
    s.ob = ASX_INVALID_ID;
    ASSERT_EQ(asx_task_spawn(r, poll_reserver, &s, &t), ASX_OK);
    run = asx_budget_from_polls(10);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(asx_obligation_get_info(s.ob, &info), ASX_OK);
    ASSERT_EQ(info.state, ASX_OBLIGATION_RESERVED);

    /* Draining cancels the holder, which completes with it unresolved: a
     * leak, as in Rust, where the dropped token posts a Leak whether or not
     * the task was cancelled (fixture obligation-cancelled-holder-leaks-001).
     * The region still drains. */
    run = asx_budget_from_polls(100);
    ASSERT_EQ(asx_region_drain(r, &run), ASX_OK);
    ASSERT_EQ(asx_obligation_get_info(s.ob, &info), ASX_OK);
    ASSERT_EQ(info.state, ASX_OBLIGATION_LEAKED);
    ASSERT_EQ(asx_obligation_leak_count(), (uint64_t)1u);
}

TEST(recover_policy_aborts_leaked_obligation) {
    asx_region_id r;
    asx_task_id t;
    reserver_state s;
    asx_obligation_info info;
    asx_budget run;

    ASSERT_EQ(setup_with_policy(ASX_LEAK_RECOVER, NULL), ASX_OK);
    memset(&s, 0, sizeof(s));
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    s.region = r;
    s.ob = ASX_INVALID_ID;
    ASSERT_EQ(asx_task_spawn(r, poll_reserver, &s, &t), ASX_OK);
    run = asx_budget_from_polls(10);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);
    ASSERT_EQ(asx_obligation_get_info(s.ob, &info), ASX_OK);
    ASSERT_EQ(info.state, ASX_OBLIGATION_ABORTED);
    ASSERT_EQ(info.abort_reason, ASX_OBLIGATION_ABORT_ERROR);
    ASSERT_EQ(asx_obligation_leak_count(), (uint64_t)1u);
}

TEST(panic_policy_routes_leak_through_containment) {
    asx_region_id r;
    asx_task_id t;
    reserver_state s;
    asx_obligation_info info;
    asx_budget run;
    asx_status st;
    int poisoned = 0;

    ASSERT_EQ(setup_with_policy(ASX_LEAK_PANIC, NULL), ASX_OK);
    memset(&s, 0, sizeof(s));
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    s.region = r;
    s.ob = ASX_INVALID_ID;
    ASSERT_EQ(asx_task_spawn(r, poll_reserver, &s, &t), ASX_OK);
    run = asx_budget_from_polls(10);
    st = asx_scheduler_run(r, &run);
    ASSERT_EQ(asx_obligation_get_info(s.ob, &info), ASX_OK);
    ASSERT_EQ(info.state, ASX_OBLIGATION_LEAKED);
    ASSERT_EQ(asx_obligation_leak_count(), (uint64_t)1u);

    /* Same routing as a failing poll: POISON_REGION poisons and keeps
     * draining; FAIL_FAST / ERROR_ONLY surface the fault to the caller. */
    if (asx_containment_policy_active() == ASX_CONTAIN_POISON_REGION) {
        ASSERT_EQ(st, ASX_OK);
        ASSERT_EQ(asx_region_is_poisoned(r, &poisoned), ASX_OK);
        ASSERT_TRUE(poisoned);
    } else {
        ASSERT_EQ(st, ASX_E_UNRESOLVED_OBLIGATIONS);
    }
}

TEST(leak_escalation_switches_policy_at_threshold) {
    asx_leak_escalation_config esc;
    asx_region_id r;
    asx_task_id t;
    reserver_state s[3];
    asx_obligation_info info;
    asx_budget run;
    uint32_t i;

    esc.threshold = 2u;
    esc.escalate_to = ASX_LEAK_RECOVER;
    ASSERT_EQ(setup_with_policy(ASX_LEAK_SILENT, &esc), ASX_OK);
    memset(s, 0, sizeof(s));
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    for (i = 0; i < 3u; i++) {
        s[i].region = r;
        s[i].ob = ASX_INVALID_ID;
        ASSERT_EQ(asx_task_spawn(r, poll_reserver, &s[i], &t), ASX_OK);
    }
    run = asx_budget_from_polls(10);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);

    /* Tasks complete in slot order. Rust counts a leak before it picks the
     * response (runtime/state.rs:5711-5717): the first leak is silent, the
     * second reaches the threshold and is the first escalated (RECOVER
     * aborts it), and so is the third. */
    ASSERT_EQ(asx_obligation_get_info(s[0].ob, &info), ASX_OK);
    ASSERT_EQ(info.state, ASX_OBLIGATION_LEAKED);
    ASSERT_EQ(asx_obligation_get_info(s[1].ob, &info), ASX_OK);
    ASSERT_EQ(info.state, ASX_OBLIGATION_ABORTED);
    ASSERT_EQ(info.abort_reason, ASX_OBLIGATION_ABORT_ERROR);
    ASSERT_EQ(asx_obligation_get_info(s[2].ob, &info), ASX_OK);
    ASSERT_EQ(info.state, ASX_OBLIGATION_ABORTED);
    ASSERT_EQ(info.abort_reason, ASX_OBLIGATION_ABORT_ERROR);
    ASSERT_EQ(asx_obligation_leak_count(), (uint64_t)3u);
}

TEST(leak_escalation_threshold_zero_escalates_the_first_leak) {
    /* Rust's LeakEscalation::new turns a threshold of 0 into 1
     * (runtime/config.rs:1354); either way the first leak escalates. */
    asx_leak_escalation_config esc;
    asx_region_id r;
    asx_task_id t;
    reserver_state s;
    asx_obligation_info info;
    asx_budget run;
    uint64_t threshold;

    for (threshold = 0u; threshold <= 1u; threshold++) {
        esc.threshold = threshold;
        esc.escalate_to = ASX_LEAK_RECOVER;
        ASSERT_EQ(setup_with_policy(ASX_LEAK_SILENT, &esc), ASX_OK);
        memset(&s, 0, sizeof(s));
        ASSERT_EQ(asx_region_open(&r), ASX_OK);
        s.region = r;
        s.ob = ASX_INVALID_ID;
        ASSERT_EQ(asx_task_spawn(r, poll_reserver, &s, &t), ASX_OK);
        run = asx_budget_from_polls(10);
        ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);
        ASSERT_EQ(asx_obligation_get_info(s.ob, &info), ASX_OK);
        ASSERT_EQ(info.state, ASX_OBLIGATION_ABORTED);
        ASSERT_EQ(info.abort_reason, ASX_OBLIGATION_ABORT_ERROR);
    }
}

TEST(runtime_reset_clears_leak_count) {
    asx_region_id r;
    asx_task_id t;
    reserver_state s;
    asx_budget run;

    setup();
    memset(&s, 0, sizeof(s));
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    s.region = r;
    s.ob = ASX_INVALID_ID;
    ASSERT_EQ(asx_task_spawn(r, poll_reserver, &s, &t), ASX_OK);
    run = asx_budget_from_polls(10);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);
    ASSERT_EQ(asx_obligation_leak_count(), (uint64_t)1u);
    asx_runtime_reset();
    ASSERT_EQ(asx_obligation_leak_count(), (uint64_t)0u);
}

/* -------------------------------------------------------------------
 * Region close: finalizers, then the leak audit, then close (bd-9kll.3.4)
 * ------------------------------------------------------------------- */

/* The close audit is the net under holder completion: Rust's Finalizing arm
 * leak-audits the obligations of the region still Reserved once no task is
 * left (runtime/state.rs:10266-10317). Every obligation has a holder, and its
 * holder's completion settles it first, so no public sequence leaves one for
 * the audit. These tests therefore call the audit while the holders are
 * still parked (it takes every Reserved obligation of the region whoever
 * holds it), then drain, to show a leaked obligation is terminal: its
 * holder's completion leaks nothing more and the region closes. */
static probe_state g_audit_holder;

/* `n` obligations of `r`, held by one parked task of it that completes when
 * cancelled. */
static void hold_reserved(asx_region_id r, uint32_t n, asx_obligation_id *out) {
    asx_task_id holder;
    uint32_t i;

    memset(&g_audit_holder, 0, sizeof(g_audit_holder));
    g_audit_holder.park = 1;
    ASSERT_EQ(asx_task_spawn(r, poll_probe, &g_audit_holder, &holder), ASX_OK);
    for (i = 0u; i < n; i++) {
        ASSERT_EQ(asx_obligation_reserve_ex(r, ASX_OBLIGATION_KIND_GENERIC, holder, &out[i]),
                  ASX_OK);
    }
}

TEST(region_close_audit_leaks_obligations_still_reserved) {
    asx_region_id r;
    asx_obligation_id ob[2];
    asx_obligation_info info;
    asx_region_state rs;
    asx_budget run;

    ASSERT_EQ(setup_with_policy(ASX_LEAK_LOG, NULL), ASX_OK);
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    hold_reserved(r, 2u, ob);
    if (test_current_failed) return;
    ASSERT_EQ(asx_region_audit_leaks_internal(r), ASX_OK);
    ASSERT_EQ(asx_obligation_get_info(ob[0], &info), ASX_OK);
    ASSERT_EQ(info.state, ASX_OBLIGATION_LEAKED);
    ASSERT_EQ(asx_obligation_get_info(ob[1], &info), ASX_OK);
    ASSERT_EQ(info.state, ASX_OBLIGATION_LEAKED);
    ASSERT_EQ(asx_obligation_leak_count(), (uint64_t)2u);

    run = asx_budget_infinite();
    ASSERT_EQ(asx_region_drain(r, &run), ASX_OK);
    ASSERT_EQ(asx_region_get_state(r, &rs), ASX_OK);
    ASSERT_EQ(rs, ASX_REGION_CLOSED);
    ASSERT_EQ(asx_obligation_leak_count(), (uint64_t)2u);
}

TEST(region_close_leak_audit_is_one_batch) {
    /* The audit's leaks are one batch (handle_obligation_leaks counts them
     * together, then picks one response, state.rs:5711-5717): with a
     * threshold of 2 both escalate, where two holders' completions (one
     * leak each) leave the first under the base policy. */
    asx_leak_escalation_config esc;
    asx_region_id r;
    asx_obligation_id ob[2];
    asx_obligation_info info;

    esc.threshold = 2u;
    esc.escalate_to = ASX_LEAK_RECOVER;
    ASSERT_EQ(setup_with_policy(ASX_LEAK_SILENT, &esc), ASX_OK);
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    hold_reserved(r, 2u, ob);
    if (test_current_failed) return;
    ASSERT_EQ(asx_region_audit_leaks_internal(r), ASX_OK);
    ASSERT_EQ(asx_obligation_get_info(ob[0], &info), ASX_OK);
    ASSERT_EQ(info.state, ASX_OBLIGATION_ABORTED);
    ASSERT_EQ(info.abort_reason, ASX_OBLIGATION_ABORT_ERROR);
    ASSERT_EQ(asx_obligation_get_info(ob[1], &info), ASX_OK);
    ASSERT_EQ(info.state, ASX_OBLIGATION_ABORTED);
    ASSERT_EQ(asx_obligation_leak_count(), (uint64_t)2u);
}

TEST(leak_policy_matrix_follows_rusts_count_then_pick_rule) {
    /* handle_obligation_leaks adds the batch to the cumulative count, then
     * picks one response for the whole batch: the escalation's if the
     * count has reached its threshold (a threshold of 0 is 1), else the
     * base (runtime/state.rs:5711-5721; config.rs:1354). Every base
     * policy and escalation target x threshold {0, 1, 3} x batch size
     * {1, 3} x leaks already counted {0, 2}; a close audit is one batch.
     * RECOVER aborts with Error (still counted); the others mark Leaked. */
    static const struct {
        asx_leak_response base;
        asx_leak_response to;
    } pairs[] = {{ASX_LEAK_SILENT, ASX_LEAK_RECOVER},
                 {ASX_LEAK_LOG, ASX_LEAK_RECOVER},
                 {ASX_LEAK_RECOVER, ASX_LEAK_SILENT}};
    static const uint64_t thresholds[] = {0u, 1u, 3u};
    static const uint32_t batches[] = {1u, 3u};
    static const uint32_t priors[] = {0u, 2u};
    uint32_t p;
    uint32_t t;
    uint32_t b;
    uint32_t c;

    for (p = 0; p < 3u; p++) {
        for (t = 0; t < 3u; t++) {
            for (b = 0; b < 2u; b++) {
                for (c = 0; c < 2u; c++) {
                    asx_leak_escalation_config esc;
                    asx_region_id prior_region;
                    asx_region_id r;
                    asx_obligation_id ob[3];
                    asx_obligation_info info;
                    asx_leak_response effective;
                    asx_budget run;
                    uint32_t i;
                    uint64_t effective_threshold = thresholds[t] == 0u ? 1u : thresholds[t];
                    int escalated = (uint64_t)(priors[c] + batches[b]) >= effective_threshold;

                    esc.threshold = thresholds[t];
                    esc.escalate_to = pairs[p].to;
                    ASSERT_EQ(setup_with_policy(pairs[p].base, &esc), ASX_OK);

                    if (priors[c] > 0u) {
                        ASSERT_EQ(asx_region_open(&prior_region), ASX_OK);
                        hold_reserved(prior_region, priors[c], ob);
                        if (test_current_failed) return;
                        ASSERT_EQ(asx_region_audit_leaks_internal(prior_region), ASX_OK);
                        run = asx_budget_infinite();
                        ASSERT_EQ(asx_region_drain(prior_region, &run), ASX_OK);
                        ASSERT_EQ(asx_obligation_leak_count(), (uint64_t)priors[c]);
                    }

                    ASSERT_EQ(asx_region_open(&r), ASX_OK);
                    hold_reserved(r, batches[b], ob);
                    if (test_current_failed) return;
                    ASSERT_EQ(asx_region_audit_leaks_internal(r), ASX_OK);
                    run = asx_budget_infinite();
                    ASSERT_EQ(asx_region_drain(r, &run), ASX_OK);

                    effective = escalated ? pairs[p].to : pairs[p].base;
                    for (i = 0; i < batches[b]; i++) {
                        ASSERT_EQ(asx_obligation_get_info(ob[i], &info), ASX_OK);
                        if (effective == ASX_LEAK_RECOVER) {
                            ASSERT_EQ(info.state, ASX_OBLIGATION_ABORTED);
                            ASSERT_EQ(info.abort_reason, ASX_OBLIGATION_ABORT_ERROR);
                        } else {
                            ASSERT_EQ(info.state, ASX_OBLIGATION_LEAKED);
                        }
                    }
                    /* Counted either way, the recovered ones included. */
                    ASSERT_EQ(asx_obligation_leak_count(), (uint64_t)(priors[c] + batches[b]));
                }
            }
        }
    }
}

TEST(region_close_leak_audit_under_panic_fails_before_closing) {
    /* PANIC: Rust marks them leaked, then fails fast before the region
     * closes (state.rs:5735-5756). The leaked obligation no longer holds
     * the region, so a later drain closes it. */
    asx_region_id r;
    asx_obligation_id a;
    asx_obligation_info info;
    asx_region_state rs;
    asx_budget run;

    ASSERT_EQ(setup_with_policy(ASX_LEAK_PANIC, NULL), ASX_OK);
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    hold_reserved(r, 1u, &a);
    if (test_current_failed) return;
    ASSERT_EQ(asx_region_audit_leaks_internal(r), ASX_E_UNRESOLVED_OBLIGATIONS);
    ASSERT_EQ(asx_obligation_get_info(a, &info), ASX_OK);
    ASSERT_EQ(info.state, ASX_OBLIGATION_LEAKED);
    ASSERT_EQ(asx_obligation_leak_count(), (uint64_t)1u);
    if (asx_containment_policy_active() != ASX_CONTAIN_POISON_REGION) {
        /* The fault the audit raised is the scheduler's to report once. */
        ASSERT_EQ(asx_runtime_take_pending_fault_internal(), ASX_E_UNRESOLVED_OBLIGATIONS);
        run = asx_budget_infinite();
        ASSERT_EQ(asx_region_drain(r, &run), ASX_OK);
        ASSERT_EQ(asx_region_get_state(r, &rs), ASX_OK);
        ASSERT_EQ(rs, ASX_REGION_CLOSED);
    }
}

int main(void) {
    fprintf(stderr, "=== test_budget_obligation ===\n");

    RUN_TEST(deadline_cancels_parked_task_on_virtual_time);
    RUN_TEST(expired_deadline_cancels_before_first_poll);
    RUN_TEST(poll_quota_cancels_after_quota_polls);
    RUN_TEST(unbounded_poll_quota_is_not_consumed);
    RUN_TEST(spent_poll_quota_strengthens_pending_cancel_at_checkpoint);
    RUN_TEST(spent_poll_quota_strengthens_pending_cancel_before_poll);
    RUN_TEST(spent_poll_quota_keeps_a_stronger_pending_cancel);
    RUN_TEST(cost_quota_spent_to_zero_cancels_at_the_next_checkpoint);
    RUN_TEST(cost_charge_the_quota_cannot_cover_is_refused_and_cancels_nothing);
    RUN_TEST(consume_cost_rejects_stale_handle);
    RUN_TEST(spawned_child_inherits_the_quota_until_the_cleanup_phase_starts);
    RUN_TEST(cleanup_spawn_inherits_without_the_parents_cleanup_poll_quota);
    RUN_TEST(spawn_budget_from_a_task_keeps_its_priority_and_clamps_the_rest);
    RUN_TEST(nested_region_budgets_equal_their_meet);
    RUN_TEST(budgets_tighten_down_the_region_tree);
    RUN_TEST(region_deadline_bounds_tasks_spawned_into_it_from_a_task);
    RUN_TEST(reserve_ex_records_kind_and_holder);
    RUN_TEST(non_holder_cannot_resolve_an_obligation_but_its_owner_can);
    RUN_TEST(handoff_same_region_reuses_the_full_quota);
    RUN_TEST(handoff_cross_region_moves_the_credit_and_holder);
    RUN_TEST(handoff_full_destination_is_failure_atomic);
    RUN_TEST(handoff_lab_posts_keep_original_reservation_and_transfer_order);
    RUN_TEST(handoff_lab_same_region_reuses_the_full_quota);
    RUN_TEST(handoff_same_holder_is_failure_atomic);
    RUN_TEST(handoff_unchecked_source_is_failure_atomic);
    RUN_TEST(handoff_invalid_destination_is_failure_atomic);
    RUN_TEST(handoff_wrong_type_destination_is_failure_atomic);
    RUN_TEST(handoff_stale_reused_destination_is_failure_atomic);
    RUN_TEST(handoff_completed_destination_is_failure_atomic);
    RUN_TEST(handoff_cancelled_destination_is_failure_atomic);
    RUN_TEST(handoff_closing_destination_is_failure_atomic);
    RUN_TEST(handoff_poisoned_destination_is_failure_atomic);
    RUN_TEST(handoff_zero_destination_quota_is_failure_atomic);
    RUN_TEST(handoff_full_destination_preserves_both_region_counts);
    RUN_TEST(handoff_unknown_obligation_is_failure_atomic);
    RUN_TEST(handoff_wrong_type_obligation_is_failure_atomic);
    RUN_TEST(handoff_stale_reused_obligation_is_failure_atomic);
    RUN_TEST(handoff_resolved_obligation_is_failure_atomic);
    RUN_TEST(host_raw_resolution_of_a_held_credit_is_failure_atomic);
    RUN_TEST(task_raw_resolution_of_another_regions_credit_is_failure_atomic);
    RUN_TEST(handoff_uses_no_extra_slot_when_the_obligation_arena_is_full);
    RUN_TEST(reserve_ex_validates_arguments);
    RUN_TEST(reserve_holder_must_belong_to_the_region);
    RUN_TEST(reserve_outside_a_poll_is_refused_and_creates_nothing);
    RUN_TEST(reserve_in_poll_binds_current_task_and_leaks_under_log);
    RUN_TEST(drop_leaks_the_obligation_at_once_not_at_completion);
    RUN_TEST(drop_under_recover_aborts_with_error);
    RUN_TEST(drop_rejects_a_resolved_or_unknown_obligation);
    RUN_TEST(cancelled_holder_leaks_its_unresolved_obligation);
    RUN_TEST(recover_policy_aborts_leaked_obligation);
    RUN_TEST(panic_policy_routes_leak_through_containment);
    RUN_TEST(leak_escalation_switches_policy_at_threshold);
    RUN_TEST(leak_escalation_threshold_zero_escalates_the_first_leak);
    RUN_TEST(runtime_reset_clears_leak_count);
    RUN_TEST(region_close_audit_leaks_obligations_still_reserved);
    RUN_TEST(region_close_leak_audit_is_one_batch);
    RUN_TEST(leak_policy_matrix_follows_rusts_count_then_pick_rule);
    RUN_TEST(region_close_leak_audit_under_panic_fails_before_closing);

    TEST_REPORT();
    return test_failures;
}
