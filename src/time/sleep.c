/*
 * sleep.c — sleep, timeout, and interval poll primitives
 *
 * SPDX-License-Identifier: MIT
 */

#include "../runtime/runtime_internal.h"
#include <asx/asx_config.h>
#include <asx/time/sleep.h>
#include <stdint.h>
#include <string.h>

/* Wait on the calling task's traced sleep timer for `target` (registered
 * once, fired or cancelled in the trace like a Rust Sleep). Best-effort:
 * outside a scheduler poll (or with an invalid `self`) the primitive
 * degrades to plain re-polling. Callers have checked that `target` is
 * still ahead, so this returns ASX_E_PENDING. */
static asx_status park_until(asx_task_id self, asx_time target) {
    asx_status st = asx_task_wait_until(self, target);
    return st == ASX_OK ? ASX_E_PENDING : st;
}

/* ===================================================================
 * Sleep
 * =================================================================== */

asx_status asx_sleep_init(asx_sleep_state *state, uint64_t duration_ns) {
    if (state == NULL) return ASX_E_INVALID_ARGUMENT;

    memset(state, 0, sizeof(*state));
    state->duration_ns = duration_ns;
    state->initialized = 0;
    return ASX_OK;
}

asx_status asx_sleep_init_until(asx_sleep_state *state, asx_time until_ns) {
    if (state == NULL) return ASX_E_INVALID_ARGUMENT;

    memset(state, 0, sizeof(*state));
    state->absolute = 1;
    state->until_ns = until_ns;
    return ASX_OK;
}

/* Rust Sleep's cancellation point (time/sleep.rs:788-794): a cancel is
 * observed when one is requested, its kind is neither Timeout nor
 * Deadline, and a checkpoint reports it (an unmasked task); the checkpoint
 * acknowledges it. The kind is read first, so a Timeout or Deadline
 * cancel is not acknowledged here. */
static int sleep_observes_cancel(asx_task_id self) {
    asx_cancel_reason reason;
    asx_checkpoint_result cr;
    /* The Cx's reason, which a budget cancel may have strengthened ahead of
     * the record (Rust reads cx.cancel_reason()). */
    if (asx_task_get_cx_cancel_reason_internal(self, &reason) != ASX_OK) return 0;
    if (reason.kind == ASX_CANCEL_TIMEOUT || reason.kind == ASX_CANCEL_DEADLINE) return 0;
    if (asx_checkpoint(self, &cr) != ASX_OK) return 0;
    return cr.cancelled;
}

asx_status asx_sleep_poll(void *user_data, asx_task_id self) {
    asx_sleep_state *s = (asx_sleep_state *)user_data;
    asx_time now;
    asx_status st;

    if (s == NULL) return ASX_E_INVALID_ARGUMENT;

    /* First poll: fix the deadline. */
    if (!s->initialized) {
        st = s->absolute ? asx_deadline_init(&s->deadline, s->until_ns)
                         : asx_deadline_after(&s->deadline, s->duration_ns);
        if (st != ASX_OK) return st;
        s->initialized = 1;
    }

    if (sleep_observes_cancel(self)) {
        /* First polled after the cancel: one trip through the scheduler
         * (the task stays runnable), so a loop of fresh sleeps cannot spin
         * inside one poll. A sleep already waiting completes at once. */
        if (!s->polled) {
            s->polled = 1;
            return ASX_E_PENDING;
        }
        st = asx_task_cancel_timer(self);
        (void)st;
        return ASX_OK;
    }
    s->polled = 1;

    /* Due (or a zero-length sleep): done. Otherwise wait on the timer;
     * a later poll (timer fired or spurious wake) re-checks. */
    st = asx_runtime_now_ns(&now);
    if (st != ASX_OK) return st;
    if (asx_deadline_is_expired_at(&s->deadline, now)) {
        /* Completed at its deadline: the sleep records the fire (Rust
         * complete_ready_registration, sleep.rs:677). */
        st = asx_task_complete_timer(self);
        (void)st;
        return ASX_OK;
    }

    return park_until(self, asx_deadline_target(&s->deadline));
}

/* ===================================================================
 * Timeout
 * =================================================================== */

