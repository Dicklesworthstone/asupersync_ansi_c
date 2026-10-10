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
steps += 1                                                             (LR:4483-4499)
drain deferred cancel dispatches; drain spawn admissions (Cx::spawn children, FIFO)
drain handle cancel requests (<= 16, coalesced); drain region commands (<= 8)
drain handle cancel requests; drain deferred cancel dispatches
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
Ready:   forget_task(t) (purges t's entries only if t is scheduled); complete; wake waiters;
         then t's cancel waker, if acknowledged and due (§5)
Pending: cache (waker, prio); if acknowledged: schedule_cancel(t, cleanup priority);
         then t's cancel waker, if due (§5)
```

A pick of a task that already completed (a retired id a due cancel waker
scheduled, §5) is a dispatch that polls nothing (LR:4772-4790).

Consequences:
- A child spawned during step N's poll is admitted at the start of step
  N+1, before that step's draw.
- A child spawned into a closing or closed region also waits in the spawn
  mailbox. Step N+1's admission refuses it (`state.rs:1702`), but the step
  still runs and draws. C refuses the spawn at once and still takes that
  step (`asx_lab_defer_refused_admission`; fixture
  `spawn-into-cancelled-region-step-001`).
- A region command a task queued during step N (Create, Cancel, Close:
  `Cx::open_child_region`, `ChildRegion::cancel`, `ChildRegion::close`
  and its drop backstop) is applied at the start of step N+1, after the
  admissions. A Create wakes its opener once the batch is applied; a
  closer waits parked until the region's Closed transition wakes it. A
  Cancel or Close is `close_region_command`: the cancel request, then an
  advance of the region's state (a cancelled region with live work goes on
  to Draining), and its cancel effects are deferred, not dispatched
  (`state.rs:4936-4940`). The deferred dispatches are drained after the
  handle cancels the commands queued: every cancel-lane entry first, then
  the wakes, batch by batch (LR:4219-4237). C:
  `asx_region_open_child_poll`, `asx_region_cancel_request`,
  `asx_region_close_poll` / `asx_region_close_request`
  (`lab_dispatch.c`, `asx_lab_drain_region_commands`,
  `asx_lab_drain_deferred_cancels`); fixtures
  `lab-dispatch-region-command-deferred-wakes-001`,
  `lab-dispatch-region-command-advances-region-001`.
- A join-handle abort queued during step N (`JoinHandle::abort_with_reason`)
  is applied at the start of step N+1: after the admissions, and again
  after the region commands, at most 16 per drain, several for one task
  strengthened into one. An abort of a target spawned in step N, not yet
  admitted, is cached in its spawn slot; its admission turns the cached
  reason into one handle-cancel command queued behind those already
  queued, applied by the drain that follows (`spawn_mailbox.rs:629-660`;
  fixture `lab-dispatch-cached-abort-at-admission-001`). A later abort that
  changes the reason or cleanup budget schedules the cancel again
  (`record/task.rs:1019-1049`). C: `asx_task_abort_request`, the cache in
  `lab_dispatch.c`.
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
| Waker wake (timer fire, channel, notify, join) | `schedule(t, waker.priority)`, on the ready lane | Priority of the poll that registered that waker: usually the task's last poll; for a join or a sleep timer, the poll that last polled it (see Join registrations, Timer registrations) |
| `yield_now` | Wake during the poll, then Pending | Current poll's priority |
| Driver `cancel_task` | If the request changed the reason or cleanup budget (`cancel_task` returns `changed && published`, ST:3426-3448; a strengthening counts, not only a new cancel): `schedule_cancel(t, cleanup priority)`. Then the CancelTaskWaker: `schedule_cancel(t, waker priority)` | Cleanup, then last-poll priority |
| Region cancel (`cancel_request`, ST:7811-7876) | For each task whose reason or cleanup budget changed: `schedule_cancel(t, request's cleanup priority)`, all tasks first. Then every task's CancelTaskWaker | As left |
| Checkpoint ack + Pending | `schedule_cancel(t, cleanup priority)` | Cleanup |

A CancelTaskWaker:
- exists only after the task's first poll;
- fires when the Cx is newly cancel-requested or its reason strengthened
  (TR:700-870).

### Acknowledgements and due cancel wakers

- Every unmasked checkpoint that observes the cancel acknowledges it, not
  only the first (`cx.rs:2842-2844`). The lab consumes the acknowledgement
  after the poll (`consume_cancel_ack`, TR:1171-1245). A pending poll that
  acknowledged goes to the cancel lane at the cleanup priority.
- A checkpoint whose budget check changes the reason marks the task's
  cancel waker due (`cx.rs:2824-2840`). The check covers a passed deadline,
  a poll quota of 0 and a cost quota of 0.
- The acknowledgement fires a due waker after the poll:
  - on Pending, after the cleanup `schedule_cancel`;
  - on Ready, after the completion (LR:5011). Rust then schedules the
    retired task id, and the next pick of it polls nothing.
- A later region cancel or handle abort fires a due waker too (TR:840-866).
  The lab's own dispatch-time PollQuota (§2) marks nothing due.
- C: `asx_checkpoint` (`lab_ack_in_poll`, `cancel_wakers_pending`),
  `sched_poll_slot`, `asx_lab_schedule_cancel_retired`. Found by the
  dispatch comparison (bd-9kll.4.8) in fixtures
  `budget-poll-quota-exhaustion-001` and `budget-inherit-before-cleanup-001`.

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

### Join registrations

- A join's poll registers the joiner's waker of that poll on the joined
  task's handle (`poll_join`, `task_handle.rs:925-965`; the retirement
  barrier, `:115-133`). It stays until the same join is polled again (a
  waker of another priority replaces it) or until it fires, which consumes
  it.
- So a joiner that changed priority since it last polled a join is woken by
  that join at the old priority.
- Groups:
  - `join_all` polls only the join it is waiting on.
  - A quorum's collecting poll polls every branch's join (phase 1).
  - A quorum's drain joins one branch at a time, in order (phase 2,
    `cx/scope.rs:1897-1903`). The phase-1 registrations of the branches
    after it stay, at the collecting poll's priority.
