/*
 * scheduler.c — deterministic, wake-driven scheduler with event sequencing
 *
 * The scheduler drives every task in a region subtree. Within a round,
 * runnable tasks are polled in ascending arena index order (deterministic
 * tie-break). A task that returns ASX_E_PENDING stays runnable (yield)
 * unless it parked itself with asx_task_park(); parked tasks are skipped
 * until woken by a timer, a join target completing, a waker/IO signal, a
 * cancel request, or user code.
 *
 * When every live task in scope is parked the scheduler goes idle:
 *   1. cancel-pending parked tasks are unparked (bounded cleanup must make
 *      progress, so cancellation never waits on a wake that may not come);
 *   2. signaled wakers are drained;
 *   3. due task timers fire; with the runtime's virtual clock time jumps
 *      directly to the earliest armed timer (lab-runtime semantics); with
 *      a real clock the reactor wait hook blocks until the deadline or
 *      I/O readiness;
 *   4. otherwise the run returns ASX_E_WOULD_BLOCK.
 *
 * Task timers live in a binary min-heap keyed by (deadline, arm sequence),
 * i.e. an EDF timed lane with insertion-stable ties.
 *
 * SPDX-License-Identifier: MIT
 */

#include "runtime_internal.h"
#include <asx/asx.h>
#include <asx/asx_config.h>
#include <asx/core/cancel.h>
#include <asx/core/transition.h>
#include <asx/runtime/io_driver.h>
#include <asx/runtime/runtime.h>
#include <asx/runtime/waker.h>
#define ASX_INTERNAL_TRACE_FAMILY_ACCESS 1
#include <asx/runtime/trace.h>
#undef ASX_INTERNAL_TRACE_FAMILY_ACCESS

/* -------------------------------------------------------------------
 * Event log (ring buffer for deterministic sequencing)
 * ------------------------------------------------------------------- */

#define ASX_SCHED_EVENT_LOG_CAPACITY 256u

/* Consecutive idle passes without clock movement before a run with a
 * frozen custom clock gives up instead of spinning forever. */
#define ASX_SCHED_IDLE_STALL_LIMIT 4096u

/* Upper bound for a single reactor wait while idle (milliseconds). */
#define ASX_SCHED_MAX_IDLE_WAIT_MS 1000u

static asx_scheduler_event g_event_log[ASX_SCHED_EVENT_LOG_CAPACITY];
static uint32_t g_event_count = 0;

static void sched_emit(asx_scheduler_event_kind kind, asx_task_id tid, uint32_t round) {
    if (g_event_count < ASX_SCHED_EVENT_LOG_CAPACITY) {
        asx_scheduler_event *e = &g_event_log[g_event_count];
        e->kind = kind;
        e->task_id = tid;
        e->sequence = g_event_count;
        e->round = round;
    }
    /* Saturate at UINT32_MAX to prevent wrap-around losing event history */
    if (g_event_count < UINT32_MAX) { g_event_count++; }
}

static uint64_t asx_trace_task_transition_aux(asx_task_state from, asx_task_state to) {
    return ((uint64_t)(uint32_t)from << 32) | (uint64_t)(uint32_t)to;
}

uint32_t asx_scheduler_event_count(void) { return g_event_count; }

int asx_scheduler_event_get(uint32_t index, asx_scheduler_event *out) {
    if (out == NULL) return 0;
    if (index >= g_event_count) return 0;
    if (index >= ASX_SCHED_EVENT_LOG_CAPACITY) return 0;
    *out = g_event_log[index];
    return 1;
}

void asx_scheduler_event_reset(void) { g_event_count = 0; }

/* -------------------------------------------------------------------
 * Task timer heap (EDF timed lane)
 * ------------------------------------------------------------------- */

static uint32_t g_timer_heap[ASX_MAX_TASKS];
static uint32_t g_timer_heap_len = 0;
static uint64_t g_timer_seq = 0;

static int timer_less(uint32_t a, uint32_t b) {
    const asx_task_slot *ta = &g_tasks[a];
    const asx_task_slot *tb = &g_tasks[b];
    if (ta->wake_at != tb->wake_at) return ta->wake_at < tb->wake_at;
    return ta->timer_seq < tb->timer_seq;
}