asx_status asx_timeout_init(asx_timeout_state *state, uint64_t timeout_ns,
                            asx_task_poll_fn inner_poll, void *inner_data) {
    if (state == NULL || inner_poll == NULL) return ASX_E_INVALID_ARGUMENT;

    memset(state, 0, sizeof(*state));
    state->timeout_ns = timeout_ns;
    state->inner_poll = inner_poll;
    state->inner_data = inner_data;
    state->initialized = 0;
    state->inner_done = 0;
    return ASX_OK;
}

asx_status asx_timeout_poll(void *user_data, asx_task_id self) {
    asx_timeout_state *s = (asx_timeout_state *)user_data;
    asx_time now;
    asx_status st;
    asx_status inner_st;

    if (s == NULL) return ASX_E_INVALID_ARGUMENT;

    /* First poll: set up the deadline */
    if (!s->initialized) {
        st = asx_deadline_after(&s->deadline, s->timeout_ns);
        if (st != ASX_OK) return st;

        s->initialized = 1;
    }

    /* A repoll after completion fails closed (TimeoutFuture::poll). */
    if (s->completed) return ASX_E_TIMED_OUT;

    st = asx_runtime_now_ns(&now);
    if (st != ASX_OK) return st;

    /* Past the deadline: timed out without polling the inner function. */
    if (now > asx_deadline_target(&s->deadline)) {
        (void)asx_deadline_is_expired_at(&s->deadline, now); /* latches expired */
        s->completed = 1;
        return ASX_E_TIMED_OUT;
    }

    /* Completed work wins at the deadline exactly: poll it first. */
    inner_st = s->inner_poll(s->inner_data, self);
    if (inner_st != ASX_E_PENDING) {
        s->completed = 1;
        if (inner_st == ASX_OK) s->inner_done = 1;
        return inner_st;
    }

    /* Still pending at the deadline: timed out. */
    if (asx_deadline_is_expired_at(&s->deadline, now)) {
        s->completed = 1;
        return ASX_E_TIMED_OUT;
    }

    /* Inner is pending: guarantee a wake at the deadline without parking
     * (whether to park is the inner poll's decision). */
    {
        asx_status a_st_ = asx_task_arm_timer(self, asx_deadline_target(&s->deadline));
        (void)a_st_;
    }
    return ASX_E_PENDING;
}

/* ===================================================================
 * Interval
 * =================================================================== */

asx_status asx_interval_init(asx_interval_state *state, uint64_t period_ns, uint32_t max_ticks) {
    if (state == NULL) return ASX_E_INVALID_ARGUMENT;
    if (period_ns == 0) return ASX_E_INVALID_ARGUMENT;

    memset(state, 0, sizeof(*state));
    state->period_ns = period_ns;
    state->max_ticks = max_ticks;
    state->missed_tick_behavior = ASX_MISSED_TICK_BURST;
    return ASX_OK;
}

asx_status asx_interval_init_at(asx_interval_state *state, asx_time start_ns, uint64_t period_ns,
                                uint32_t max_ticks) {
    asx_status st = asx_interval_init(state, period_ns, max_ticks);
    if (st != ASX_OK) return st;
    state->has_start = 1;
    state->start_ns = start_ns;
    return ASX_OK;
}

asx_status asx_interval_set_missed_tick_behavior(asx_interval_state *state,
                                                 asx_missed_tick_behavior behavior) {
    if (state == NULL) return ASX_E_INVALID_ARGUMENT;
    if ((int)behavior < (int)ASX_MISSED_TICK_BURST || (int)behavior > (int)ASX_MISSED_TICK_SKIP) {
        return ASX_E_INVALID_ARGUMENT;
    }
    state->missed_tick_behavior = behavior;
    return ASX_OK;
}

static asx_time interval_add(asx_time t, uint64_t ns) {
    return ns > UINT64_MAX - t ? UINT64_MAX : t + ns;
}

/* Count the ticks due at `now` from the current deadline on and move the
 * deadline past them (Rust Interval::tick and advance_deadline,
 * time/interval.rs:293-303, :385-420), at most `room` of them. Burst
 * steps a period from the deadline, so the ticks due are counted at once;
 * Delay and Skip leave the next deadline after `now`, so one tick is due.
 * Deadlines saturate: the tick at UINT64_MAX is the last one. */
