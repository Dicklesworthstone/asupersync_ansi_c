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

#ifndef ASX_SCHED_EVENT_LOG_CAPACITY
#define ASX_SCHED_EVENT_LOG_CAPACITY 256u
#endif
#if (ASX_SCHED_EVENT_LOG_CAPACITY) < 1
#error "ASX_SCHED_EVENT_LOG_CAPACITY must be at least 1"
#endif

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
/* Lab dispatch: budget-deadline timers armed (task deadline_timer_*). */
static uint32_t g_lab_deadline_armed = 0;

/* Task whose poll function is running (ASX_INVALID_ID outside polls). */
static asx_task_id g_current_task = ASX_INVALID_ID;

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
    uint32_t removed;
    if (pos >= g_timer_heap_len || pos >= ASX_MAX_TASKS) return; /* not in the heap */
    removed = g_timer_heap[pos];
    g_timer_heap_len--;
    g_tasks[removed].timer_pos = ASX_SLOT_NONE;
    if (pos >= g_timer_heap_len) return; /* removed the last entry */
    timer_heap_place(pos, g_timer_heap[g_timer_heap_len]);
    if (pos > 0u && timer_less(g_timer_heap[pos], g_timer_heap[(pos - 1u) / 2u])) {
        timer_heap_sift_up(pos);
    } else {
        timer_heap_sift_down(pos);
    }
}

static void lab_deadline_disarm(asx_task_slot *t) {
    if (!t->deadline_timer_armed) return;
    t->deadline_timer_armed = 0u;
    if (g_lab_deadline_armed > 0u) g_lab_deadline_armed--;
}

void asx_task_timer_disarm_internal(asx_task_slot *task) {
    if (task == NULL) return;
    /* A finished task's budget-deadline timer leaves the wheel with it
     * (take_cancel_wakers, types/task_context.rs:1118-1124). */
    lab_deadline_disarm(task);
    /* A sleep timer that never fired is dropped with its task (Rust: the
     * Sleep future is dropped, TimerCancelled). */
    if (task->traced_deadline != 0u) {
        asx_trace_emit(ASX_TRACE_TIMER_CANCEL,
                       (uint64_t)asx_task_handle_for_slot((uint32_t)(task - g_tasks)),
                       task->traced_deadline);
        task->traced_deadline = 0u;
    }
    if (task->timer_pos == ASX_SLOT_NONE) return;
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
    /* Each task holds at most one heap entry, so the heap never exceeds
     * ASX_MAX_TASKS; the guard keeps that invariant locally checkable. */
    if (g_timer_heap_len >= ASX_MAX_TASKS) return;
    t->wake_at = deadline;
    t->timer_seq = g_timer_seq++;
    timer_heap_place(g_timer_heap_len, idx);
    g_timer_heap_len++;
    timer_heap_sift_up(g_timer_heap_len - 1u);
}

/* Lab dispatch: Rust arms a work task's budget-deadline timer when it is
 * created or admitted (Cx::arm_budget_deadline, cx/cx.rs:3860-3890;
 * runtime/state.rs:4400-4403, 4892), a wheel timer behind those registered
 * before it. */
void asx_lab_arm_budget_deadline_internal(asx_task_slot *t) {
    if (t->budget.deadline == 0u) return;
    if (t->deadline_timer_armed) {
        /* The creation's budget, met after the task was armed: still the
         * one registration (asx_task_spawn_with_budget). */
        t->deadline_timer_at = t->budget.deadline;
        return;
    }
    t->deadline_timer_armed = 1u;
    t->deadline_timer_at = t->budget.deadline;
    t->deadline_timer_seq = g_timer_seq++;
    g_lab_deadline_armed++;
}

/* The budget-deadline timer fires (BudgetDeadlineWake, cx/cx.rs:313-367):
 * unless the task is already cancel-requested or its budget lost this
 * deadline (an acknowledged cancel's cleanup budget replaced it), a
 * DEADLINE cancel stamped with the deadline, its cancel waker at once. */
static void lab_deadline_fire(uint32_t slot) {
    asx_task_slot *t = &g_tasks[slot];
    asx_time at = t->deadline_timer_at;
    asx_status st;
    lab_deadline_disarm(t);
    if (!t->alive || asx_task_is_terminal(t->state) || t->cancel_pending) return;
    if (t->budget.deadline == 0u || t->budget.deadline > at) return;
    st = asx_task_cancel_budget_internal(asx_task_handle_for_slot(slot), ASX_CANCEL_DEADLINE, at);
    (void)st;
    t->cancel_wakers_pending = 0u;
    asx_lab_cancel_wake(t);
}

/* Whether a timer is pending, and the earliest deadline (lab
 * budget-deadline timers included). */
int asx_scheduler_next_timer_internal(asx_time *out_next) {
    int any = 0;
    asx_time next = 0;
    uint32_t i;
    if (g_timer_heap_len > 0u) {
        next = g_tasks[g_timer_heap[0]].wake_at;
        any = 1;
    }
    for (i = 0; g_lab_deadline_armed > 0u && i < g_task_count; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: g_task_count <= ASX_MAX_TASKS");
        if (!g_tasks[i].deadline_timer_armed) continue;
        if (!any || g_tasks[i].deadline_timer_at < next) next = g_tasks[i].deadline_timer_at;
        any = 1;
    }
    if (any) *out_next = next;
    return any;
}

/* Lab dispatch: fire the timers due at `now` in the order Rust's timer
 * wheel wakes them, which decides their wake order and so their dispatch
 * generations (bd-9kll.4.2). The wheel keeps a timer due within its
 * current 1 ms tick in its ready vector in registration order, files later
 * ones in per-tick slots drained in tick order, and fires every due entry
 * of the ready vector, keeping the others in order (time/wheel.rs:710-754,
 * 921-968): (deadline tick, registration) for timers within the first
 * level's 256 ms. A task's wake timer and its budget-deadline timer are
 * separate entries; due[] marks the latter with LAB_DUE_DEADLINE. */
#define LAB_WHEEL_TICK_NS ((asx_time)1000000u)
#define LAB_DUE_DEADLINE 0x80000000u