- C: `asx_task_slot.watcher_prio`, `group_collect`. Found by the dispatch
  comparison in fuzz scenarios gen-5-75 and gen-8-30 (fixtures
  `task-groups-quorum-drain-wakers-001`, `-002`).

### Timer registrations

- A sleep registers its timer with the waker of the poll that polls it
  (`Sleep::poll_inner`, `time/sleep.rs:880-998`); a later poll of the same
  sleep with a waker of another priority moves the registration.
- A task polled again without polling its sleep keeps the old waker: when
  the timer fires it is woken at the old priority. `Scope::timeout`'s owner
  does this once cancelled: it stops polling its deadline sleep and only
  joins the operation (`cx/scope.rs:2152-2186`), so at the deadline its
  timer wakes it at its pre-cancel priority, behind members re-armed at
  their cleanup priority.
- C: `asx_task_slot.timer_waker_prio`, set when the timer is armed or
  re-armed, used by the lab's timer fire. Found by the dispatch comparison
  in fuzz scenarios gen-17-81 and gen-29-127 (fixtures
  `race-deadline-timer-old-waker-001`,
  `race-deadline-region-cancel-member-order-001`; bd-1maj).

## 7. Run loops and time

- **`run_until_idle`** (LR:3428-3450): drain the handle cancels and the
  deferred cancel dispatches, then step while the scheduled set is not
  empty, or spawns, handle cancels, region commands or deferred cancel
  dispatches wait (`has_pending_dispatch_commands`, LR:2872). It never
  moves time and never fires timers by itself.
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

### Budget-deadline timer

- Rust arms a timer for every work task whose budget has a deadline
  (`Cx::arm_budget_deadline`, `cx.rs:3860-3890`). It is a wheel timer of its
  own, registered when the task is created (`state.rs:4400-4403`) or
  admitted (`state.rs:4892`).
- When it fires (`BudgetDeadlineWake`, `cx.rs:313-367`), it does nothing if
  the task is already cancel-requested or its budget lost that deadline (an
  acknowledged cancel's cleanup budget replaced it). Otherwise it cancels
  the task, Cx only, with Deadline stamped with the deadline, and fires the
  cancel waker at once: the task goes to the cancel lane.
- It leaves the wheel when the task completes (`task_context.rs:1118-1124`).
  Until then it is a pending timer for `run_with_auto_advance`.
- The lab checks no deadline at dispatch. A checkpoint past the deadline
  adds a Deadline candidate stamped now (`cx.rs:3112-3130`).
- C: `asx_lab_arm_budget_deadline_internal`, `lab_deadline_fire`, and the
  wheel-order firing in `timers_fire_wheel_order`. Found by the dispatch
  comparison in fixture `budget-deadline-sleep-checkpoint-001`.

### Region cancel order

A region cancel visits each region's live tasks in its membership's
insertion order: when the task joined the region (creation, or admission for
a child spawned in a poll), not by slot (record/region.rs:341-350,
state.rs:7811-7830). The order is that of the cancel lane entries and the
`cancel.requested` events. C: `asx_task_slot.member_seq`.

## Open points

1. Join wake timing. Is the joiner woken during the child's final poll
   (result channel), or in the completion branch (retirement barrier)? C
   wakes joiners at completion. No fixture has distinguished the two yet.
2. Handle aborts: Rust's target Cx sees the cancel at once
   (`apply_or_defer_cancel_reason`, `task_handle.rs:470-542`); its record
   and cancel lane change at the next step. C applies the whole abort at
   the next step (`asx_task_abort_request`, §2). The Cx-only window is not
   observable with one worker: the drain runs before the next poll.
3. A spawn that is never admitted (its region closed before the next
   step): Rust never creates the task. C refuses it synchronously when the
   region is already closing, and takes the step its admission costs in
   Rust. A region that starts closing in the window between enqueue and
   admission is not modelled.
4. The timer wheel beyond level 0: cascades from level 1 land after level-0
   entries of the same tick. C orders by (tick, registration) only.
5. Dispatch certificates (bd-9kll.4.8).
   - Done: C records every lab dispatch (`asx_scheduler_record_dispatches`).
     twin_run writes Rust's forced-schedule dispatches into each fixture
     (`schedule.dispatches`), and `asx-conformance compare` requires the
     two to match.
   - Still open: Rust's `ScheduleCertificate` hash is not computed in C,
     and no forced schedule is replayed across engines.