static uint64_t interval_take_due(asx_interval_state *s, asx_time now, uint64_t room) {
    asx_time d = asx_deadline_target(&s->deadline);
    uint64_t due = 1u;
    asx_time next;
    if (d == UINT64_MAX) {
        s->exhausted = 1;
        return 1u;
    }
    switch (s->missed_tick_behavior) {
    case ASX_MISSED_TICK_DELAY: next = interval_add(now, s->period_ns); break;
    case ASX_MISSED_TICK_SKIP: {
        uint64_t skip = (now - d) / s->period_ns;
        skip = skip == UINT64_MAX ? skip : skip + 1u;
        next = skip > UINT64_MAX / s->period_ns ? UINT64_MAX : interval_add(d, skip * s->period_ns);
        break;
    }
    case ASX_MISSED_TICK_BURST:
    default: {
        uint64_t q = (now - d) / s->period_ns;
        due = q == UINT64_MAX ? q : q + 1u;
        if (due > room) due = room;
        next = due > UINT64_MAX / s->period_ns ? UINT64_MAX : interval_add(d, due * s->period_ns);
        break;
    }
    }
    {
        asx_status d_st_ = asx_deadline_init(&s->deadline, next); /* fails only for NULL */
        (void)d_st_;
    }
    return due;
}

asx_status asx_interval_poll(void *user_data, asx_task_id self) {
    asx_interval_state *s = (asx_interval_state *)user_data;
    asx_time now;
    asx_status st;

    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    if (s->max_ticks > 0u && s->ticks >= s->max_ticks) return ASX_OK; /* completed */

    st = asx_runtime_now_ns(&now);
    if (st != ASX_OK) return st;

    /* First poll: the first tick is at the start (Rust Interval::new). */
    if (!s->initialized) {
        st = asx_deadline_init(&s->deadline, s->has_start ? s->start_ns : now);
        if (st != ASX_OK) return st;
        s->initialized = 1;
    }

    /* Every tick due now. Each pass leaves the deadline after `now` or at
     * UINT64_MAX, whose tick ends the interval: at most three passes. */
    while (!s->exhausted && now >= asx_deadline_target(&s->deadline)) {
        uint64_t room = s->max_ticks > 0u ? (uint64_t)(s->max_ticks - s->ticks) : UINT64_MAX;
        uint64_t taken;
        ASX_CHECKPOINT_WAIVER("bounded: each pass passes now or exhausts the interval");
        taken = interval_take_due(s, now, room);
        s->ticks =
            taken > (uint64_t)(UINT32_MAX - s->ticks) ? UINT32_MAX : s->ticks + (uint32_t)taken;
        if (s->max_ticks > 0u && s->ticks >= s->max_ticks) return ASX_OK;
    }
    if (s->exhausted) return ASX_E_TIMER_DURATION_EXCEEDED;

    return park_until(self, asx_deadline_target(&s->deadline));
}

uint32_t asx_interval_ticks(const asx_interval_state *state) {
    if (state == NULL) return 0;
    return state->ticks;
}

asx_status asx_interval_tick_at(asx_interval_state *state, asx_time now, asx_time *out_time,
                                int *out_due) {
    if (state == NULL || out_time == NULL || out_due == NULL) return ASX_E_INVALID_ARGUMENT;

    if (!state->initialized) {
        asx_status st =
            asx_deadline_init(&state->deadline, state->has_start ? state->start_ns : now);
        if (st != ASX_OK) return st;
        state->initialized = 1;
    }

    /* Rust's tick returns the deadline either way and advances only when
     * it is due; one tick per call, so a Burst interval takes a call per
     * missed tick. */
    *out_time = asx_deadline_target(&state->deadline);
    *out_due = 0;
    if (state->exhausted || now < *out_time) return ASX_OK;

    (void)interval_take_due(state, now, 1u);
    if (state->ticks < UINT32_MAX) state->ticks++;
    *out_due = 1;
    return ASX_OK;
}

/* ===================================================================
 * DelayQueue (Rust time/delay_queue.rs)
 * =================================================================== */

/* Earliest deadline first, then the schedule counter (insertion order);
 * the index is a tie-break nothing reaches, as schedules are unique. */
static int dq_before(const asx_delay_heap_item *a, const asx_delay_heap_item *b) {
    if (a->deadline != b->deadline) return a->deadline < b->deadline;
    if (a->schedule != b->schedule) return a->schedule < b->schedule;
    return a->index < b->index;
}