static void lab_due_key(uint32_t entry, asx_time *out_tick, uint64_t *out_seq) {
    const asx_task_slot *t = &g_tasks[entry & ~LAB_DUE_DEADLINE];
    if (entry & LAB_DUE_DEADLINE) {
        *out_tick = t->deadline_timer_at / LAB_WHEEL_TICK_NS;
        *out_seq = t->deadline_timer_seq;
    } else {
        *out_tick = t->wake_at / LAB_WHEEL_TICK_NS;
        *out_seq = t->timer_seq;
    }
}

static uint32_t timers_fire_wheel_order(asx_time now) {
    uint32_t due[2u * (uint32_t)ASX_MAX_TASKS];
    uint32_t n = 0;
    uint32_t i;
    for (i = 0; i < g_timer_heap_len + g_task_count; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: heap length and g_task_count <= ASX_MAX_TASKS");
        uint32_t entry;
        uint32_t j = n;
        asx_time qt;
        uint64_t qs;
        if (i < g_timer_heap_len) {
            entry = g_timer_heap[i];
            if (g_tasks[entry].wake_at > now) continue;
        } else {
            entry = i - g_timer_heap_len;
            if (!g_tasks[entry].deadline_timer_armed || g_tasks[entry].deadline_timer_at > now) {
                continue;
            }
            entry |= LAB_DUE_DEADLINE;
        }
        lab_due_key(entry, &qt, &qs);
        /* Insertion by (tick, registration). */
        while (j > 0u) {
            ASX_CHECKPOINT_WAIVER("bounded: insertion into <= 2 * ASX_MAX_TASKS entries");
            asx_time pt;
            uint64_t ps;
            lab_due_key(due[j - 1u], &pt, &ps);
            if (pt < qt || (pt == qt && ps < qs)) break;
            due[j] = due[j - 1u];
            j--;
        }
        due[j] = entry;
        n++;
    }
    for (i = 0; i < n; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: due timers <= 2 * ASX_MAX_TASKS");
        asx_task_slot *t = &g_tasks[due[i] & ~LAB_DUE_DEADLINE];
        if (due[i] & LAB_DUE_DEADLINE) {
            lab_deadline_fire(due[i] & ~LAB_DUE_DEADLINE);
            continue;
        }
        if (t->timer_pos != ASX_SLOT_NONE) timer_heap_remove_at(t->timer_pos);
        asx_task_wake_slot_internal(t);
    }
    return n;
}

/* Fire every task timer due at `now`, in (deadline, seq) order: wake its
 * task. The trace records nothing here: as in Rust, the sleep records
 * fired or cancelled itself when it completes (asx_task_complete_timer,
 * asx_task_cancel_timer; sleep.rs:670-690), so a sleeper woken by its
 * timer that first observes a cancel records a cancel (fuzz finding
 * gen-1-21, bd-ij9w). Returns the number of tasks woken. */
static uint32_t timers_fire(asx_time now) {
    uint32_t fired = 0;
    if (asx_lab_dispatch_active()) return timers_fire_wheel_order(now);
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
    g_current_task = ASX_INVALID_ID;
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
    task->deadline_timer_armed = 0;
    task->deadline_timer_at = 0;
    task->deadline_timer_seq = 0;
    task->first_waiter = ASX_SLOT_NONE;
    task->next_waiter = ASX_SLOT_NONE;
    task->waiting_on = ASX_SLOT_NONE;
    task->watcher = ASX_SLOT_NONE;
    task->watcher_gen = 0;
    task->watcher_prio = 0;
    task->mask_depth = 0;
    task->traced_deadline = 0;
    task->panicked = 0;
    task->panic_message = NULL;
    task->cancel_polled = 0;
    task->spawned_in_poll = 0;
    task->first_polled = 0;
    task->cancel_before_first_poll = 0;
    task->lab_scheduled = 0;
    task->lab_waker_prio = 0;
    task->lab_admission_pending = 0;
    task->lab_admission_refusal = ASX_OK;
    task->lab_ack_in_poll = 0;
    task->cancel_wakers_pending = 0;
    task->lab_waker_epoch = 0;
    task->traced_waker_epoch = 0;
    task->region_wait = ASX_REGION_WAIT_NONE;
    task->region_wait_status = ASX_OK;
    task->region_wait_region = ASX_INVALID_ID;
}

/* Whether a pending cancel makes a completing task's outcome CANCELLED
 * (Rust classify_spawn_completion, task_handle.rs:173-202). A task spawned
 * from inside another task's poll (Rust cx.spawn,
 * PreserveAcknowledgedCancellationResult) keeps the value it returned when
 * its cancel came after its first poll and it acknowledged it (a checkpoint
 * moved it to CANCELLING); C reasons are always attributed. Every other
 * task (Rust create_task) is cancellation-dominant (DSL v2 §2). */
static int sched_cancel_dominates(const asx_task_slot *t) {
    int acknowledged = t->state == ASX_TASK_CANCELLING || t->state == ASX_TASK_FINALIZING;
    if (!t->cancel_pending) return 0;
    return !(t->spawned_in_poll && !t->cancel_before_first_poll && acknowledged);
}

asx_status asx_task_panic(asx_task_id self, const char *message) {
    asx_task_slot *t;
    asx_status st = asx_task_slot_lookup(self, &t);
    if (st != ASX_OK) return st;
    if (!t->in_poll) return ASX_E_INVALID_STATE;
    t->panicked = 1;
    /* Rust reports a non-string payload as "unknown panic" (cx/scope.rs:181-187). */
    t->panic_message = message != NULL ? message : "unknown panic";
    return ASX_OK;
}

asx_status asx_task_get_panic_message(asx_task_id id, const char **out) {
    asx_task_slot *t;
    asx_status st;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    st = asx_task_slot_lookup(id, &t);
    if (st != ASX_OK) return st;
    if (!t->panicked) return ASX_E_NOT_FOUND;
    *out = t->panic_message;
    return ASX_OK;
}

