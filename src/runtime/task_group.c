/*
 * task_group.c — structured combinators over spawned tasks with loser drain
 *
 * The owner task polls the group; members wake it through the task-slot
 * completion watcher. Every decision (winner, quorum reached or
 * impossible, owner cancel, deadline) moves the group to DRAINING: the
 * unfinished members are cancelled once and the group only reaches DONE
 * after each of them has completed and been joined.
 *
 * SPDX-License-Identifier: MIT
 */

#include "runtime_internal.h"
#include <asx/core/transition.h>
#include <asx/runtime/task_group.h>
#include <string.h>

static asx_time group_now(void) {
    asx_time now;
    if (asx_runtime_now_ns(&now) != ASX_OK) now = asx_runtime_virtual_now();
    return now;
}

/* Status a completed member contributes to the group result. */
static asx_status group_member_status(const asx_task_slot *t) {
    switch (asx_outcome_severity_of(&t->outcome)) {
    case ASX_OUTCOME_OK: return ASX_OK;
    case ASX_OUTCOME_ERR: return t->last_error != ASX_OK ? t->last_error : ASX_E_INVALID_STATE;
    case ASX_OUTCOME_CANCELLED: return ASX_E_CANCELLED;
    case ASX_OUTCOME_PANICKED: return ASX_E_INVALID_STATE;
    default: return ASX_E_INVALID_STATE;
    }
}

/* Most severe status among completed members the group did not cancel
 * itself (lowest index on ties); ASX_OK when all of them succeeded. */
static asx_status group_worst_status(const asx_task_group *g) {
    asx_outcome_severity worst = ASX_OUTCOME_OK;
    asx_status st = ASX_OK;
    uint32_t i;
    for (i = 0; i < g->count; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: count <= ASX_TASK_GROUP_MAX");
        asx_outcome_severity sev;
        if (!g->completed[i] || g->cancel_sent[i]) continue;
        sev = asx_outcome_severity_of(&g->outcomes[i]);
        if (sev > worst) {
            worst = sev;
            st = g->statuses[i];
        }
    }
    return st;
}

/* Collect completed members (joining them) and register the owner as the
 * completion watcher of the rest. */
static void group_collect(asx_task_group *g, const asx_task_slot *owner) {
    uint32_t owner_idx = (uint32_t)(owner - g_tasks);
    uint32_t n = g->mode == ASX_TASK_GROUP_FIRST_OK ? g->spawned : g->count;
    uint32_t i;
    /* Rust's join_all awaits the members' joins one by one, in order
     * (cx/scope.rs:1479-1490), and so does a quorum's drain (:1897-1903):
     * this poll joins only the first unfinished member. A member whose join
     * an earlier poll registered keeps that registration, at that poll's
     * waker priority, until it fires (task_handle.rs:115-133). */
    int sequential = g->mode == ASX_TASK_GROUP_JOIN_ALL ||
                     (g->mode == ASX_TASK_GROUP_QUORUM && g->phase == ASX_TASK_GROUP_DRAINING);

    for (i = 0; i < n; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: count <= ASX_TASK_GROUP_MAX");
        asx_task_slot *t;
        asx_status st;

        if (g->completed[i]) continue;
        st = asx_task_slot_lookup(g->members[i], &t);
        if (st != ASX_OK) {
            /* Joined or released behind the group's back. */
            g->outcomes[i] = asx_outcome_make(ASX_OUTCOME_ERR);
            g->statuses[i] = st;
        } else if (asx_task_is_terminal(t->state)) {
            g->outcomes[i] = t->outcome;
            g->statuses[i] = group_member_status(t);
            g->cancel_kinds[i] = t->cancel_reason.kind;
            t->watcher = ASX_SLOT_NONE;
            st = asx_task_join(g->members[i], NULL);
            (void)st;
        } else {
            t->watcher = owner_idx;
            t->watcher_gen = owner->generation;
            t->watcher_prio = owner->lab_waker_prio;
            if (sequential) break;
            continue;
        }
        g->completed[i] = 1;
        g->completed_count++;
        if (asx_outcome_severity_of(&g->outcomes[i]) == ASX_OUTCOME_OK) g->ok_count++;
    }
}

static void group_begin_drain(asx_task_group *g, asx_cancel_kind kind, asx_status result,
                              int early) {
    g->phase = ASX_TASK_GROUP_DRAINING;
    g->drain_kind = kind;
    g->result = result;
    g->early = (uint8_t)(early ? 1 : 0);
}

