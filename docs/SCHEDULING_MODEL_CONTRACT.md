# Scheduling Model Contract: Rust LabRuntime dispatch (single worker)

The model `src/runtime/lab_dispatch.c` and the lab run loop in
`src/runtime/scheduler.c` implement (`asx_scheduler_use_lab_dispatch`,
bead S1 bd-9kll.4.2). It was extracted from asupersync at the pinned rev
`5e60b1c4c` (bead S0 bd-9kll.4.1). It describes how the lab chooses the next
task to poll, so that C reproduces Rust's dispatch order exactly.

Provenance:
- A research pass extracted it from the source on 2026-10-09, reading only
  through `git show 5e60b1c4c:<path>`.
- It was spot-checked by hand: the xorshift values, wheel insertion, the
  cancel-wake rules and region cancel.
- It was validated end to end by the differential fuzzer. On 491 generated
  scenarios (seeds 1-5), C and Rust agree on trace, snapshot and
  observations, except 10 scenarios that belong to two classified
  upstream-attribution findings (bd-wxep, bd-mex3).
- The points still open are listed at the end.

Abbreviations:

| Abbreviation | Meaning |
|---|---|
| LR | `src/lab/runtime.rs` |
| PR | `src/runtime/scheduler/priority.rs` |
| CFG | `src/lab/config.rs` |
| RNG | `src/util/det_rng.rs` |
| TR | `src/record/task.rs` |
| ST | `src/runtime/state.rs` |
| W | the single worker's `priority::Scheduler` |

## 0. State (worker_count = 1)

```
LabRuntime:   rng: DetRng; steps; dispatches; virtual_time; scheduler
LabScheduler: lab_scheduled: set<TaskId>       (mirror of W.scheduled; is_empty() reads this)
              cancel_streak = 0; cancel_streak_limit = 16          (LR:6051, 6099)
W:            cancel_lane, ready_lane: max-heap<SchedulerEntry>
              timed_lane: max-heap<TimedEntry>
              scheduled: set<TaskId>; next_generation: u64 = 0
              scratch capacity 256 (cancel and ready share it)     (PR:319-346, 425-446)
```

## 1. RNG

- **Seed.** `config.rng()` is `DetRng::new(seed)` (LR:2426, CFG:437-439).
  - There is no mixing.
  - Seed 0 becomes 1.
  - The degenerate seeds {all ones, `0x00000000FFFFFFFF`,
    `0xFFFFFFFF00000000`, `0x5555…`, `0xAAAA…`} get
    `+ 0x9E3779B97F4A7C15` (RNG:129-158).
- **Algorithm.** xorshift64: `x ^= x << 13; x ^= x >> 7; x ^= x << 17`
  (RNG:267-276).
- **Draws.** Exactly one per step that gets past the pending-handle-cancel
  early return (LR:4492-4499). It is drawn whether or not a task is
  runnable. Nothing else draws from it.
- **Seed 42 outputs:** 45454805674, 11532217803599905471,
  10021416941527320954, 2899061411254629736, 5661411637479084162 (unit
  test `test_lab_dispatch`).

## 2. One step (`step_inner`, LR:4460-5045)

```
steps += 1
drain deferred cancel dispatches; drain spawn admissions (Cx::spawn children, FIFO)
drain handle cancel requests (<= 16, coalesced); drain region commands (<= 8)
if handle cancels or deferred cancel dispatches remain: return         (no draw)
r = rng.next_u64()
check_futurelocks(); timer_driver.process_timers(); poll_io(); schedule_async_finalizers()
pick = pop_for_worker(0, r, now)        (§3); none -> return (the draw is spent)
dispatches += 1
cx.budget.consume_poll(); if None: cx cancel_requested + strengthen PollQuota (Cx only)
prio = cx.budget.priority
waker = cached waker if its prio == prio else new TaskWaker{t, prio}   (§6)
poll
consume_cancel_ack -> acknowledge_cancel (budget := cleanup budget)
Ready:   forget_task(t) (purges t's entries only if t is scheduled); complete; wake waiters
Pending: cache (waker, prio); if acknowledged: schedule_cancel(t, cleanup priority)
```

Consequences:
- A child spawned during step N's poll is admitted at the start of step
  N+1, before that step's draw.
- A region command a task queued during step N (Create, Cancel, Close:
  `Cx::open_child_region`, `ChildRegion::cancel`, `ChildRegion::close`
  and its drop backstop) is applied at the start of step N+1, after the
  admissions. A Create wakes its opener once the batch is applied; a
  closer waits parked until the region's Closed transition wakes it. C:
  `asx_region_open_child_poll`, `asx_region_cancel_request`,
  `asx_region_close_poll` / `asx_region_close_request`
  (`lab_dispatch.c`, `asx_lab_drain_region_commands`).
- Timers due at the current time fire inside the step, after the draw and
  before the pick.
- Wakes that happen during a poll get their generation before the
  post-poll `schedule_cancel`.

## 3. Pick order (`pop_for_worker`, LR:6324-6373)

```
if streak < 16 and t = pop(cancel_lane, r): streak += 1; return t
if t = pop_timed_due(r, now):              streak = 0;  return t     (never fed by the lab here)
if t = pop(ready_lane, r):                 streak = 0;  return t
if t = pop(cancel_lane, r):                streak = 1;  return t     (fallback)
streak = 0; return none
```

## 4. Entries and ties

`SchedulerEntry {task, priority: u8, generation: u64}` (PR:28-41). The
highest priority comes first; for equal priority, the lowest generation
comes first.