static void timer_heap_place(uint32_t pos, uint32_t idx) {
    g_timer_heap[pos] = idx;
    g_tasks[idx].timer_pos = pos;
}

static void timer_heap_sift_up(uint32_t pos) {
    uint32_t idx = g_timer_heap[pos];
    while (pos > 0u) {
        ASX_CHECKPOINT_WAIVER("bounded: heap depth <= log2(ASX_MAX_TASKS)");
        uint32_t parent = (pos - 1u) / 2u;
        if (!timer_less(idx, g_timer_heap[parent])) break;
        timer_heap_place(pos, g_timer_heap[parent]);
        pos = parent;
    }
    timer_heap_place(pos, idx);
}

static void timer_heap_sift_down(uint32_t pos) {
    uint32_t idx = g_timer_heap[pos];
    for (;;) {
        ASX_CHECKPOINT_WAIVER("bounded: heap depth <= log2(ASX_MAX_TASKS)");
        uint32_t left = 2u * pos + 1u;
        uint32_t best;
        if (left >= g_timer_heap_len) break;
        best = left;
        if (left + 1u < g_timer_heap_len &&
            timer_less(g_timer_heap[left + 1u], g_timer_heap[left])) {
            best = left + 1u;
        }
        if (!timer_less(g_timer_heap[best], idx)) break;
        timer_heap_place(pos, g_timer_heap[best]);
        pos = best;
    }
    timer_heap_place(pos, idx);
}

static void timer_heap_remove_at(uint32_t pos) {
    uint32_t removed = g_timer_heap[pos];
    g_timer_heap_len--;
    g_tasks[removed].timer_pos = ASX_SLOT_NONE;
    if (pos == g_timer_heap_len) return;
    timer_heap_place(pos, g_timer_heap[g_timer_heap_len]);
    if (pos > 0u && timer_less(g_timer_heap[pos], g_timer_heap[(pos - 1u) / 2u])) {
        timer_heap_sift_up(pos);
    } else {
        timer_heap_sift_down(pos);
    }
}

void asx_task_timer_disarm_internal(asx_task_slot *task) {
    if (task == NULL || task->timer_pos == ASX_SLOT_NONE) return;
    timer_heap_remove_at(task->timer_pos);
}

/* Arm (or tighten) a task's timer. Keeps the earliest deadline. */
static void timer_arm(uint32_t idx, asx_time deadline) {
    asx_task_slot *t = &g_tasks[idx];
    if (t->timer_pos != ASX_SLOT_NONE) {
        if (deadline >= t->wake_at) return;
        t->wake_at = deadline;
        t->timer_seq = g_timer_seq++;
        timer_heap_sift_up(t->timer_pos);
        return;
    }
    t->wake_at = deadline;
    t->timer_seq = g_timer_seq++;
    timer_heap_place(g_timer_heap_len, idx);
    g_timer_heap_len++;
    timer_heap_sift_up(g_timer_heap_len - 1u);
}

/* Fire every task timer due at `now`, in (deadline, seq) order.
 * Returns the number of tasks woken. */
static uint32_t timers_fire(asx_time now) {
    uint32_t fired = 0;
    while (g_timer_heap_len > 0u) {
        ASX_CHECKPOINT_WAIVER("bounded: each iteration removes one heap entry");
        uint32_t idx = g_timer_heap[0];
        if (g_tasks[idx].wake_at > now) break;
        timer_heap_remove_at(0u);
        asx_task_wake_slot_internal(&g_tasks[idx]);
        fired++;
    }
    return fired;
}

/* -------------------------------------------------------------------
 * Task scheduling state
 * ------------------------------------------------------------------- */

void asx_scheduler_reset_internal(void) {
    g_timer_heap_len = 0;
    g_timer_seq = 0;
}