static void group_send_cancels(asx_task_group *g) {
    uint32_t i;
    for (i = 0; i < g->count; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: count <= ASX_TASK_GROUP_MAX");
        asx_status st;
        if (g->completed[i] || g->cancel_sent[i]) continue;
        if (g->mode == ASX_TASK_GROUP_QUORUM &&
            (g->owner_cancelled || g->drain_kind == ASX_CANCEL_RACE_LOST)) {
            /* Rust's quorum drains with handle aborts: the caller's reason,
             * or CancelReason::quorum_met() (race_loser, default
             * attribution) (cx/scope.rs:1878-1891). */
            asx_cancel_reason r =
                g->owner_cancelled ? g->owner_reason
                                   : asx_cancel_reason_testing_default(ASX_CANCEL_RACE_LOST, NULL);
            st = asx_task_abort_request(g->members[i], &r);
        } else {
            st = asx_task_cancel(g->members[i], g->drain_kind);
        }
        (void)st;
        g->cancel_sent[i] = 1;
    }
}

/* QUORUM's result once every member completed (Rust Scope::quorum and
 * quorum_to_result, cx/scope.rs:1925-1941, combinator/quorum.rs:398-468):
 * a panic first, even when the quorum was met; the owner cancelled before
 * it was met; met; a member cancelled by anything but the RACE_LOST drain;
 * the first error by index. */
static asx_status group_quorum_result(const asx_task_group *g) {
    uint32_t i;
    for (i = 0; i < g->count; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: count <= ASX_TASK_GROUP_MAX");
        if (asx_outcome_severity_of(&g->outcomes[i]) == ASX_OUTCOME_PANICKED) return g->statuses[i];
    }
    if (g->ok_count >= g->needed) return ASX_OK;
    if (g->owner_cancelled) return ASX_E_CANCELLED;
    for (i = 0; i < g->count; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: count <= ASX_TASK_GROUP_MAX");
        if (asx_outcome_severity_of(&g->outcomes[i]) == ASX_OUTCOME_CANCELLED &&
            g->cancel_kinds[i] != ASX_CANCEL_RACE_LOST) {
            return ASX_E_CANCELLED;
        }
    }
    for (i = 0; i < g->count; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: count <= ASX_TASK_GROUP_MAX");
        if (asx_outcome_severity_of(&g->outcomes[i]) == ASX_OUTCOME_ERR) return g->statuses[i];
    }
    return ASX_E_CANCELLED;
}

/* Decide the group from the members collected so far. Member completion
 * takes precedence over owner cancellation and the deadline. */
static void group_decide(asx_task_group *g, const asx_task_slot *owner, asx_task_id self) {
    uint32_t i;
    int owner_cancelled;

    switch (g->mode) {
    case ASX_TASK_GROUP_JOIN_ALL:
        if (g->completed_count == g->count) {
            group_begin_drain(g, ASX_CANCEL_RACE_LOST, group_worst_status(g), 0);
        }
        break;
    case ASX_TASK_GROUP_RACE:
        for (i = 0; i < g->count; i++) {
            ASX_CHECKPOINT_WAIVER("bounded: count <= ASX_TASK_GROUP_MAX");
            if (!g->completed[i]) continue;
            g->winner = (int32_t)i;
            group_begin_drain(g, ASX_CANCEL_RACE_LOST, g->statuses[i], 0);
            break;
        }
        break;
    case ASX_TASK_GROUP_FIRST_OK: break; /* sequential: group_first_ok_poll */
    case ASX_TASK_GROUP_QUORUM:
        if (g->needed == 0u || g->needed > g->count) {
            group_begin_drain(g, ASX_CANCEL_RACE_LOST, ASX_E_INVALID_ARGUMENT, 1);
        } else if (g->ok_count >= g->needed) {
            group_begin_drain(g, ASX_CANCEL_RACE_LOST, ASX_OK, 0);
        } else if (g->completed_count - g->ok_count > g->count - g->needed) {
            group_begin_drain(g, ASX_CANCEL_RACE_LOST, group_worst_status(g), 0);
        }
        break;
    default: group_begin_drain(g, ASX_CANCEL_RACE_LOST, ASX_E_INVALID_STATE, 1); break;
    }
    if (g->phase != ASX_TASK_GROUP_COLLECTING) return;

    /* join_all's joins are uninterruptible: the owner's cancel does not
     * reach the members (Rust's join_all awaits every join). QUORUM sees
     * it through a checkpoint (cx/scope.rs:1874), which also raises a
     * spent poll quota, attributed to the owner and stamped now, and
     * acknowledges the cancel. */
    if (g->mode == ASX_TASK_GROUP_QUORUM) {
        asx_checkpoint_result cp;
        owner_cancelled = asx_checkpoint(self, &cp) == ASX_OK && cp.cancelled;
    } else {
        owner_cancelled = owner->cancel_pending && g->mode != ASX_TASK_GROUP_JOIN_ALL;
    }
    if (owner_cancelled) {
        g->owner_cancelled = 1u;
        g->owner_reason = owner->cancel_reason;
        group_begin_drain(g, ASX_CANCEL_PARENT, ASX_E_CANCELLED, 1);
        return;
    }
    if (g->deadline != 0u && group_now() >= g->deadline) {
        group_begin_drain(g, ASX_CANCEL_TIMEOUT,
                          g->mode == ASX_TASK_GROUP_QUORUM ? ASX_E_THRESHOLD_TIMEOUT
                                                           : ASX_E_TIMED_OUT,
                          1);
    }
}