static void dq_heap_push(asx_delay_queue *q, asx_time deadline, uint64_t schedule, uint32_t index) {
    asx_delay_heap_item item;
    uint32_t i = q->heap_len++;

    item.deadline = deadline;
    item.schedule = schedule;
    item.index = index;
    while (i > 0u) {
        uint32_t parent = (i - 1u) / 2u;
        ASX_CHECKPOINT_WAIVER("bounded: a sift up is at most log2 of the heap length");
        if (!dq_before(&item, &q->heap[parent])) break;
        q->heap[i] = q->heap[parent];
        i = parent;
    }
    q->heap[i] = item;
}

static void dq_heap_pop(asx_delay_queue *q) {
    asx_delay_heap_item last;
    uint32_t i = 0u;

    if (q->heap_len == 0u) return;
    last = q->heap[--q->heap_len];
    for (;;) {
        uint32_t child = 2u * i + 1u;
        ASX_CHECKPOINT_WAIVER("bounded: a sift down is at most log2 of the heap length");
        if (child >= q->heap_len) break;
        if (child + 1u < q->heap_len && dq_before(&q->heap[child + 1u], &q->heap[child])) child++;
        if (!dq_before(&q->heap[child], &last)) break;
        q->heap[i] = q->heap[child];
        i = child;
    }
    if (q->heap_len > 0u) q->heap[i] = last;
}

/* Rust's schedule(): push the heap item, then rebuild from the live
 * entries before stale items (resets, removals) dominate the heap. */
static void dq_schedule(asx_delay_queue *q, asx_time deadline, uint64_t schedule, uint32_t index) {
    dq_heap_push(q, deadline, schedule, index);
    if (q->heap_len > 2u * q->len + 64u) {
        uint32_t i;
        q->heap_len = 0u;
        for (i = 0; i < ASX_DELAY_QUEUE_CAPACITY; i++) {
            ASX_CHECKPOINT_WAIVER("bounded: ASX_DELAY_QUEUE_CAPACITY constant");
            if (q->entries[i].live) {
                dq_heap_push(q, q->entries[i].deadline, q->entries[i].schedule, i);
            }
        }
    }
}

/* The live entry `key` names, or NULL. */
static asx_delay_entry *dq_entry(const asx_delay_queue *q, const asx_delay_key *key) {
    asx_delay_entry *e;
    if (q == NULL || key == NULL || key->index >= ASX_DELAY_QUEUE_CAPACITY) return NULL;
    e = (asx_delay_entry *)&q->entries[key->index];
    return e->live && e->generation == key->generation ? e : NULL;
}

asx_status asx_delay_queue_init(asx_delay_queue *queue) {
    if (queue == NULL) return ASX_E_INVALID_ARGUMENT;
    memset(queue, 0, sizeof(*queue));
    return ASX_OK;
}

asx_status asx_delay_queue_insert_at(asx_delay_queue *queue, uint64_t value, asx_time deadline,
                                     asx_delay_key *out_key) {
    uint32_t slot;
    uint64_t generation;

    if (queue == NULL || out_key == NULL) return ASX_E_INVALID_ARGUMENT;
    for (slot = 0; slot < ASX_DELAY_QUEUE_CAPACITY; slot++) {
        ASX_CHECKPOINT_WAIVER("bounded: ASX_DELAY_QUEUE_CAPACITY constant");
        if (!queue->entries[slot].live) break;
    }
    if (slot == ASX_DELAY_QUEUE_CAPACITY) return ASX_E_RESOURCE_EXHAUSTED;

    generation = queue->next_generation++;
    queue->entries[slot].value = value;
    queue->entries[slot].deadline = deadline;
    queue->entries[slot].generation = generation;
    queue->entries[slot].schedule = generation;
    queue->entries[slot].live = 1;
    queue->len++;
    dq_schedule(queue, deadline, generation, slot);

    out_key->index = slot;
    out_key->generation = generation;
    return ASX_OK;
}

asx_status asx_delay_queue_insert(asx_delay_queue *queue, asx_time now, uint64_t value,
                                  uint64_t timeout_ns, asx_delay_key *out_key) {
    return asx_delay_queue_insert_at(queue, value,
                                     timeout_ns > UINT64_MAX - now ? UINT64_MAX : now + timeout_ns,
                                     out_key);
}