void asx_task_sched_init_internal(asx_task_slot *task) {
    task->in_poll = 0;
    task->park_requested = 0;
    task->notified = 0;
    task->parked = 0;
    task->last_error = ASX_OK;
    task->wake_at = 0;
    task->timer_seq = 0;
    task->timer_pos = ASX_SLOT_NONE;
    task->first_waiter = ASX_SLOT_NONE;
    task->next_waiter = ASX_SLOT_NONE;
    task->waiting_on = ASX_SLOT_NONE;
}

void asx_task_wake_slot_internal(asx_task_slot *task) {
    if (task == NULL || !task->alive) return;
    if (asx_task_is_terminal(task->state)) return;
    if (task->in_poll) {
        task->notified = 1;
        return;
    }
    task->parked = 0;
}

void asx_task_join_detach_internal(asx_task_slot *task) {
    uint32_t self_idx;
    uint32_t *link;

    if (task == NULL || task->waiting_on == ASX_SLOT_NONE) return;
    self_idx = (uint32_t)(task - g_tasks);
    link = &g_tasks[task->waiting_on].first_waiter;
    while (*link != ASX_SLOT_NONE) {
        ASX_CHECKPOINT_WAIVER("bounded: waiter list length <= ASX_MAX_TASKS");
        if (*link == self_idx) {
            *link = task->next_waiter;
            break;
        }
        link = &g_tasks[*link].next_waiter;
    }
    task->waiting_on = ASX_SLOT_NONE;
    task->next_waiter = ASX_SLOT_NONE;
}

void asx_task_join_wake_waiters_internal(asx_task_slot *task) {
    uint32_t w;

    if (task == NULL) return;
    w = task->first_waiter;
    task->first_waiter = ASX_SLOT_NONE;
    while (w != ASX_SLOT_NONE) {
        ASX_CHECKPOINT_WAIVER("bounded: waiter list length <= ASX_MAX_TASKS");
        asx_task_slot *ws = &g_tasks[w];
        uint32_t next = ws->next_waiter;
        ws->next_waiter = ASX_SLOT_NONE;
        ws->waiting_on = ASX_SLOT_NONE;
        asx_task_wake_slot_internal(ws);
        w = next;
    }
}

/* Clock read for scheduling decisions: the runtime clock when hooks are
 * installed, the virtual clock otherwise. */
static asx_time sched_now(void) {
    asx_time now;
    if (asx_runtime_now_ns(&now) != ASX_OK) now = asx_runtime_virtual_now();
    return now;
}

/* -------------------------------------------------------------------
 * Public wake/park/timer/join API
 * ------------------------------------------------------------------- */

asx_status asx_task_park(asx_task_id self) {
    asx_task_slot *t;
    asx_status st = asx_task_slot_lookup(self, &t);
    if (st != ASX_OK) return st;
    if (!t->in_poll) return ASX_E_INVALID_STATE;
    t->park_requested = 1;
    return ASX_OK;
}

asx_status asx_task_wake(asx_task_id id) {
    asx_task_slot *t;
    asx_status st = asx_task_slot_lookup(id, &t);
    if (st != ASX_OK) return st;
    asx_task_wake_slot_internal(t);
    return ASX_OK;
}

asx_status asx_task_arm_timer(asx_task_id self, asx_time deadline) {
    asx_task_slot *t;
    asx_status st = asx_task_slot_lookup(self, &t);
    if (st != ASX_OK) return st;
    if (asx_task_is_terminal(t->state)) return ASX_E_INVALID_STATE;
    timer_arm((uint32_t)(t - g_tasks), deadline);
    return ASX_OK;
}

asx_status asx_task_wait_until(asx_task_id self, asx_time deadline) {
    asx_task_slot *t;
    asx_time now = sched_now();

    if (now >= deadline) return ASX_OK;
    if (asx_task_slot_lookup(self, &t) == ASX_OK && t->in_poll) {
        timer_arm((uint32_t)(t - g_tasks), deadline);
        t->park_requested = 1;
    }
    return ASX_E_PENDING;
}