static asx_status group_finish(asx_task_group *g, asx_status result) {
    g->phase = ASX_TASK_GROUP_DONE;
    g->result = result;
    return result;
}

/* FIRST_OK (Rust Scope::first_ok, cx/scope.rs:2023-2099): one attempt at a
 * time, in registration order; see asx_task_group_add_attempt. */
static asx_status group_first_ok_poll(asx_task_group *g, asx_task_slot *owner, asx_task_id self) {
    asx_checkpoint_result cp;
    asx_status st;

    /* cx.scope() is taken once, at the call: its budget is the owner's
     * inherited budget then (an unbounded poll quota in cleanup,
     * cx.rs:2504-2511), and every attempt is spawned with it. */
    if (!g->attempt_budget_set) {
        g->attempt_budget = owner->budget;
        if (owner->cleanup_applied) g->attempt_budget.poll_quota = UINT32_MAX;
        g->attempt_budget_set = 1u;
    }
    for (;;) {
        ASX_CHECKPOINT_WAIVER("bounded: each pass starts or finishes one of <= "
                              "ASX_TASK_GROUP_MAX attempts");
        if (g->spawned > 0u) {
            uint32_t cur = g->spawned - 1u;
            /* Every poll of the join checks the caller's cancel first, the
             * poll that finds the attempt done included (cx/scope.rs:
             * 2068-2079): it reaches the attempt once, as a handle abort
             * with the caller's reason, and the attempt is still awaited. */
            if (!g->forwarded && !g->completed[cur] && asx_checkpoint(self, &cp) == ASX_OK &&
                cp.cancelled) {
                st = asx_task_abort_request(g->members[cur], &owner->cancel_reason);
                (void)st; /* not collected yet: its record exists */
                g->forwarded = 1u;
            }
            group_collect(g, owner);
            if (!g->completed[cur]) {
                /* A deadline ends the attempt with TIMEOUT. */
                if (!g->forwarded && g->deadline != 0u && group_now() >= g->deadline) {
                    st = asx_task_cancel(g->members[cur], ASX_CANCEL_TIMEOUT);
                    (void)st;
                    g->forwarded = 1u;
                    g->early = 1u;
                    g->result = ASX_E_TIMED_OUT;
                }
                if (g->deadline != 0u && !g->forwarded) {
                    st = asx_task_arm_timer(self, g->deadline);
                    (void)st;
                }
                if (owner->in_poll) owner->park_requested = 1;
                return ASX_E_PENDING;
            }
            if (g->early) return group_finish(g, g->result);
            switch (asx_outcome_severity_of(&g->outcomes[cur])) {
            case ASX_OUTCOME_OK: g->winner = (int32_t)cur; return group_finish(g, ASX_OK);
            case ASX_OUTCOME_ERR: break; /* the next attempt */
            case ASX_OUTCOME_CANCELLED:
            case ASX_OUTCOME_PANICKED:
            default: return group_finish(g, g->statuses[cur]);
            }
        }
        if (g->early) return group_finish(g, g->result);
        if (g->spawned == g->count) {
            return group_finish(g, g->count == 0u ? ASX_E_INVALID_ARGUMENT : group_worst_status(g));
        }
        /* Never start an attempt once the caller is cancelled. */
        if (asx_checkpoint(self, &cp) == ASX_OK && cp.cancelled) {
            return group_finish(g, ASX_E_CANCELLED);
        }
        {
            uint32_t i = g->spawned;
            asx_task_slot *t;
            st = asx_task_spawn(g->attempt_regions[i], g->attempt_fns[i], g->attempt_data[i],
                                &g->members[i]);
            if (st != ASX_OK) return group_finish(g, ASX_E_CANCELLED);
            /* spawn_in_cancellation_dominant: a cancel pending when the
             * attempt completes makes it CANCELLED whatever it returned. */
            if (asx_task_slot_lookup(g->members[i], &t) == ASX_OK) {
                t->spawned_in_poll = 0;
                t->budget = g->attempt_budget;
            }
            g->spawned++;
            g->forwarded = 0u;
        }
    }
}