asx_status asx_delay_queue_remove(asx_delay_queue *queue, const asx_delay_key *key,
                                  asx_delay_expired *out) {
    asx_delay_entry *e;

    if (queue == NULL || key == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    e = dq_entry(queue, key);
    if (e == NULL) return ASX_E_NOT_FOUND;

    /* Its heap item is stale now and is skipped when it surfaces. */
    out->value = e->value;
    out->deadline = e->deadline;
    out->key = *key;
    e->live = 0;
    queue->len--;
    return ASX_OK;
}

asx_status asx_delay_queue_reset_at(asx_delay_queue *queue, const asx_delay_key *key,
                                    asx_time deadline) {
    asx_delay_entry *e;
    uint64_t schedule;

    if (queue == NULL || key == NULL) return ASX_E_INVALID_ARGUMENT;
    e = dq_entry(queue, key);
    if (e == NULL) return ASX_E_NOT_FOUND;

    schedule = queue->next_generation++;
    e->deadline = deadline;
    e->schedule = schedule;
    dq_schedule(queue, deadline, schedule, key->index);
    return ASX_OK;
}

asx_status asx_delay_queue_reset(asx_delay_queue *queue, const asx_delay_key *key, asx_time now,
                                 uint64_t timeout_ns) {
    return asx_delay_queue_reset_at(queue, key,
                                    timeout_ns > UINT64_MAX - now ? UINT64_MAX : now + timeout_ns);
}

asx_status asx_delay_queue_deadline(const asx_delay_queue *queue, const asx_delay_key *key,
                                    asx_time *out_deadline) {
    const asx_delay_entry *e;

    if (queue == NULL || key == NULL || out_deadline == NULL) return ASX_E_INVALID_ARGUMENT;
    e = dq_entry(queue, key);
    if (e == NULL) return ASX_E_NOT_FOUND;
    *out_deadline = e->deadline;
    return ASX_OK;
}

int asx_delay_queue_contains(const asx_delay_queue *queue, const asx_delay_key *key) {
    return dq_entry(queue, key) != NULL;
}

uint32_t asx_delay_queue_len(const asx_delay_queue *queue) {
    return queue == NULL ? 0u : queue->len;
}

asx_status asx_delay_queue_clear(asx_delay_queue *queue) {
    uint32_t i;

    if (queue == NULL) return ASX_E_INVALID_ARGUMENT;
    for (i = 0; i < ASX_DELAY_QUEUE_CAPACITY; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: ASX_DELAY_QUEUE_CAPACITY constant");
        queue->entries[i].live = 0;
    }
    queue->len = 0u;
    queue->heap_len = 0u;
    return ASX_OK;
}

asx_status asx_delay_queue_poll_at(asx_delay_queue *queue, asx_time now,
                                   asx_delay_poll_state *out_state, asx_delay_expired *out) {
    if (queue == NULL || out_state == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;

    while (queue->heap_len > 0u) {
        const asx_delay_heap_item top = queue->heap[0];
        asx_delay_entry *e = &queue->entries[top.index];
        ASX_CHECKPOINT_WAIVER("bounded: each pass pops a heap item or returns");
        if (!e->live || e->schedule != top.schedule) {
            dq_heap_pop(queue); /* a reset or removed entry's old item */
            continue;
        }
        if (top.deadline <= now) {
            dq_heap_pop(queue);
            out->value = e->value;
            out->deadline = top.deadline;
            out->key.index = top.index;
            out->key.generation = e->generation;
            e->live = 0;
            queue->len--;
            *out_state = ASX_DELAY_EXPIRED;
            return ASX_OK;
        }
        out->deadline = top.deadline;
        *out_state = ASX_DELAY_WAITING;
        return ASX_OK;
    }
    *out_state = ASX_DELAY_EMPTY;
    return ASX_OK;
}

asx_status asx_delay_queue_poll(asx_delay_queue *queue, asx_task_id self,
                                asx_delay_poll_state *out_state, asx_delay_expired *out) {
    asx_time now;
    asx_status st = asx_runtime_now_ns(&now);

    if (st != ASX_OK) return st;
    st = asx_delay_queue_poll_at(queue, now, out_state, out);
    if (st != ASX_OK || *out_state != ASX_DELAY_WAITING) return st;
    return park_until(self, out->deadline);
}