asx_status asx_task_join_poll(asx_task_id self, asx_task_id target, asx_outcome *out_outcome) {
    asx_task_slot *tt;
    asx_task_slot *ts;
    uint32_t self_idx;
    uint32_t target_idx;
    asx_status st;

    st = asx_task_slot_lookup(target, &tt);
    if (st != ASX_OK) return st;
    st = asx_task_slot_lookup(self, &ts);
    if (st != ASX_OK) return st;
    if (ts == tt) return ASX_E_INVALID_ARGUMENT;

    if (asx_task_is_terminal(tt->state)) {
        if (ts->waiting_on != ASX_SLOT_NONE) asx_task_join_detach_internal(ts);
        return asx_task_join(target, out_outcome);
    }

    self_idx = (uint32_t)(ts - g_tasks);
    target_idx = (uint32_t)(tt - g_tasks);
    if (ts->waiting_on != target_idx) {
        asx_task_join_detach_internal(ts);
        ts->next_waiter = tt->first_waiter;
        tt->first_waiter = self_idx;
        ts->waiting_on = target_idx;
    }
    if (ts->in_poll) ts->park_requested = 1;
    return ASX_E_PENDING;
}

asx_status asx_task_get_error(asx_task_id id, asx_status *out_error) {
    asx_task_slot *t;
    asx_status st;

    if (out_error == NULL) return ASX_E_INVALID_ARGUMENT;
    st = asx_task_slot_lookup(id, &t);
    if (st != ASX_OK) return st;
    if (!asx_task_is_terminal(t->state)) return ASX_E_TASK_NOT_COMPLETED;
    *out_error = t->last_error;
    return ASX_OK;
}

/* -------------------------------------------------------------------
 * Scope (region subtree) tracking
 * ------------------------------------------------------------------- */

static uint32_t g_scope_slots[ASX_MAX_REGIONS];
static uint32_t g_scope_mark[ASX_MAX_REGIONS];
static uint32_t g_scope_epoch = 0;

static void sched_compute_scope(asx_region_id root) {
    uint32_t n;
    uint32_t i;

    g_scope_epoch++;
    if (g_scope_epoch == 0u) {
        /* Epoch wrapped: clear stale marks once. */
        for (i = 0; i < ASX_MAX_REGIONS; i++) g_scope_mark[i] = 0u;
        g_scope_epoch = 1u;
    }
    n = asx_region_subtree_internal(root, g_scope_slots, ASX_MAX_REGIONS);
    for (i = 0; i < n; i++) g_scope_mark[g_scope_slots[i]] = g_scope_epoch;
}

/* Resolve a live task's region slot if it lies in the current scope. */
static asx_region_slot *sched_scope_region(const asx_task_slot *t) {
    uint32_t slot = asx_handle_slot(t->region);
    if (slot >= ASX_MAX_REGIONS) return NULL;
    if (g_scope_mark[slot] != g_scope_epoch) return NULL;
    if (!g_regions[slot].alive) return NULL;
    if (g_regions[slot].generation != asx_handle_generation(t->region)) return NULL;
    return &g_regions[slot];
}

/* -------------------------------------------------------------------
 * Completion helpers
 * ------------------------------------------------------------------- */

/* Advance a cancelled task's phase/witness to COMPLETED. */
static void sched_cancel_phase_complete(asx_task_slot *t) {
    asx_status w_st_;
    t->cancel_phase = ASX_CANCEL_PHASE_COMPLETED;
    w_st_ = asx_cancel_witness_advance(t->cancel_witness, ASX_CANCEL_PHASE_COMPLETED);
    (void)w_st_;
    w_st_ = asx_cancel_witness_release(t->cancel_witness);
    (void)w_st_;
}

/* Drive a task to COMPLETED with the given severity. Emits the transition
 * trace, runs completion bookkeeping, and logs `ev`. The slot may be
 * released (detached task) when this returns: do not touch `t` after. */