/* -------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------- */

asx_status asx_task_group_init(asx_task_group *g, asx_task_group_mode mode, uint32_t needed) {
    if (g == NULL) return ASX_E_INVALID_ARGUMENT;
    if ((int)mode < (int)ASX_TASK_GROUP_JOIN_ALL || (int)mode > (int)ASX_TASK_GROUP_QUORUM) {
        return ASX_E_INVALID_ARGUMENT;
    }
    memset(g, 0, sizeof(*g));
    g->mode = mode;
    g->needed = needed;
    g->phase = ASX_TASK_GROUP_COLLECTING;
    g->winner = -1;
    g->drain_kind = ASX_CANCEL_RACE_LOST;
    g->result = ASX_E_PENDING;
    return ASX_OK;
}

asx_status asx_task_group_set_deadline(asx_task_group *g, asx_time deadline) {
    if (g == NULL) return ASX_E_INVALID_ARGUMENT;
    if (g->phase != ASX_TASK_GROUP_COLLECTING) return ASX_E_INVALID_STATE;
    g->deadline = deadline;
    return ASX_OK;
}

asx_status asx_task_group_add(asx_task_group *g, asx_task_id task) {
    asx_task_slot *t;
    asx_status st;
    uint32_t i;

    if (g == NULL) return ASX_E_INVALID_ARGUMENT;
    if (g->phase != ASX_TASK_GROUP_COLLECTING) return ASX_E_INVALID_STATE;
    if (g->mode == ASX_TASK_GROUP_FIRST_OK) return ASX_E_INVALID_STATE; /* add_attempt */
    if (g->count >= ASX_TASK_GROUP_MAX) return ASX_E_RESOURCE_EXHAUSTED;
    st = asx_task_slot_lookup(task, &t);
    if (st != ASX_OK) return st;
    /* A detached member's slot (and outcome) vanishes at completion. */
    if (t->detached) return ASX_E_INVALID_STATE;
    for (i = 0; i < g->count; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: count <= ASX_TASK_GROUP_MAX");
        if (g->members[i] == task) return ASX_E_ALREADY_EXISTS;
    }

    /* Rust spawns quorum branches cancellation-dominant
     * (spawn_quorum_branches, cx/scope.rs:1962): a branch that acknowledges
     * a cancel and still returns OK counts as CANCELLED. */
    if (g->mode == ASX_TASK_GROUP_QUORUM) t->spawned_in_poll = 0;
    g->members[g->count] = task;
    g->outcomes[g->count] = asx_outcome_make(ASX_OUTCOME_OK);
    g->statuses[g->count] = ASX_E_PENDING;
    g->completed[g->count] = 0;
    g->cancel_sent[g->count] = 0;
    g->count++;
    return ASX_OK;
}

asx_status asx_task_group_spawn(asx_task_group *g, asx_region_id region, asx_task_poll_fn poll_fn,
                                void *user_data, asx_task_id *out_id) {
    asx_task_id id;
    asx_status st;

    if (g == NULL) return ASX_E_INVALID_ARGUMENT;
    if (g->phase != ASX_TASK_GROUP_COLLECTING) return ASX_E_INVALID_STATE;
    if (g->mode == ASX_TASK_GROUP_FIRST_OK) return ASX_E_INVALID_STATE; /* add_attempt */
    if (g->count >= ASX_TASK_GROUP_MAX) return ASX_E_RESOURCE_EXHAUSTED;
    st = asx_task_spawn(region, poll_fn, user_data, &id);
    if (st != ASX_OK) return st;
    st = asx_task_group_add(g, id);
    if (st != ASX_OK) return st;
    if (out_id != NULL) *out_id = id;
    return ASX_OK;
}

asx_status asx_task_group_add_attempt(asx_task_group *g, asx_region_id region,
                                      asx_task_poll_fn poll_fn, void *user_data) {
    if (g == NULL || poll_fn == NULL) return ASX_E_INVALID_ARGUMENT;
    if (g->mode != ASX_TASK_GROUP_FIRST_OK || g->spawned > 0u ||
        g->phase != ASX_TASK_GROUP_COLLECTING) {
        return ASX_E_INVALID_STATE;
    }
    if (g->count >= ASX_TASK_GROUP_MAX) return ASX_E_RESOURCE_EXHAUSTED;
    g->attempt_fns[g->count] = poll_fn;
    g->attempt_data[g->count] = user_data;
    g->attempt_regions[g->count] = region;
    g->members[g->count] = ASX_INVALID_ID;
    g->outcomes[g->count] = asx_outcome_make(ASX_OUTCOME_OK);
    g->statuses[g->count] = ASX_E_PENDING;
    g->completed[g->count] = 0;
    g->cancel_sent[g->count] = 0;
    g->count++;
    return ASX_OK;
}

