/*
 * asx/runtime/task_group.h — structured combinators over spawned tasks
 *
 * A task group gathers spawned tasks and lets one owner task wait on them
 * under a completion mode:
 *
 *   JOIN_ALL  every member completes; succeeds when all succeeded. As Rust's
 *             join_all, the members are awaited one by one in order and
 *             the owner's own cancel does not reach them.
 *   RACE      the first member to complete wins (its result is the group's).
 *   FIRST_OK  attempts run one at a time, in order, until one completes OK
 *             (asx_task_group_add_attempt; Rust Scope::first_ok).
 *   QUORUM    `needed` members complete OK; fails once that is impossible.
 *
 * Losers are drained. Once the group is decided, every unfinished member
 * is cancelled (ASX_CANCEL_RACE_LOST) and the owner keeps waiting until
 * each one has completed: the group never resolves while a member it
 * started is still running, so loser obligations, finalizers and handles
 * are resolved rather than abandoned. The same holds when the group ends
 * early:
 *
 *   - owner cancelled  -> members cancelled (PARENT; QUORUM: with the owner's
 *                         own reason), drained, ASX_E_CANCELLED
 *   - deadline reached -> members cancelled (TIMEOUT), drained,
 *                         ASX_E_TIMED_OUT (ASX_E_THRESHOLD_TIMEOUT for QUORUM)
 *   - asx_task_group_cancel() -> members cancelled with the given kind,
 *                         drained, ASX_E_CANCELLED
 *
 * The owner drives the group from its poll function:
 *
 *     static asx_status owner_poll(void *ud, asx_task_id self) {
 *         my_state *s = ud;
 *         return asx_task_group_poll(&s->group, self);
 *     }
 *
 * asx_task_group_poll() parks the owner and returns ASX_E_PENDING while
 * the group is unresolved; members wake the owner as they complete. The
 * final status is the group's result (stable across further polls):
 *
 *   JOIN_ALL  ASX_OK, or the status of the most severe failing member
 *   RACE      the winner's status (ASX_OK, its error, or ASX_E_CANCELLED);
 *             once the owner's cancel is requested its checkpoint observes
 *             it (Rust race_all, cx/scope.rs:1371-1375): every unfinished
 *             member is aborted with the owner's own reason and joined in
 *             order, and the result is ASX_E_CANCELLED, or the status of the
 *             first member that panicked (drain_owner_cancelled_race)
 *   FIRST_OK  ASX_OK, or the status of the attempt that ended it (see
 *             asx_task_group_add_attempt)
 *   QUORUM    as Rust's Scope::quorum (cx/scope.rs:1811-1941, quorum.rs:398):
 *             a member that panicked: its status (ASX_E_INVALID_STATE), even
 *             when the quorum was met; met: ASX_OK; the owner cancelled
 *             first: ASX_E_CANCELLED; a member cancelled by anything but the
 *             group's RACE_LOST drain: ASX_E_CANCELLED; otherwise the first
 *             failing member's error (by index)
 *
 * A member's status is ASX_OK for an OK outcome, the error its poll
 * function returned for ERR, and ASX_E_CANCELLED for CANCELLED. Among
 * members completing in the same poll, the lowest index wins
 * (deterministic). Members are joined (their slots released) as they are
 * collected; their outcomes stay readable through the group.
 *
 * Members must be scheduled by the same asx_scheduler_run() subtree as
 * the owner, and must not be joined or detached by anyone else.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_RUNTIME_TASK_GROUP_H
#define ASX_RUNTIME_TASK_GROUP_H

#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>
#include <asx/core/outcome.h>
#include <asx/runtime/runtime.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ASX_TASK_GROUP_MAX
#define ASX_TASK_GROUP_MAX 16u
#endif

typedef enum {
    ASX_TASK_GROUP_JOIN_ALL = 0,
    ASX_TASK_GROUP_RACE = 1,
    ASX_TASK_GROUP_FIRST_OK = 2,
    ASX_TASK_GROUP_QUORUM = 3
} asx_task_group_mode;

typedef enum {
    ASX_TASK_GROUP_COLLECTING = 0, /* waiting for the group to be decided */
    ASX_TASK_GROUP_DRAINING = 1,   /* decided; cancelling and awaiting losers */
    ASX_TASK_GROUP_DONE = 2        /* every member completed; result final */
} asx_task_group_phase;