static void sched_complete(asx_task_slot *t, asx_task_id tid, asx_region_slot *rslot,
                           asx_outcome_severity severity, asx_scheduler_event_kind ev,
                           uint32_t round) {
    asx_task_state from = t->state;
    (void)asx_ghost_check_task_transition(tid, t->state, ASX_TASK_COMPLETED);
    t->state = ASX_TASK_COMPLETED;
    if (t->cancel_pending) sched_cancel_phase_complete(t);
    asx_trace_emit(ASX_TRACE_TASK_TRANSITION, (uint64_t)tid,
                   asx_trace_task_transition_aux(from, ASX_TASK_COMPLETED));
    t->outcome = asx_outcome_make(severity);
    asx_task_on_complete_internal(t, rslot);
    sched_emit(ev, tid, round);
    asx_trace_emit(ASX_TRACE_SCHED_COMPLETE, (uint64_t)tid, round);
}

static void sched_transition(asx_task_slot *t, asx_task_id tid, asx_task_state to,
                             asx_cancel_phase phase) {
    asx_task_state from = t->state;
    asx_status w_st_;
    (void)asx_ghost_check_task_transition(tid, t->state, to);
    t->state = to;
    t->cancel_phase = phase;
    w_st_ = asx_cancel_witness_advance(t->cancel_witness, phase);
    (void)w_st_;
    asx_trace_emit(ASX_TRACE_TASK_TRANSITION, (uint64_t)tid,
                   asx_trace_task_transition_aux(from, to));
}

/* -------------------------------------------------------------------
 * Idle handling
 * ------------------------------------------------------------------- */

/* Wake every task whose waker was signaled. Returns the count woken. */
static uint32_t sched_drain_wakers(void) {
    asx_task_id woken[16];
    uint32_t total = 0;
    uint32_t n;

    do {
        uint32_t i;
        ASX_CHECKPOINT_WAIVER("bounded: each pass clears up to 16 signaled wakers");
        n = asx_waker_drain_signaled(woken, 16u);
        for (i = 0; i < n; i++) {
            if (asx_task_wake(woken[i]) == ASX_OK) total++;
        }
    } while (n == 16u);
    return total;
}

/* Unpark every cancel-pending task in scope. Returns the count. */
static uint32_t sched_unpark_cancelled(void) {
    uint32_t i;
    uint32_t n = 0;
    for (i = 0; i < g_task_count; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: g_task_count <= ASX_MAX_TASKS");
        asx_task_slot *t = &g_tasks[i];
        if (!t->alive || !t->parked || !t->cancel_pending) continue;
        if (sched_scope_region(t) == NULL) continue;
        t->parked = 0;
        n++;
    }
    return n;
}

static int sched_io_pending(void) {
#if ASX_HAS_NATIVE_IO_DRIVER
    return asx_io_driver_is_initialized() && asx_io_active_count() > 0u;
#else
    return 0;
#endif
}

/* Block for up to timeout_ms waiting for I/O (or just time to pass). */
static void sched_block(uint32_t timeout_ms) {
#if ASX_HAS_NATIVE_IO_DRIVER
    if (sched_io_pending()) {
        asx_io_event events[8];
        (void)asx_io_driver_poll(events, 8u, timeout_ms);
        return;
    }
#endif
    {
        uint32_t ready = 0;
        (void)asx_runtime_reactor_wait(timeout_ms, &ready, 0u);
    }
}

/* Called when every live task in scope is parked. Returns ASX_OK if the
 * scheduler should run another round, ASX_E_WOULD_BLOCK if nothing can
 * wake the parked tasks. */
static asx_status sched_idle(asx_time *last_idle_now, uint32_t *stall) {
    asx_time now;

    if (sched_unpark_cancelled() > 0u) return ASX_OK;
    if (sched_drain_wakers() > 0u) return ASX_OK;

    if (g_timer_heap_len > 0u) {
        asx_time next = g_tasks[g_timer_heap[0]].wake_at;
        now = sched_now();
        if (now < next && asx_runtime_clock_is_virtual()) {
            asx_runtime_virtual_advance(next);
            now = sched_now();
        }
        if (timers_fire(now) > 0u) return ASX_OK;
        if (now < next) {
            uint64_t wait_ns = next - now;
            uint64_t wait_ms = (wait_ns + 999999u) / 1000000u;
            if (wait_ms > ASX_SCHED_MAX_IDLE_WAIT_MS) wait_ms = ASX_SCHED_MAX_IDLE_WAIT_MS;
            sched_block((uint32_t)wait_ms);
            (void)sched_drain_wakers();
            now = sched_now();
            (void)timers_fire(now);
        }
    } else if (sched_io_pending()) {
        sched_block(ASX_SCHED_MAX_IDLE_WAIT_MS);
        (void)sched_drain_wakers();
        now = sched_now();
    } else {
        return ASX_E_WOULD_BLOCK;
    }

    /* Guard against a frozen custom clock: if repeated idle passes see no
     * clock movement, stop rather than spin. */
    if (now == *last_idle_now) {
        if (++(*stall) >= ASX_SCHED_IDLE_STALL_LIMIT) return ASX_E_WOULD_BLOCK;
    } else {
        *stall = 0;
        *last_idle_now = now;
    }
    return ASX_OK;
}