void asx_task_wake_slot_internal(asx_task_slot *task) {
    if (task == NULL || !task->alive) return;
    if (asx_task_is_terminal(task->state)) return;
    /* Lab dispatch: a wake goes through the task's waker, which schedules
     * it on the ready lane at the priority it was built with (TaskWaker,
     * LR:6531-6535). */
    if (asx_lab_dispatch_active()) {
        asx_lab_schedule(task, task->lab_waker_prio);
        return;
    }
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
    if (task->watcher != ASX_SLOT_NONE) {
        asx_task_slot *ws = &g_tasks[task->watcher];
        if (ws->generation == task->watcher_gen) {
            if (asx_lab_dispatch_active()) {
                /* The waker the watch was registered with. */
                if (ws->alive && !asx_task_is_terminal(ws->state)) {
                    asx_lab_schedule(ws, task->watcher_prio);
                }
            } else {
                asx_task_wake_slot_internal(ws);
            }
        }
        task->watcher = ASX_SLOT_NONE;
    }
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

void asx_region_wake_close_waiters_internal(asx_region_id id) {
    uint32_t i;
    for (i = 0; i < g_task_count; i++) {
        asx_task_slot *t = &g_tasks[i];
        ASX_CHECKPOINT_WAIVER("bounded: g_task_count <= ASX_MAX_TASKS");
        if (t->alive && t->region_wait == ASX_REGION_WAIT_CLOSE && t->region_wait_region == id) {
            asx_task_wake_slot_internal(t);
        }
    }
}

/* Clock read for scheduling decisions: the runtime clock when hooks are
 * installed, the virtual clock otherwise. */
static asx_time sched_now(void) {
    asx_time now;
    if (asx_runtime_now_ns(&now) != ASX_OK) now = asx_runtime_virtual_now();
    return now;
}

uint32_t asx_scheduler_fire_due_timers_internal(void) { return timers_fire(sched_now()); }

/* -------------------------------------------------------------------
 * Public wake/park/timer/join API
 * ------------------------------------------------------------------- */

asx_task_id asx_task_current(void) { return g_current_task; }

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
        /* A traced sleep timer: registered once per deadline (re-polls of
         * the same sleep add nothing); a new deadline drops the old timer
         * first, as re-arming a Rust Sleep does (sleep.rs:985-988). */
        if (t->traced_deadline != deadline) {
            if (t->traced_deadline != 0u) {
                asx_trace_emit(ASX_TRACE_TIMER_CANCEL, (uint64_t)self, t->traced_deadline);
            }
            t->traced_deadline = deadline;
            t->traced_waker_epoch = t->lab_waker_epoch;
            asx_trace_emit(ASX_TRACE_TIMER_SET, (uint64_t)self, deadline);
        } else if (asx_lab_dispatch_active() && t->traced_waker_epoch != t->lab_waker_epoch) {
            /* Polled with a new waker (its priority changed): the Sleep
             * moves its registration to the new waker, which cancels the
             * old one and registers it afresh, behind the timers already
             * registered (timer.update, time/sleep.rs:952-998). The same
             * timer: the new registration is marked "rearm". */
            asx_trace_payload rearm;
            rearm.text = "rearm";
            rearm.reason = NULL;
            asx_trace_emit(ASX_TRACE_TIMER_CANCEL, (uint64_t)self, deadline);
            asx_trace_emit_payload_internal(ASX_TRACE_TIMER_SET, (uint64_t)self, deadline, &rearm);
            t->traced_waker_epoch = t->lab_waker_epoch;
            if (t->timer_pos != ASX_SLOT_NONE) timer_heap_remove_at(t->timer_pos);
        }
        timer_arm((uint32_t)(t - g_tasks), deadline);
        t->park_requested = 1;
    }
    return ASX_E_PENDING;
}

asx_status asx_task_cancel_timer(asx_task_id self) {
    asx_task_slot *t;
    asx_status st = asx_task_slot_lookup(self, &t);
    if (st != ASX_OK) return st;
    /* A sleep that ends without completing at its deadline (Rust:
     * cancel_active_registration, TimerCancelled), whether or not its timer
     * already woke it. */
    if (t->traced_deadline != 0u) {
        asx_trace_emit(ASX_TRACE_TIMER_CANCEL, (uint64_t)self, t->traced_deadline);
        t->traced_deadline = 0u;
    }
    if (t->timer_pos != ASX_SLOT_NONE) timer_heap_remove_at(t->timer_pos);
    return ASX_OK;
}

asx_status asx_task_complete_timer(asx_task_id self) {
    asx_task_slot *t;
    asx_status st = asx_task_slot_lookup(self, &t);
    if (st != ASX_OK) return st;
    /* A sleep that completes at its deadline (Rust:
     * complete_ready_registration, TimerFired). A sleep that never
     * registered (ready at its first poll) records nothing. */
    if (t->traced_deadline != 0u) {
        asx_trace_emit(ASX_TRACE_TIMER_FIRE, (uint64_t)self, t->traced_deadline);
        t->traced_deadline = 0u;
    }
    if (t->timer_pos != ASX_SLOT_NONE) timer_heap_remove_at(t->timer_pos);
    return ASX_OK;
}