typedef struct {
    asx_task_id members[ASX_TASK_GROUP_MAX];
    asx_outcome outcomes[ASX_TASK_GROUP_MAX];
    asx_status statuses[ASX_TASK_GROUP_MAX]; /* member status once completed */
    uint8_t completed[ASX_TASK_GROUP_MAX];
    uint8_t cancel_sent[ASX_TASK_GROUP_MAX];
    uint32_t count;
    uint32_t completed_count;
    uint32_t ok_count;
    uint32_t needed; /* QUORUM threshold */
    asx_task_group_mode mode;
    asx_task_group_phase phase;
    int32_t winner;             /* RACE / FIRST_OK winner index, -1 if none */
    asx_cancel_kind drain_kind; /* cancel kind sent to unfinished members */
    asx_status result;          /* group result, final once DONE */
    asx_time deadline;          /* 0 = none */
    uint8_t early;              /* resolved by cancel / deadline / bad quorum */
    /* FIRST_OK attempts (asx_task_group_add_attempt), spawned one at a
     * time: members[i] is valid for i < spawned. */
    asx_task_poll_fn attempt_fns[ASX_TASK_GROUP_MAX];
    void *attempt_data[ASX_TASK_GROUP_MAX];
    asx_region_id attempt_regions[ASX_TASK_GROUP_MAX];
    uint32_t spawned;
    uint8_t forwarded; /* a cancel was passed to the running attempt */
    /* The owner's budget when the group was first polled: every attempt
     * runs with it (Rust's cx.scope() snapshots Cx::inherited_budget). */
    asx_budget attempt_budget;
    uint8_t attempt_budget_set;
    /* QUORUM: the cancel kind a member that ended CANCELLED carried. QUORUM
     * and RACE: whether the owner's cancel ended collection (its reason
     * drains the members, as Rust's quorum and race_all drain with the
     * caller's reason). */
    asx_cancel_kind cancel_kinds[ASX_TASK_GROUP_MAX];
    uint8_t owner_cancelled;
    asx_cancel_reason owner_reason;
    /* Members whose spawn the lab refused (asx_task_group_member_refusal):
     * the lab refusal ticket, and the reason their join reports. */
    uint8_t refused[ASX_TASK_GROUP_MAX];
    uint32_t refusal[ASX_TASK_GROUP_MAX];
    asx_cancel_reason refused_reason[ASX_TASK_GROUP_MAX];
} asx_task_group;

/* Initialize an empty group. `needed` is the QUORUM threshold (1..count,
 * checked at the first poll) and is ignored by the other modes.
 * Returns ASX_E_INVALID_ARGUMENT for NULL or an unknown mode. */
ASX_API ASX_MUST_USE asx_status asx_task_group_init(asx_task_group *g, asx_task_group_mode mode,
                                                    uint32_t needed);

/* Resolve the group as timed out if it is still undecided at `deadline`
 * (runtime clock; 0 clears). */
ASX_API ASX_MUST_USE asx_status asx_task_group_set_deadline(asx_task_group *g, asx_time deadline);

/* Adopt an already-spawned task as a member. The group owns its
 * completion: it must not be joined or detached elsewhere.
 * Returns ASX_E_RESOURCE_EXHAUSTED when full, ASX_E_INVALID_STATE once the
 * group is decided or for a detached task, or a lookup error. */
ASX_API ASX_MUST_USE asx_status asx_task_group_add(asx_task_group *g, asx_task_id task);

/* Spawn a task in `region` and adopt it as a member. Capacity is checked
 * before spawning, so a full group spawns nothing. Not for FIRST_OK
 * (ASX_E_INVALID_STATE): its attempts run one at a time. Under lab
 * dispatch a spawn from a poll that the closing region refuses still adds
 * a member, with *out_id = ASX_INVALID_ID (asx_task_group_member_refusal);
 * otherwise a refused spawn returns its error and adds nothing. */
ASX_API ASX_MUST_USE asx_status asx_task_group_spawn(asx_task_group *g, asx_region_id region,
                                                     asx_task_poll_fn poll_fn, void *user_data,
                                                     asx_task_id *out_id);