`generation` is one counter per worker:
- `schedule` consumes one only when it newly inserts the task;
- every `schedule_cancel` and `move_to_cancel_lane` consumes one.

Generations are therefore unique, and each lane's order is total.

```
pop(lane, r): loop {
  group = entries at the lane's highest priority, in generation order, at most 256
  chosen = group[r % len(group)]          (tie_break_index, PR:417-422; full 64-bit modulo)
  remove chosen; the others stay
  if chosen.task in scheduled: scheduled.remove(task); return task
  /* else a dead entry: dropped, retry with the same r */
}
```

### Stale entries

An entry is live only while its task is in `scheduled`:
- A move to the cancel lane pushes a new entry and **leaves the old ones
  in place**.
- A picked task is removed from `scheduled`, so its other entries go
  stale.
- A stale entry still counts in `len(group)` and can be picked; it is
  then dropped.
- When the task is scheduled again, its leftover entries become live
  again. For example, a leftover cancel entry later dispatches the task
  from the cancel lane, ahead of ready work.

The C port keeps these multisets exactly: an entry array per lane, in
generation order.

## 5. Enqueue rules

- `schedule(t, p)`:
  - no-op if t is scheduled;
  - else insert t and push `Ready{t, p, gen++}`.
- `schedule_cancel(t, p)`:
  - insert t if it is not scheduled;
  - always push `Cancel{t, p, gen++}`.

| Event | Call | Priority |
|---|---|---|
| Driver task creation | `schedule(t, 0)` (twin_run run.rs:1648) | 0, whatever the budget |
| `Cx::spawn` child | Admitted at the next step: `schedule(t, budget.priority)` | Inherited budget priority |
| Waker wake (timer fire, channel, notify, join) | `schedule(t, waker.priority)`, on the ready lane | Priority at the task's last poll |
| `yield_now` | Wake during the poll, then Pending | Current poll's priority |
| Driver `cancel_task` | If newly cancelled: `schedule_cancel(t, cleanup priority)`. Then the CancelTaskWaker: `schedule_cancel(t, waker priority)` | Cleanup, then last-poll priority |
| Region cancel (`cancel_request`, ST:7811-7876) | For each task whose reason or cleanup budget changed: `schedule_cancel(t, request's cleanup priority)`, all tasks first. Then every task's CancelTaskWaker | As left |
| Checkpoint ack + Pending | `schedule_cancel(t, cleanup priority)` | Cleanup |

A CancelTaskWaker:
- exists only after the task's first poll;
- fires when the Cx is newly cancel-requested or its reason strengthened
  (TR:700-870).

Default priorities:
- `Budget::INFINITE` = 0; `Budget::new()` = 128.
- Cleanup priorities: User 200; Timeout/Deadline 210; PollQuota/CostBudget
  215; FailFast, RaceLost, ParentCancelled, Resource, LinkedExit 220;
  Shutdown 255.

## 6. Waker identity

At each dispatch the lab builds a **new** waker if the task has none cached
(first poll), or if the cached one's priority differs from the current
budget priority (LR:4686-4708). The cache is written on Pending only.

A pending `Sleep` polled with a changed waker re-registers its timer
(sleep.rs:883-998):
- it records `TimerCancelled(old_id)` then `TimerScheduled(new_id,
  deadline)`;
- the new registration goes behind timers already registered.

The priority changes when:
- acknowledgement replaces the budget with the cleanup budget;
- a request during cleanup replaces it again.

## 7. Run loops and time

- **`run_until_idle`** (LR:3428-3450): step while the scheduled set is not
  empty, or spawns await admission. It never moves time and never fires
  timers by itself.
- **`advance_time_to`**: sets the clock only.
- **`run_with_auto_advance`** (LR:3224-3316):
  - Step while anything is scheduled. More than 1000 steps without a
    dispatch is a bailout.
  - Otherwise, when a timer is pending: move the clock to the earliest
    deadline if it is later, and fire due timers **outside any step** (no
    draw).
  - Otherwise, stop at quiescence, or keep stepping (each step draws)
    until the stuck bound.

### Timer wheel order (time/wheel.rs:710-754, 921-968)

- A timer whose deadline falls in the current 1 ms tick goes to the
  `ready` vector, in registration order.
- Later ones go to per-tick slots, which drain into `ready` in tick order.
- `collect_expired` fires every due entry of `ready` in vector order, and
  keeps the others in order.
- For timers within the first level (256 ms), the firing order is
  therefore (deadline tick, registration order), not deadline order.

## Open points

1. Join wake timing. Is the joiner woken during the child's final poll
   (result channel), or in the completion branch (retirement barrier)? C
   wakes joiners at completion. No fixture has distinguished the two yet.
2. Handle aborts are deferred to the next step in Rust
   (`drain_handle_cancel_requests`): the target's Cx sees the cancel at
   once (`apply_or_defer_cancel_reason`, `task_handle.rs:470-560`), while
   its record and cancel lane change at the next step. C's `abort_task`
   applies the whole cancel at once. Region commands follow Rust (§2).
3. A spawn that is never admitted (its region closed before the next
   step): Rust never creates the task. C refuses it synchronously when the
   region is already closing. A cancel in the window between enqueue and
   admission is not modelled.
4. The timer wheel beyond level 0: cascades from level 1 land after level-0
   entries of the same tick. C orders by (tick, registration) only.
5. Dispatch certificates. Rust's `ScheduleCertificate`
   (`(task, lane, step)` records) is not yet computed in C, so
   certificate equality (bd-9kll.4.8) is still open.