asx_status asx_task_watch(asx_task_id target, asx_task_id watcher) {
    asx_task_slot *tt;
    asx_task_slot *tw;
    asx_status st;

    st = asx_task_slot_lookup(target, &tt);
    if (st != ASX_OK) return st;
    st = asx_task_slot_lookup(watcher, &tw);
    if (st != ASX_OK) return st;
    if (tt == tw) return ASX_E_INVALID_ARGUMENT;
    if (asx_task_is_terminal(tt->state)) {
        asx_task_wake_slot_internal(tw);
        return ASX_OK;
    }
    tt->watcher = (uint32_t)(tw - g_tasks);
    tt->watcher_gen = tw->generation;
    tt->watcher_prio = tw->lab_waker_prio;
    return ASX_OK;
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
    for (i = 0; i < n; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: n <= ASX_MAX_REGIONS");
        g_scope_mark[g_scope_slots[i]] = g_scope_epoch;
    }
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
 * released (detached task) when this returns: do not touch `t` after.
 * Returns a fault raised by completion bookkeeping (obligation leak under
 * the PANIC policy with fail-fast containment), else ASX_OK. */
static asx_status sched_complete(asx_task_slot *t, asx_task_id tid, asx_region_slot *rslot,
                                 asx_outcome_severity severity, asx_scheduler_event_kind ev,
                                 uint32_t round) {
    asx_task_state from = t->state;
    asx_region_id region = t->region;
    (void)asx_ghost_check_task_transition(tid, t->state, ASX_TASK_COMPLETED);
    t->state = ASX_TASK_COMPLETED;
    if (asx_lab_dispatch_active()) asx_lab_task_retired_internal(t);
    if (t->cancel_pending) sched_cancel_phase_complete(t);
    asx_trace_emit(ASX_TRACE_TASK_TRANSITION, (uint64_t)tid,
                   asx_trace_task_transition_aux(from, ASX_TASK_COMPLETED));
    t->outcome = asx_outcome_make(severity);
    asx_task_on_complete_internal(t, rslot);
    sched_emit(ev, tid, round);
    asx_trace_emit(ASX_TRACE_SCHED_COMPLETE, (uint64_t)tid, round);
    asx_region_settle_internal(region);
    return asx_runtime_take_pending_fault_internal();
}

/* Enforce a task's budget before polling it: deadline -> DEADLINE cancel,
 * exhausted poll quota -> POLL_QUOTA cancel, which also strengthens a
 * cancel already pending (Rust's lab, lab/runtime.rs:4663-4670; fuzz
 * finding gen-1-73, bd-ij9w). `*now`/`*have_now` cache one clock read per
 * round (only taken when one is needed). */
static void sched_enforce_budget(asx_task_slot *t, asx_task_id tid, asx_time *now, int *have_now) {
    asx_status st;
    /* Under lab dispatch the budget-deadline timer cancels the task, as
     * Rust's lab does (lab_deadline_fire); it checks no deadline here. */
    if (!asx_lab_dispatch_active() && !t->cancel_pending && t->budget.deadline != 0u) {
        if (!*have_now) {
            *now = sched_now();
            *have_now = 1;
        }
        if (*now >= t->budget.deadline) {
            /* Stamped with the deadline itself, as Rust's deadline timer
             * stamps it (cx.rs:357). */
            st = asx_task_cancel_budget_internal(tid, ASX_CANCEL_DEADLINE, t->budget.deadline);
            (void)st;
            return;
        }
    }
    if (t->budget.poll_quota == 0u) {
        if (asx_lab_dispatch_active()) {
            /* Rust's lab stamps this one with CancelReason::poll_quota()'s
             * testing defaults (lab/runtime.rs:4667); a later checkpoint
             * re-attributes it, earlier timestamp winning (bd-wxep). */
            asx_cancel_reason r = asx_cancel_reason_default(ASX_CANCEL_POLL_QUOTA, NULL);
            st = asx_task_cancel_reason_internal(tid, &r, ASX_CANCEL_SRC_BUDGET);
            (void)st;
            return;
        }
        if (!*have_now) {
            *now = sched_now();
            *have_now = 1;
        }
        st = asx_task_cancel_budget_internal(tid, ASX_CANCEL_POLL_QUOTA, *now);
        (void)st;
    }
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
            ASX_CHECKPOINT_WAIVER("bounded: n <= 16");
            if (asx_task_wake(woken[i]) == ASX_OK) total++;
        }
    } while (n == 16u);
    return total;
}

/* Unpark every cancel-pending task in scope. Returns the count. */
/* `stranded` = 0: the cancel request woke the task; once it has been
 * polled since, it saw the cancel and chose to keep waiting (a
 * Deadline-kind sleep, an uninterruptible join, a Notify wait), and Rust
 * lets it wait (sleep.rs:789-812, task_handle.rs:1080-1126,
 * sync/notify.rs:347). Only a task not yet polled since its cancel is
 * unparked so it can observe it.
 *
 * `stranded` = 1 (the opt-in hard cleanup bound only, run-to-completion,
 * when nothing in the runtime can wake anything): a cancelled task with no
 * timer or join to wait on is unparked anyway so the hard bound drives it
 * to completion. By default, as in Rust, such a task stays parked and the
 * run reports ASX_E_WOULD_BLOCK (Rust reports a futurelock,
 * lab/runtime.rs:5612; fuzz finding gen-1-93, bd-9kll.3.2). */
static uint32_t sched_unpark_cancelled(int stranded) {
    uint32_t i;
    uint32_t n = 0;
    for (i = 0; i < g_task_count; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: g_task_count <= ASX_MAX_TASKS");
        asx_task_slot *t = &g_tasks[i];
        if (!t->alive || !t->parked || !t->cancel_pending) continue;
        if (sched_scope_region(t) == NULL) continue;
        if (stranded) {
            if (t->timer_pos != ASX_SLOT_NONE || t->waiting_on != ASX_SLOT_NONE) continue;
        } else if (t->cancel_polled) {
            continue;
        }
        t->parked = 0;
        n++;
    }
    return n;
}

static int sched_io_pending(void) {
#if ASX_HAS_NATIVE_IO_DRIVER
    return asx_io_driver_is_initialized() && asx_io_armed_count() > 0u;
#else
    return 0;
#endif
}

/* Live backend: collect readiness without blocking so I/O-bound tasks
 * make progress even while other tasks keep the scheduler busy. */
static void sched_poll_io_nonblocking(void) {
#if ASX_HAS_NATIVE_IO_DRIVER
    if (asx_io_driver_is_live() && asx_io_armed_count() > 0u) {
        asx_io_event events[16];
        (void)asx_io_driver_poll(events, 16u, 0u);
    }
#endif
}

/* Block for up to timeout_ms waiting for I/O, a cross-thread wake
 * (blocking-pool completion), or just time to pass. Returns at once when
 * a waker is already signaled. */
static void sched_block(uint32_t timeout_ms) {
    if (!asx_waker_prepare_block_internal()) return;
#if ASX_HAS_NATIVE_IO_DRIVER
    if (sched_io_pending()) {
        asx_io_event events[8];
        (void)asx_io_driver_poll(events, 8u, timeout_ms);
        asx_waker_finish_block_internal();
        return;
    }
#endif
    {
        uint32_t ready = 0;
        (void)asx_runtime_reactor_wait(timeout_ms, &ready, 0u);
    }
    asx_waker_finish_block_internal();
}