/* -------------------------------------------------------------------
 * Scheduler: run all tasks in a region subtree until completion,
 * budget exhaustion, or an unwakeable idle state.
 *
 * Ordering invariant: runnable tasks are polled in ascending arena
 * index within each round. This produces a deterministic event stream
 * for any given input and seed combination.
 * ------------------------------------------------------------------- */

asx_status asx_scheduler_run(asx_region_id region, asx_budget *budget) {
    asx_region_slot *root;
    asx_status st;
    uint32_t round;
    asx_time last_idle_now = 0;
    uint32_t idle_stall = 0;

    if (budget == NULL) return ASX_E_INVALID_ARGUMENT;

    st = asx_region_slot_lookup(region, &root);
    if (st != ASX_OK) return st;

    /* Reset event log for this scheduler invocation */
    asx_scheduler_event_reset();

    for (round = 0;; round++) {
        uint32_t active = 0;
        uint32_t progress = 0;
        uint32_t i;

        ASX_CHECKPOINT_WAIVER("kernel-scheduler: this IS the scheduler event loop; "
                              "budget exhaustion provides bounded termination");
        asx_trace_emit(ASX_TRACE_SCHED_ROUND, ASX_INVALID_ID, round);

        /* Check budget exhaustion */
        if (asx_budget_is_exhausted(budget)) {
            sched_emit(ASX_SCHED_EVENT_BUDGET, ASX_INVALID_ID, round);
            asx_trace_emit(ASX_TRACE_SCHED_BUDGET, ASX_INVALID_ID, round);
            return ASX_E_POLL_BUDGET_EXHAUSTED;
        }

        sched_compute_scope(region);
        (void)sched_drain_wakers();
        if (g_timer_heap_len > 0u) (void)timers_fire(sched_now());

        for (i = 0; i < g_task_count; i++) {
            ASX_CHECKPOINT_WAIVER("kernel-scheduler: inner poll loop bounded by "
                                  "g_task_count <= ASX_MAX_TASKS arena capacity");
            asx_task_slot *t = &g_tasks[i];
            asx_region_slot *rslot;
            asx_region_id task_region;
            asx_task_id tid;
            asx_status poll_result;

            if (!t->alive) continue;
            if (asx_task_is_terminal(t->state)) continue;
            rslot = sched_scope_region(t);
            if (rslot == NULL) continue;

            active++;
            tid = asx_task_handle_for_slot(i);

            /* FINALIZING: the task called asx_task_finalize() — cleanup is
             * done. Complete without consuming a poll unit. */
            if (t->state == ASX_TASK_FINALIZING) {
                sched_complete(t, tid, rslot, ASX_OUTCOME_CANCELLED, ASX_SCHED_EVENT_COMPLETE,
                               round);
                active--;
                progress++;
                continue;
            }

            /* Cleanup budget exhausted: force-complete with CANCELLED. The
             * task either never called checkpoint (CANCEL_REQUESTED) or ran
             * out of cleanup polls (CANCELLING). Bounded cleanup. */
            if (t->cancel_pending &&
                (t->state == ASX_TASK_CANCELLING || t->state == ASX_TASK_CANCEL_REQUESTED) &&
                t->cleanup_polls_remaining == 0) {
                if (t->state == ASX_TASK_CANCEL_REQUESTED) {
                    sched_transition(t, tid, ASX_TASK_CANCELLING, ASX_CANCEL_PHASE_CANCELLING);
                }
                sched_transition(t, tid, ASX_TASK_FINALIZING, ASX_CANCEL_PHASE_FINALIZING);
                sched_complete(t, tid, rslot, ASX_OUTCOME_CANCELLED, ASX_SCHED_EVENT_CANCEL_FORCED,
                               round);
                active--;
                progress++;
                continue;
            }

            if (t->parked) continue;

            /* Consume one poll unit */
            if (asx_budget_consume_poll(budget) == 0) {
                sched_emit(ASX_SCHED_EVENT_BUDGET, ASX_INVALID_ID, round);
                asx_trace_emit(ASX_TRACE_SCHED_BUDGET, ASX_INVALID_ID, round);
                return ASX_E_POLL_BUDGET_EXHAUSTED;
            }

            /* Transition Created → Running on first poll */
            if (t->state == ASX_TASK_CREATED) {
                asx_task_state from = t->state;
                (void)asx_ghost_check_task_transition(tid, t->state, ASX_TASK_RUNNING);
                t->state = ASX_TASK_RUNNING;
                asx_trace_emit(ASX_TRACE_TASK_TRANSITION, (uint64_t)tid,
                               asx_trace_task_transition_aux(from, ASX_TASK_RUNNING));
            }

            /* Emit poll event */
            sched_emit(ASX_SCHED_EVENT_POLL, tid, round);
            asx_trace_emit(ASX_TRACE_SCHED_POLL, (uint64_t)tid, round);
            progress++;

            /* Call the task's poll function */
            task_region = t->region;
            t->in_poll = 1;
            t->park_requested = 0;
            t->notified = 0;
            asx_error_ledger_bind_task(tid);
            poll_result = t->poll_fn(t->user_data, tid);
            asx_error_ledger_bind_task(ASX_INVALID_ID);
            t->in_poll = 0;

            if (poll_result == ASX_OK) {
                /* Completed — outcome joins to CANCELLED if cancel was pending */
                sched_complete(t, tid, rslot,
                               t->cancel_pending ? ASX_OUTCOME_CANCELLED : ASX_OUTCOME_OK,
                               ASX_SCHED_EVENT_COMPLETE, round);
                active--;
            } else if (poll_result != ASX_E_PENDING) {
                /* Failed — CANCELLED > ERR in the severity lattice, so a
                 * pending cancel dominates. */
                if (!t->cancel_pending) t->last_error = poll_result;
                sched_complete(t, tid, rslot,
                               t->cancel_pending ? ASX_OUTCOME_CANCELLED : ASX_OUTCOME_ERR,
                               ASX_SCHED_EVENT_COMPLETE, round);
                active--;

                /* Apply fault containment policy (bd-hwb.15). In
                 * POISON_REGION mode this poisons the task's region and the
                 * scheduler continues draining existing tasks. */
                {
                    asx_status fc_ = asx_region_contain_fault(task_region, poll_result);
                    if (fc_ != ASX_OK &&
                        asx_containment_policy_active() != ASX_CONTAIN_POISON_REGION) {
                        return fc_;
                    }
                }
            } else {
                /* PENDING: park if requested and not woken mid-poll. */
                if (t->park_requested && !t->notified) t->parked = 1;
                t->park_requested = 0;
                t->notified = 0;
                /* Each poll of a cancel-phase task consumes one cleanup
                 * unit; the scheduler is the sole budget enforcer. */
                if (t->cancel_pending && t->cleanup_polls_remaining > 0) {
                    t->cleanup_polls_remaining--;
                }
            }
        }

        /* No active tasks left — quiescent */
        if (active == 0) {
            sched_emit(ASX_SCHED_EVENT_QUIESCENT, ASX_INVALID_ID, round);
            asx_trace_emit(ASX_TRACE_SCHED_QUIESCENT, ASX_INVALID_ID, round);
            return ASX_OK;
        }

        if (progress == 0u) {
            st = sched_idle(&last_idle_now, &idle_stall);
            if (st != ASX_OK) return st;
        }
    }
}