/* FIRST_OK only: register an attempt, spawned in `region` when its turn
 * comes (Rust Scope::first_ok, cx/scope.rs:2023-2099). The owner's polls
 * run the attempts in registration order, one at a time:
 *   - before each attempt the owner checkpoints; once it is cancelled no
 *     further attempt starts and the group resolves ASX_E_CANCELLED;
 *   - an attempt runs cancellation-dominant; a cancel of the owner while
 *     it runs is passed to it once (a handle abort with the owner's reason)
 *     and the attempt is still awaited to completion;
 *   - an attempt completing OK wins (ASX_OK, asx_task_group_winner);
 *     an ERR moves on to the next attempt; CANCELLED or PANICKED ends the
 *     group with that member's status;
 *   - every attempt failed: the most severe member status (the first
 *     error among equals); no attempt registered: ASX_E_INVALID_ARGUMENT.
 * A spawn refused when an attempt's turn comes ends the group
 * ASX_E_CANCELLED: under lab dispatch once the next step's admission
 * refuses it and its join reports Cancelled (asx_task_group_member_refusal),
 * otherwise at once (Rust: Cancelled(resource_unavailable)).
 * Returns ASX_E_INVALID_STATE for another mode or once the group started,
 * ASX_E_RESOURCE_EXHAUSTED when full, ASX_E_INVALID_ARGUMENT for a NULL
 * poll function. */
ASX_API ASX_MUST_USE asx_status asx_task_group_add_attempt(asx_task_group *g, asx_region_id region,
                                                           asx_task_poll_fn poll_fn,
                                                           void *user_data);

/* Drive the group from the owner task `self`. Returns ASX_E_PENDING (owner
 * parked until a member completes or the deadline passes) while
 * unresolved, then the group result. */
ASX_API ASX_MUST_USE asx_status asx_task_group_poll(asx_task_group *g, asx_task_id self);

/* Abandon the group: cancel every unfinished member with `kind`. The next
 * polls drain them and resolve the group as ASX_E_CANCELLED. No effect
 * once the group is already decided. */
ASX_API ASX_MUST_USE asx_status asx_task_group_cancel(asx_task_group *g, asx_cancel_kind kind);

/* Winner index for RACE / FIRST_OK, -1 when none. */
ASX_API int32_t asx_task_group_winner(const asx_task_group *g);

/* Number of members that completed OK. */
ASX_API uint32_t asx_task_group_ok_count(const asx_task_group *g);

/* Current phase. */
ASX_API asx_task_group_phase asx_task_group_phase_of(const asx_task_group *g);

/* Aggregate outcome: JOIN_ALL joins every member outcome; RACE / FIRST_OK
 * report the winner's; QUORUM is OK when achieved. Early resolution
 * (cancel, deadline) and failed groups report the most severe member
 * outcome, at least CANCELLED for cancel/deadline. */
ASX_API asx_outcome asx_task_group_outcome(const asx_task_group *g);

/* Outcome and status of member `index` once it has completed.
 * Returns ASX_E_INVALID_ARGUMENT for a bad index or NULL outputs,
 * ASX_E_TASK_NOT_COMPLETED while it is still running. */
ASX_API ASX_MUST_USE asx_status asx_task_group_member_result(const asx_task_group *g,
                                                             uint32_t index,
                                                             asx_outcome *out_outcome,
                                                             asx_status *out_status);

/* A member whose spawn was refused under lab dispatch: spawned from a poll
 * into a closing or closed region, it is still a member, as Rust's spawn
 * mailbox keeps the child until the next step's admission refuses it
 * (asx_task_group_spawn gives it ASX_INVALID_ID). From that step its join
 * reports JoinError::Cancelled with a ParentCancelled reason with
 * testing-default attribution (the denial, lab/runtime.rs:3994-4008),
 * strengthened by the abort reasons the group sent it
 * (task_handle.rs:544-554), and the group counts it as a CANCELLED
 * member.
 * Returns ASX_OK with *out once the refusal was delivered, ASX_E_PENDING
 * before, ASX_E_NOT_FOUND for a member that was not refused, or
 * ASX_E_INVALID_ARGUMENT for a bad index or NULL argument. */
ASX_API ASX_MUST_USE asx_status asx_task_group_member_refusal(const asx_task_group *g,
                                                              uint32_t index,
                                                              asx_cancel_reason *out);

#ifdef __cplusplus
}
#endif

#endif /* ASX_RUNTIME_TASK_GROUP_H */