/* A live, unparked task in scope: a round has work. Rust's lab steps, and
 * so processes due timers, only while some task is runnable
 * (run_until_idle, lab/runtime.rs:3428-3447; the timer pass is inside the
 * step, :4509), so an idle run_until_idle leaves due timers unfired for the
 * caller's next advance (fuzz finding gen-1-38, bd-ij9w). */
static int sched_any_runnable(void) {
    uint32_t i;
    for (i = 0; i < g_task_count; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: g_task_count <= ASX_MAX_TASKS");
        const asx_task_slot *t = &g_tasks[i];
        if (!t->alive || t->parked || asx_task_is_terminal(t->state)) continue;
        if (sched_scope_region(t) != NULL) return 1;
    }
    return 0;
}

/* Work that completes off the scheduler thread and signals a waker. */
static int sched_external_pending(void) {
#if ASX_HAS_BLOCKING_SURFACE
    if (asx_blocking_active_count() > 0u) return 1;
#endif
    return sched_io_pending();
}

/* Milliseconds to block before `deadline` (0 = none), capped at
 * `cap_ms`; rounds up so a wake never lands just short of the deadline. */
static uint32_t sched_ms_until(asx_time deadline, asx_time now, uint32_t cap_ms) {
    uint64_t wait_ms;
    if (deadline == 0u) return cap_ms;
    if (now >= deadline) return 0u;
    wait_ms = ((deadline - now) + 999999u) / 1000000u;
    return wait_ms > cap_ms ? cap_ms : (uint32_t)wait_ms;
}

/* Called when every live task in scope is parked. Returns ASX_OK if the
 * scheduler should run another round, ASX_E_WOULD_BLOCK if nothing can
 * wake the parked tasks. Blocking never extends past `run_deadline` (the
 * caller's run budget deadline, 0 = none); the round loop then reports
 * ASX_E_TIMED_OUT. */