asx_status asx_task_group_poll(asx_task_group *g, asx_task_id self) {
    asx_task_slot *owner;
    asx_status st;

    if (g == NULL) return ASX_E_INVALID_ARGUMENT;
    if (g->phase == ASX_TASK_GROUP_DONE) return g->result;
    st = asx_task_slot_lookup(self, &owner);
    if (st != ASX_OK) return st;
    if (g->mode == ASX_TASK_GROUP_FIRST_OK) return group_first_ok_poll(g, owner, self);

    group_collect(g, owner);
    if (g->phase == ASX_TASK_GROUP_COLLECTING) group_decide(g, owner, self);
    if (g->phase == ASX_TASK_GROUP_DRAINING) {
        group_send_cancels(g);
        group_collect(g, owner);
        if (g->completed_count == g->count) {
            g->phase = ASX_TASK_GROUP_DONE;
            /* An invalid threshold, a deadline or an explicit cancel keeps
             * its result; otherwise every member's final outcome counts. */
            if (g->mode == ASX_TASK_GROUP_QUORUM && (!g->early || g->owner_cancelled)) {
                g->result = group_quorum_result(g);
            }
            return g->result;
        }
    }

    /* Still waiting: members wake the owner as they complete. */
    if (g->phase == ASX_TASK_GROUP_COLLECTING && g->deadline != 0u) {
        st = asx_task_arm_timer(self, g->deadline);
        (void)st;
    }
    if (owner->in_poll) owner->park_requested = 1;
    return ASX_E_PENDING;
}

asx_status asx_task_group_cancel(asx_task_group *g, asx_cancel_kind kind) {
    if (g == NULL) return ASX_E_INVALID_ARGUMENT;
    if (g->phase != ASX_TASK_GROUP_COLLECTING) return ASX_OK;
    if (g->mode == ASX_TASK_GROUP_FIRST_OK) {
        /* No further attempt; the running one is cancelled and awaited. */
        g->early = 1u;
        g->result = ASX_E_CANCELLED;
        if (g->spawned > 0u && !g->completed[g->spawned - 1u] && !g->forwarded) {
            asx_status st = asx_task_cancel(g->members[g->spawned - 1u], kind);
            (void)st;
            g->forwarded = 1u;
        }
        return ASX_OK;
    }
    group_begin_drain(g, kind, ASX_E_CANCELLED, 1);
    group_send_cancels(g);
    return ASX_OK;
}

int32_t asx_task_group_winner(const asx_task_group *g) { return g != NULL ? g->winner : -1; }

uint32_t asx_task_group_ok_count(const asx_task_group *g) { return g != NULL ? g->ok_count : 0u; }

asx_task_group_phase asx_task_group_phase_of(const asx_task_group *g) {
    return g != NULL ? g->phase : ASX_TASK_GROUP_DONE;
}

asx_outcome asx_task_group_outcome(const asx_task_group *g) {
    asx_outcome acc = asx_outcome_make(ASX_OUTCOME_OK);
    uint32_t i;

    if (g == NULL) return acc;
    if (!g->early && g->winner >= 0) return g->outcomes[g->winner];
    if (!g->early && g->mode == ASX_TASK_GROUP_QUORUM && g->phase != ASX_TASK_GROUP_COLLECTING &&
        g->result == ASX_OK) {
        return acc;
    }
    for (i = 0; i < g->count; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: count <= ASX_TASK_GROUP_MAX");
        if (!g->completed[i] || g->cancel_sent[i]) continue;
        acc = asx_outcome_join(&acc, &g->outcomes[i]);
    }
    if (g->early) {
        asx_outcome cancelled = asx_outcome_make(ASX_OUTCOME_CANCELLED);
        acc = asx_outcome_join(&acc, &cancelled);
    }
    return acc;
}

asx_status asx_task_group_member_result(const asx_task_group *g, uint32_t index,
                                        asx_outcome *out_outcome, asx_status *out_status) {
    if (g == NULL || index >= g->count) return ASX_E_INVALID_ARGUMENT;
    if (out_outcome == NULL && out_status == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!g->completed[index]) return ASX_E_TASK_NOT_COMPLETED;
    if (out_outcome != NULL) *out_outcome = g->outcomes[index];
    if (out_status != NULL) *out_status = g->statuses[index];
    return ASX_OK;
}