static asx_status sched_idle(asx_time *last_idle_now, uint32_t *stall, asx_time run_deadline,
                             int advance_clock) {
    asx_time now;

    if (sched_unpark_cancelled(0) > 0u) return ASX_OK;
    if (sched_drain_wakers() > 0u) return ASX_OK;

    /* Run-until-idle: nothing is runnable, so the run ends, due timers
     * unfired: the lab processes timers only within a step, and steps only
     * while some task is runnable; the clock is the caller's to move. */
    if (!advance_clock) {
        /* ASX_ANALYZER_WAIVER("config-dependent: 0 without blocking pool/native I/O") */
        return (g_timer_heap_len > 0u || sched_external_pending()) ? ASX_E_PENDING
                                                                   : ASX_E_WOULD_BLOCK;
    }

    if (g_timer_heap_len > 0u) {
        asx_time next = g_tasks[g_timer_heap[0]].wake_at;
        asx_time target = next;
        if (run_deadline != 0u && run_deadline < target) target = run_deadline;
        now = sched_now();
        if (now < target && asx_runtime_clock_is_virtual()) {
            asx_runtime_virtual_advance(target);
            now = sched_now();
        }
        if (timers_fire(now) > 0u) return ASX_OK;
        if (now < target) {
            sched_block(sched_ms_until(target, now, ASX_SCHED_MAX_IDLE_WAIT_MS));
            (void)sched_drain_wakers();
            now = sched_now();
            (void)timers_fire(now);
        }
        /* ASX_ANALYZER_WAIVER("config-dependent: 0 without blocking pool/native I/O") */
    } else if (sched_external_pending()) {
        now = sched_now();
        if (run_deadline != 0u && now >= run_deadline) return ASX_OK;
        sched_block(sched_ms_until(run_deadline, now, ASX_SCHED_MAX_IDLE_WAIT_MS));
        (void)sched_drain_wakers();
        now = sched_now();
    } else if (asx_cleanup_hard_bound_internal() && sched_unpark_cancelled(1) > 0u) {
        return ASX_OK;
    } else {
        return ASX_E_WOULD_BLOCK;
    }

    /* The run deadline passed while idle: let the round loop report it. */
    if (run_deadline != 0u && now >= run_deadline) return ASX_OK;

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

/* Poll task slot `i` once (live, in a run's scope, not parked, its budget
 * already enforced) and act on the result: Created -> Running, the poll
 * event, the poll charged to its quota, the poll itself, then completion,
 * containment or parking. Returns ASX_OK or a fault the run must surface;
 * *out_done is set when the task completed. Under lab dispatch it also
 * does what Rust's lab does around the poll (LR:4657-5040): the waker of
 * this poll takes the task's budget priority; a task that yields schedules
 * itself; one that acknowledged its cancel and keeps running goes to the
 * cancel lane at its cleanup priority; a completed task is forgotten. */
/* A cancel waker a budget cancel left due (asx_checkpoint) fires with the
 * poll's acknowledgement: the task's CancelTaskWaker schedules it on the
 * cancel lane at its last poll's priority. After the task completed, Rust
 * still schedules its id, and the next pick of it polls nothing
 * (record/task.rs:1227-1233, lab/runtime.rs:5011, 4772-4790). */
static void sched_fire_due_cancel_waker(asx_task_slot *t) {
    if (!t->cancel_wakers_pending) return;
    t->cancel_wakers_pending = 0u;
    asx_lab_schedule_cancel(t, t->lab_waker_prio);
}

static asx_status sched_poll_slot(uint32_t i, asx_region_slot *rslot, uint32_t round,
                                  int *out_done) {
    asx_task_slot *t = &g_tasks[i];
    asx_task_id tid = asx_task_handle_for_slot(i);
    asx_region_id task_region = t->region;
    int lab = asx_lab_dispatch_active();
    uint16_t task_gen = t->generation;
    int retired_wake = 0;
    asx_status poll_result;
    asx_status st;

    *out_done = 0;

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

    /* The waker of this poll: a new one on the first poll or when the
     * priority changed since the last poll (LR:4686-4692). */
    if (lab && (!t->first_polled || t->budget.priority != t->lab_waker_prio)) {
        t->lab_waker_epoch++;
    }
    /* Call the task's poll function */
    if (t->cancel_pending) t->cancel_polled = 1;
    if (!t->first_polled) {
        t->first_polled = 1;
        t->cancel_before_first_poll = t->cancel_pending ? 1u : 0u;
        /* Rust forks a spawned task's entropy before its code first runs
         * (overlay_parent_inheritance, cx.rs:5876-5879). */
        if (lab) asx_lab_entropy_first_poll_internal(t);
    }
    /* Charge the poll before running it, as Rust's lab does
     * (lab/runtime.rs:4663): a checkpoint in the poll that spends
     * the last unit sees the quota exhausted. */
    if (t->budget.poll_quota != UINT32_MAX && t->budget.poll_quota > 0u) { t->budget.poll_quota--; }
    if (lab) t->lab_waker_prio = t->budget.priority;
    t->in_poll = 1;
    t->park_requested = 0;
    t->notified = 0;
    t->lab_ack_in_poll = 0;
    g_current_task = tid;
    asx_error_ledger_bind_task(tid);
    poll_result = t->poll_fn(t->user_data, tid);
    asx_error_ledger_bind_task(ASX_INVALID_ID);
    g_current_task = ASX_INVALID_ID;
    t->in_poll = 0;
    /* Acknowledged during this poll and still running: the cleanup
     * budget becomes the task's budget now (Rust applies the
     * acknowledgement after the poll, lab/runtime.rs:4809). A task
     * that completed keeps the budget it ended with. */
    if (poll_result == ASX_E_PENDING && !t->panicked) {
        /* A pending poll that did not park is a yield: the task woke
         * itself during the poll (yield_now, src/runtime/yield_now.rs). */
        if (lab && !t->park_requested) asx_lab_schedule(t, t->lab_waker_prio);
        asx_task_apply_cleanup_budget_internal(t);
        /* The poll acknowledged its cancel: the cancel lane at the cleanup
         * priority, then the cancel waker if a budget cancel left it due
         * (lab/runtime.rs:5027-5036). */
        if (lab && t->lab_ack_in_poll) {
            asx_lab_schedule_cancel(t, t->cleanup_budget.priority);
            sched_fire_due_cancel_waker(t);
        }
    } else if (lab) {
        asx_lab_forget(t);
        /* A due cancel waker fires after the completion (taken now: the
         * completion may release the slot). */
        retired_wake = t->lab_ack_in_poll && t->cancel_wakers_pending;
        t->cancel_wakers_pending = 0u;
    }

    /* A panic ends the task whatever the poll returned (Rust catches
     * it at the poll boundary: Outcome::Panicked). It dominates a
     * pending cancel in the outcome lattice and is not a containment
     * fault: the 0.6.0 runtime applies no policy action to it. */
    if (t->panicked) {
        uint8_t prio = t->lab_waker_prio;
        *out_done = 1;
        st = sched_complete(t, tid, rslot, ASX_OUTCOME_PANICKED, ASX_SCHED_EVENT_COMPLETE, round);
        if (retired_wake) asx_lab_schedule_cancel_retired(i, task_gen, prio);
        return st;
    }

    if (poll_result == ASX_OK) {
        /* Completed — CANCELLED if a pending cancel dominates. */
        uint8_t prio = t->lab_waker_prio;
        *out_done = 1;
        st = sched_complete(t, tid, rslot,
                            sched_cancel_dominates(t) ? ASX_OUTCOME_CANCELLED : ASX_OUTCOME_OK,
                            ASX_SCHED_EVENT_COMPLETE, round);
        if (retired_wake) asx_lab_schedule_cancel_retired(i, task_gen, prio);
        return st;
    }
    if (poll_result != ASX_E_PENDING) {
        /* Failed — CANCELLED > ERR in the severity lattice, so a
         * dominating pending cancel wins. */
        int was_cancelled = sched_cancel_dominates(t);
        uint8_t prio = t->lab_waker_prio;
        if (!was_cancelled) t->last_error = poll_result;
        *out_done = 1;
        st = sched_complete(t, tid, rslot, was_cancelled ? ASX_OUTCOME_CANCELLED : ASX_OUTCOME_ERR,
                            ASX_SCHED_EVENT_COMPLETE, round);
        if (retired_wake) asx_lab_schedule_cancel_retired(i, task_gen, prio);
        if (st != ASX_OK) return st;

        /* Apply fault containment policy (bd-hwb.15). In
         * POISON_REGION mode this poisons the task's region and the
         * scheduler continues draining existing tasks. A cancelled
         * task's outcome is CANCELLED, not a fault: its error (often
         * ASX_E_CANCELLED itself) is how it acknowledged the cancel. */
        if (!was_cancelled) {
            asx_status fc_ = asx_region_contain_fault(task_region, poll_result);
            if (fc_ != ASX_OK && asx_containment_policy_active() != ASX_CONTAIN_POISON_REGION) {
                return fc_;
            }
        }
        return ASX_OK;
    }

    /* PENDING: park if requested and not woken mid-poll. */
    if (t->park_requested && !t->notified) t->parked = 1;
    t->park_requested = 0;
    t->notified = 0;
    /* A parked task with a deadline must wake to be cancelled (under lab
     * dispatch its budget-deadline timer does it). */
    if (!lab && t->parked && !t->cancel_pending && t->budget.deadline != 0u) {
        timer_arm(i, t->budget.deadline);
    }
    /* Opt-in hard bound's counter: each poll of a cancel-phase
     * task consumes one cleanup unit. Masked polls do not
     * count: the cancel is not yet acknowledged. */
    if (asx_cleanup_hard_bound_internal() && t->cancel_pending && t->mask_depth == 0u &&
        t->cleanup_polls_remaining > 0) {
        t->cleanup_polls_remaining--;
    }
    return ASX_OK;
}

/* -------------------------------------------------------------------
 * Lab dispatch run (bd-9kll.4.2; lab_dispatch.c has the model)
 * ------------------------------------------------------------------- */

static uint32_t sched_live_tasks(void) {
    uint32_t i;
    uint32_t n = 0;
    for (i = 0; i < g_task_count; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: g_task_count <= ASX_MAX_TASKS");
        if (g_tasks[i].alive && !asx_task_is_terminal(g_tasks[i].state)) n++;
    }
    return n;
}

/* One step of Rust's lab with one worker (LR:4460-5045): admit the tasks
 * spawned from polls, draw the step's value, fire the timers that are due,
 * then pick a task and poll it. *out_dispatched tells whether one was
 * picked. */
static asx_status sched_lab_step(uint32_t step, int *out_dispatched) {
    uint64_t r;
    uint32_t slot;
    uint16_t task_gen = 0;
    int cancel_lane;
    asx_task_slot *t;
    asx_region_slot *rslot;
    asx_task_id tid;
    asx_time now = 0;
    int have_now = 0;
    int done = 0;
    asx_status st;
    uint64_t lab_step = asx_lab_step_begin_internal();

    *out_dispatched = 0;
    /* step_inner (LR:4481-4500): spawn admissions, handle aborts, region
     * commands, handle aborts again, then the step's draw. */
    asx_lab_admit_pending();
    asx_lab_drain_handle_cancels();
    asx_lab_drain_region_commands();
    asx_lab_drain_handle_cancels();
    r = asx_lab_rng_next();
    if (g_timer_heap_len > 0u || g_lab_deadline_armed > 0u) (void)timers_fire(sched_now());
    (void)sched_drain_wakers();
    if (asx_lab_dispatch_overflowed()) return ASX_E_RESOURCE_EXHAUSTED;
    if (!asx_lab_pick(r, &slot, &task_gen, &cancel_lane)) return ASX_OK;
    *out_dispatched = 1;
    /* Recorded at the pick, before the poll (LR:4627-4644). */
    asx_lab_record_dispatch_internal(slot, task_gen, cancel_lane, lab_step, sched_now());
    t = &g_tasks[slot];
    /* A retired task a cancel waker scheduled: a dispatch that polls
     * nothing (LR:4772-4790). */
    if (!t->alive || t->generation != task_gen || asx_task_is_terminal(t->state)) return ASX_OK;
    tid = asx_task_handle_for_slot(slot);
    st = asx_region_slot_lookup(t->region, &rslot);
    if (st != ASX_OK) return st;
    if (t->state == ASX_TASK_FINALIZING) {
        return sched_complete(t, tid, rslot, ASX_OUTCOME_CANCELLED, ASX_SCHED_EVENT_COMPLETE, step);
    }
    asx_task_apply_cleanup_budget_internal(t);
    sched_enforce_budget(t, tid, &now, &have_now);
    st = sched_poll_slot(slot, rslot, step, &done);
    if (st == ASX_OK && asx_lab_dispatch_overflowed()) st = ASX_E_RESOURCE_EXHAUSTED;
    return st;
}

/* Rust's run_until_idle (LR:3428-3450) when !advance_clock: step while a
 * task is scheduled, awaits admission or a region command or handle abort
 * is queued (has_pending_dispatch_commands, LR:2872). run_with_auto_advance
 * (LR:3224-3316) otherwise: when nothing is scheduled, move the clock to
 * the next timer and fire it outside any step, and stop at quiescence or
 * after 1000 steps without a dispatch. The budget counts steps. */
static asx_status sched_lab_run(asx_budget *budget, int advance_clock) {
    uint32_t step = 0;
    uint32_t stuck = 0;
    asx_status st;

    for (;;) {
        int dispatched = 0;
        int has_timer;
        asx_time next = 0;
        ASX_CHECKPOINT_WAIVER("kernel-scheduler: the lab step loop; the step budget and the "
                              "1000-step stuck bound end it");
        if (asx_lab_scheduled_count() > 0u || asx_lab_admissions_pending() ||
            asx_lab_region_commands_pending() || asx_lab_handle_cancels_pending()) {
            if (asx_budget_consume_poll(budget) == 0) {
                sched_emit(ASX_SCHED_EVENT_BUDGET, ASX_INVALID_ID, step);
                asx_trace_emit(ASX_TRACE_SCHED_BUDGET, ASX_INVALID_ID, step);
                return ASX_E_POLL_BUDGET_EXHAUSTED;
            }
            st = sched_lab_step(step++, &dispatched);
            if (st != ASX_OK) return st;
            if (dispatched) {
                stuck = 0;
            } else if (advance_clock && ++stuck > 1000u) {
                return ASX_E_WOULD_BLOCK;
            }
            continue;
        }
        if (sched_live_tasks() == 0u) {
            sched_emit(ASX_SCHED_EVENT_QUIESCENT, ASX_INVALID_ID, step);
            asx_trace_emit(ASX_TRACE_SCHED_QUIESCENT, ASX_INVALID_ID, step);
            return ASX_OK;
        }
        has_timer = asx_scheduler_next_timer_internal(&next);
        if (!advance_clock) {
            /* ASX_ANALYZER_WAIVER("config-dependent: 0 without blocking pool/native I/O") */
            return (has_timer || sched_external_pending()) ? ASX_E_PENDING : ASX_E_WOULD_BLOCK;
        }
        if (has_timer) {
            if (next > sched_now() && asx_runtime_clock_is_virtual()) {
                asx_runtime_virtual_advance(next);
            } else if (++stuck > 1000u) {
                return ASX_E_WOULD_BLOCK;
            }
            (void)timers_fire(sched_now());
            continue;
        }
        /* Live tasks, nothing scheduled, no timer: Rust keeps stepping
         * (each step draws a value) until its stuck bound. */
        if (++stuck > 1000u) return ASX_E_WOULD_BLOCK;
        if (asx_budget_consume_poll(budget) == 0) return ASX_E_POLL_BUDGET_EXHAUSTED;
        st = sched_lab_step(step++, &dispatched);
        if (st != ASX_OK) return st;
    }
}

static asx_status sched_run(asx_region_id region, asx_budget *budget, int advance_clock);

asx_status asx_scheduler_run(asx_region_id region, asx_budget *budget) {
    return sched_run(region, budget, 1);
}

asx_status asx_scheduler_run_until_idle(asx_region_id region, asx_budget *budget) {
    return sched_run(region, budget, 0);
}

static asx_status sched_run(asx_region_id region, asx_budget *budget, int advance_clock) {
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
    (void)asx_runtime_take_pending_fault_internal(); /* drop stale faults */

    if (asx_lab_dispatch_active()) return sched_lab_run(budget, advance_clock);

    for (round = 0;; round++) {
        uint32_t active = 0;
        uint32_t progress = 0;
        uint32_t i;
        asx_time round_now = 0;
        int have_now = 0;

        ASX_CHECKPOINT_WAIVER("kernel-scheduler: this IS the scheduler event loop; "
                              "budget exhaustion provides bounded termination");
        asx_trace_emit(ASX_TRACE_SCHED_ROUND, ASX_INVALID_ID, round);

        /* Check budget exhaustion */
        if (asx_budget_is_exhausted(budget)) {
            sched_emit(ASX_SCHED_EVENT_BUDGET, ASX_INVALID_ID, round);
            asx_trace_emit(ASX_TRACE_SCHED_BUDGET, ASX_INVALID_ID, round);
            return ASX_E_POLL_BUDGET_EXHAUSTED;
        }

        /* The run budget's deadline bounds the whole run, including time
         * spent blocked in the reactor waiting for I/O. */
        if (asx_budget_is_past_deadline(budget, sched_now())) {
            sched_emit(ASX_SCHED_EVENT_BUDGET, ASX_INVALID_ID, round);
            asx_trace_emit(ASX_TRACE_SCHED_BUDGET, ASX_INVALID_ID, round);
            return ASX_E_TIMED_OUT;
        }

        sched_compute_scope(region);
        sched_poll_io_nonblocking();
        (void)sched_drain_wakers();
        /* Due timers fire at the start of a round, as a lab step processes
         * them before polling; run-until-idle starts no round for them
         * when nothing is runnable (sched_any_runnable). */
        if (g_timer_heap_len > 0u && (advance_clock || sched_any_runnable())) {
            (void)timers_fire(sched_now());
        }

        for (i = 0; i < g_task_count; i++) {
            ASX_CHECKPOINT_WAIVER("kernel-scheduler: inner poll loop bounded by "
                                  "g_task_count <= ASX_MAX_TASKS arena capacity");
            asx_task_slot *t = &g_tasks[i];
            asx_region_slot *rslot;
            asx_task_id tid;

            if (!t->alive) continue;
            if (asx_task_is_terminal(t->state)) continue;
            rslot = sched_scope_region(t);
            if (rslot == NULL) continue;

            active++;
            tid = asx_task_handle_for_slot(i);

            /* FINALIZING: the task called asx_task_finalize() — cleanup is
             * done. Complete without consuming a poll unit. */
            if (t->state == ASX_TASK_FINALIZING) {
                st = sched_complete(t, tid, rslot, ASX_OUTCOME_CANCELLED, ASX_SCHED_EVENT_COMPLETE,
                                    round);
                if (st != ASX_OK) return st;
                active--;
                progress++;
                continue;
            }

            /* Opt-in hard bound only (asx_runtime_config.cleanup_hard_bound):
             * cleanup polls exhausted, force-complete with CANCELLED. The
             * task either never called checkpoint (CANCEL_REQUESTED) or ran
             * out of cleanup polls (CANCELLING). A masked task is inside a
             * critical section and is never interrupted. Rust never
             * force-completes: its cleanup budget only strengthens the
             * reason once spent (record/task.rs:1340, lab/runtime.rs:4663;
             * three_lane.rs:1268-1279 keeps it advisory in production). */
            if (asx_cleanup_hard_bound_internal() && t->cancel_pending && t->mask_depth == 0u &&
                (t->state == ASX_TASK_CANCELLING || t->state == ASX_TASK_CANCEL_REQUESTED ||
                 t->cancel_unmaterialized) &&
                t->cleanup_polls_remaining == 0) {
                asx_task_materialize_cancel_internal(t);
                if (t->state == ASX_TASK_CANCEL_REQUESTED) {
                    sched_transition(t, tid, ASX_TASK_CANCELLING, ASX_CANCEL_PHASE_CANCELLING);
                }
                sched_transition(t, tid, ASX_TASK_FINALIZING, ASX_CANCEL_PHASE_FINALIZING);
                st = sched_complete(t, tid, rslot, ASX_OUTCOME_CANCELLED,
                                    ASX_SCHED_EVENT_CANCEL_FORCED, round);
                if (st != ASX_OK) return st;
                active--;
                progress++;
                continue;
            }

            if (t->parked) continue;

            /* A cancel acknowledged outside any poll (a checkpoint called
             * by the host) takes effect before the task's next poll. */
            asx_task_apply_cleanup_budget_internal(t);

            /* Budget: deadline / poll quota -> cancellation before polling */
            sched_enforce_budget(t, tid, &round_now, &have_now);

            /* Consume one poll unit */
            if (asx_budget_consume_poll(budget) == 0) {
                sched_emit(ASX_SCHED_EVENT_BUDGET, ASX_INVALID_ID, round);
                asx_trace_emit(ASX_TRACE_SCHED_BUDGET, ASX_INVALID_ID, round);
                return ASX_E_POLL_BUDGET_EXHAUSTED;
            }

            progress++;
            {
                int done = 0;
                st = sched_poll_slot(i, rslot, round, &done);
                if (st != ASX_OK) return st;
                if (done) active--;
            }
        }

        /* No active tasks left — quiescent */
        if (active == 0) {
            sched_emit(ASX_SCHED_EVENT_QUIESCENT, ASX_INVALID_ID, round);
            asx_trace_emit(ASX_TRACE_SCHED_QUIESCENT, ASX_INVALID_ID, round);
            return ASX_OK;
        }

        if (progress == 0u) {
            st = sched_idle(&last_idle_now, &idle_stall, budget->deadline, advance_clock);
            if (st != ASX_OK) return st;
        }
    }
}
