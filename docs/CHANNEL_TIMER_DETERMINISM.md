# Channel and Timer Determinism Contract (Canonical Extraction)

> **Bead:** bd-296.17 (original extraction); truth pass bd-9kll.2.12
> **Scope:** Deterministic MPSC reserve/send/abort behavior, timer ordering, cancellation handles, and tie-break contract
> **Status:** Reference for what the Rust code does at the baseline below, with a **C status** line in each section. Where asupersync's normative v4 semantics (`asupersync_v4_formal_semantics.md` at the same commit) states a rule, the section says so and points at `docs/C_REFINEMENT_MAP.md` for the C status of that rule. `docs/CHANNEL_TIMER_SEMANTICS.md` is the fuller channel/timer reference.
> **Rust baseline commit:** `5e60b1c4c` (asupersync). Every Rust `path:line` below was re-read at this commit with `git -C /dp/asupersync show 5e60b1c4c:<path>`. The original extraction used `38c152405`; none of its line numbers are kept. No Rust toolchain was involved: the facts are read from source.
> **C revision:** C `file:line` citations were read in the working tree at `b412780`. Line numbers drift as the code changes, so treat them as grep anchors and look for the symbol named beside each one.
> **Last verified:** 2026-10-10 (bd-9kll.2.12)

This file is the Phase 1 extraction artifact for `bd-296.17`. It captures channel/timer determinism and tie-break behavior required by downstream beads (`bd-296.18`, `bd-296.19`, `bd-1md.13`, `bd-1md.14`, `bd-1md.15`) and by the C runtime kernel phases.

## 1. Source Provenance

Primary Rust sources at `5e60b1c4c` (paths relative to the asupersync repository):

- `src/channel/mpsc.rs` (2279 lines). Its unit tests live in `src/channel/mpsc_tests.rs`, pulled in with `include!` at `mpsc.rs:2279`.
- `src/time/wheel.rs` (3509 lines, tests in the same file)
- `src/runtime/timer.rs` (903 lines)
- `src/runtime/scheduler/priority.rs` (lane entry ordering, section 4)
- `src/lab/runtime.rs` (`LabRuntime` dispatch, which the C lab dispatch ports, section 4)
- `asupersync_v4_formal_semantics.md` (normative rules: §1.11 lanes, §3.0 scheduling, §3.4 obligations, §3.6 time)

Primary C sources: `src/channel/mpsc.c`, `src/sync/wait_queue.h`, `src/sync/wait_queue.c`, `src/time/timer_wheel.c`, `src/runtime/scheduler.c`, `src/runtime/lab_dispatch.c`.

Cross-check references:

- `PLAN_TO_PORT_ASUPERSYNC_TO_ANSI_C.md` at the repository root (section 6.8.3 H, "Preserving Determinism (Not Just Best Effort)")
- `docs/LIFECYCLE_TRANSITION_TABLES.md` (lifecycle and quiescence coupling)
- `docs/EXISTING_ASUPERSYNC_STRUCTURE.md` (budget/exhaustion coupling from `bd-296.16`)
- `docs/CHANNEL_TIMER_SEMANTICS.md` (channel/timer reference, re-verified at `5e60b1c4c`)
- `docs/C_REFINEMENT_MAP.md` (C status per v4 rule)

## 2. Deterministic Channel Semantics (MPSC Two-Phase)

### 2.1 Core State and Capacity Invariant

`mpsc.rs` models capacity with (`ChannelInner`, `mpsc.rs:275-282`; `ChannelShared`, `mpsc.rs:297-311`):

- `queue: VecDeque<T>` (committed messages),
- `reserved: usize` (outstanding permits),
- `send_wakers: TokenSlab<Arc<RegisteredWaker>>` plus `waiter_queue: VecDeque<SlabToken>` (producer wait queue: the slab holds the wakers, the deque holds the FIFO order of their tokens),
- `capacity: usize` (fixed at construction, write-once, `mpsc.rs:308-310`).

Canonical invariant:

`used_slots = queue.len() + reserved` and `used_slots <= capacity`

Evidence:

- `used_slots` and `has_capacity` definitions: `mpsc.rs:349-359`
- `reserved` is incremented by a successful reserve (`try_reserve`, `mpsc.rs:744`; `Reserve::poll_inner`, `mpsc.rs:1087`) and decremented by commit (`try_send_deferred_wake`, `mpsc.rs:1599`) and by abort or an unsent drop (`release_capacity`, `mpsc.rs:1647`)
- zero capacity panics at construction (`assert!(capacity > 0)`, `mpsc.rs:565`)
- regression tests (`mpsc_tests.rs`): `capacity_invariant_across_reserve_send_abort` (`:2363`), `send_evict_oldest_returns_full_when_all_capacity_reserved` (`:2145`), `send_evict_oldest_evicts_committed_not_reserved` (`:2180`)

**C status:** implemented. A reserve is refused when `queue_len + reserved >= capacity` (`channel_reserve_impl`, `src/channel/mpsc.c:571-573`; lock-free backend `src/channel/mpsc.c:569`). Capacity 0 or above `ASX_CHANNEL_MAX_CAPACITY` (default 64, `include/asx/core/channel.h:57-59`) returns `ASX_E_INVALID_ARGUMENT` instead of panicking (`src/channel/mpsc.c:383`). C has no unbounded channel (Rust's `unbounded_channel` is `channel(usize::MAX)`). Unit tests: `tests/unit/channel/test_mpsc.c` `capacity_enforcement`, `capacity_mixed_reserved_and_queued`, `capacity_invariant_interleaves_send_abort`, `create_zero_capacity`.

### 2.2 Two-Phase Obligation Protocol

Superseded in part by asupersync_v4_formal_semantics.md §3.4 (rules RESERVE, COMMIT, ABORT, LEAK) and §5 INV-OBLIGATION-LINEAR (rule `inv.obligation.linear`); C status: see C_REFINEMENT_MAP.md rows `rule.obligation.reserve`, `rule.obligation.commit`, `rule.obligation.abort`, `rule.obligation.leak` and `inv.obligation.linear`. Those rules govern the permit's runtime obligation. The channel's capacity bookkeeping below is not in the v4 semantics.

Channel send path is explicitly two-phase:

1. `reserve` acquires one capacity slot and returns a `SendPermit` (`Sender::reserve`, `mpsc.rs:637`; `Reserve::poll_inner`, `mpsc.rs:1040-1172`). A permit from `reserve`/`reserve_checked` carries a runtime `SendPermit` obligation registered through the task's `Cx` after the slot is claimed (`mpsc.rs:1116-1119`); a permit from `try_reserve` carries none (field doc `mpsc.rs:1464-1472`).
2. The permit resolves exactly once through:
   - `permit.send(value)` or `permit.try_send(value)` (commit; `mpsc.rs:1565-1578`, both through `try_send_deferred_wake`, `mpsc.rs:1588-1629`). If the receiver is gone the value comes back in `SendError::Disconnected(value)` and the obligation is aborted as an error, not committed (`mpsc.rs:1602-1614`),
   - `permit.abort()` (`mpsc.rs:1633-1639`: releases the slot, wakes the head waiter, aborts the obligation with reason `Explicit`),
   - drop of an unsent permit (`Drop for SendPermit`, `mpsc.rs:1663-1673`: releases the slot the same way but aborts the obligation with reason `Cancel`, not `Explicit`).

`SendPermit` is `#[must_use]` (`mpsc.rs:1460`). Note that `Sender::send(cx, value)` reserves and commits in one poll and registers no runtime obligation (doc comment `mpsc.rs:709-713`, `Sender::send` at `mpsc.rs:715`).

Evidence:

- `SendPermit` and its obligation field: `mpsc.rs:1458-1472`
- contract tests (`mpsc_tests.rs`): `two_phase_send_recv` (`:582`), `permit_abort_releases_slot` (`:605`), `permit_drop_releases_slot` (`:633`), `dropped_permit_releases_capacity` (`:1792`)

**C status:** reserve, commit and abort are implemented; drop is not. `asx_channel_reserve` (`src/channel/mpsc.c:827-847`) registers a `SendPermit` obligation when the `Cx` names a task (`src/channel/mpsc.c:839-845`). `asx_send_permit_send` (`src/channel/mpsc.c:638-693`) consumes the permit token, returns `ASX_E_DISCONNECTED` and aborts the obligation with `ASX_OBLIGATION_ABORT_ERROR` when the receiver is closed (`src/channel/mpsc.c:662-668`), else enqueues and commits (`src/channel/mpsc.c:684-690`). `asx_send_permit_abort` (`src/channel/mpsc.c:699-722`) returns the slot and aborts with `ASX_OBLIGATION_ABORT_EXPLICIT`. C has no destructors: a permit that is neither sent nor aborted keeps its slot reserved, and its registered obligation, which is linked to the holder task, is resolved by the leak policy when that task completes (`asx_task_resolve_held_obligations_internal`, `src/runtime/lifecycle.c:515`), so it ends Leaked (default policy) where Rust's drop ends it Aborted(Cancel). The conformance interpreter emulates Rust's drop (`drop_send_permit`, `src/conformance/interpreter.c:801-809`). A double send or send-after-abort is rejected (`ASX_E_INVALID_STATE`, `src/channel/mpsc.c:646`). Rust parity: `mpsc-two-phase-send-recv-001` (its trace records `obligation.reserved` and `obligation.committed` of kind `SendPermit` for both sends). No v2 scenario aborts or drops a channel permit. Unit tests: `test_mpsc.c` `abort_returns_capacity`, `double_send_same_permit`, `reserve_with_cx_registers_send_permit_committed_by_send`.

### 2.3 FIFO Waiter Discipline and Queue-Jump Prevention

Deterministic sender fairness rule:

- Waiting `reserve` futures are queued FIFO: a waiter's token is appended to `waiter_queue` once (`mpsc.rs:1146-1170`, push at `:1157`) and keeps its position across re-polls (the registered waker is replaced in place, `mpsc.rs:1147-1152`). There is no separate monotonic waiter id: the slab token is the identity.
- A reserve poll can claim capacity only if it is the queue head (or the queue is empty and it is not queued) and capacity exists (`is_first`, `mpsc.rs:1081-1087`). When the head claims a slot and capacity remains, it wakes the next head (`mpsc.rs:1099-1101`).
- `try_reserve` returns `Full` if any live waiter is queued, even when raw capacity is available (`has_waiting_sender`, `mpsc.rs:374-377`, checked at `mpsc.rs:739-741` inside `try_reserve`, `mpsc.rs:732-754`). `try_send` (`mpsc.rs:773-794`) and `send_evict_oldest_where` (`mpsc.rs:889-936`) apply the same rule.

Evidence:

- tests (`mpsc_tests.rs`): `try_reserve_respects_fifo_over_capacity` (`:2404`), `try_reserve_full_when_waiter_queued` (`:1947`), `try_send_respects_queued_waiter_fifo` (`:672`)

**C status:** implemented. Parked producers wait in `reserve_waiters`, a wait queue that is FIFO by arrival and keeps a re-polling waiter's position (`src/sync/wait_queue.h:23-24`). A reserve reports full while a live producer is queued ahead of the caller (`asx_wait_queue_live_ahead`, `src/channel/mpsc.c:561-566`; contract `src/sync/wait_queue.h:42-45`). After each state change `channel_settle` hands free capacity to the head of the line only (`src/channel/mpsc.c:357-364`). Unit tests: `tests/unit/channel/test_channel_wake.c` `try_reserve_never_jumps_parked_producer`, `reserve_waiters_served_in_arrival_order_deterministically`, `full_channel_parks_producers_and_dequeue_wakes_one`. No v2 fixture has more than one producer on a channel, so FIFO order among several parked producers is not fixture-checked; `mpsc-two-phase-send-recv-001` (capacity 1) checks one producer waiting for the consumer.

### 2.4 Cancellation and Disconnect Behavior

Reserve path (`Reserve::poll_inner`, checked on every poll in this order):

- A cancellation checkpoint failure returns `SendError::Cancelled(())` and removes the waiter (`mpsc.rs:1061-1068`); nothing is reserved.
- Receiver dropped returns `SendError::Disconnected(())` (`mpsc.rs:1072-1079`).
- A cancelled or dropped pending `Reserve` runs `cleanup_waiter` (`mpsc.rs:960-998`, from `Drop for Reserve`, `mpsc.rs:1225-1229`): its token leaves the slab and the FIFO, and if it held a queue position and capacity is free the next head is woken. Stale tokens at the front of the FIFO are pruned (`prune_stale_waiter_front`, `mpsc.rs:361-370`), so a departed waiter does not block later ones.

Receiver path (`poll_recv`, `mpsc.rs:1778-1859`):

- Cancellation is checked before the pop: a cancelled `recv` returns `RecvError::Cancelled` without consuming a queued message (`mpsc.rs:1786-1797`).
- An empty queue with `sender_count == 0` (or the receiver closed) yields `RecvError::Disconnected` (`mpsc.rs:1813-1821`); queued messages are drained first (`mpsc.rs:1801-1810`).

Evidence (`mpsc_tests.rs`): `reserve_cancelled_returns_error` (`:1739`), `recv_cancelled_returns_error` (`:1754`), `recv_cancelled_does_not_consume_message` (`:1769`), `reserve_pending_then_cancelled_cleans_waiter_queue` (`:2466`), `stale_missing_waiter_drop_does_not_wake_next_sender` (`:2833`), `receiver_drop_unblocks_pending_reserve_without_leak` (`:2516`), `recv_after_sender_dropped_drains_queue` (`:1091`).

**C status:** implemented. `channel_wait_cancelled` (`src/channel/mpsc.c:818-825`) checks `asx_cx_checkpoint` before any channel state is read, records the trace message `mpsc::reserve cancelled` or `mpsc::recv cancelled`, and withdraws the task from both wait queues, passing on any wake it held (`asx_channel_wait_cancel`, `src/channel/mpsc.c:795-808`). Completed or reclaimed waiters never count and are unlinked lazily (`src/sync/wait_queue.h:25-28`), and a cancel-pending waiter never absorbs a wake (`src/sync/wait_queue.h:37-38`). Receiver closed gives `ASX_E_DISCONNECTED` on reserve and send (`src/channel/mpsc.c:556-559`, `:662-668`); a receive on an empty channel whose sender side is closed gives `ASX_E_DISCONNECTED` once no permit is outstanding (`src/channel/mpsc.c:766-776`). Unit tests: `test_mpsc.c` `cancelled_cx_wins_over_ready_channel_and_traces` (a cancelled recv leaves the queued value for a later receive), `test_channel_wake.c` `cancelled_waiter_does_not_absorb_wake`, `close_wakes_every_waiter`. Rust parity: `mpsc-recv-cancel-first-001` (a pending recv on an empty channel is cancelled by the driver and returns `ASX_E_CANCELLED`), `lab-dispatch-mpsc-disconnect-order-001` (a send after the receiver's task completed reports `ASX_E_DISCONNECTED`). Reserve cancellation has no v2 fixture.

### 2.5 Backpressure and Drop-Oldest Policy

`send_evict_oldest` (`mpsc.rs:872-874`) is `send_evict_oldest_where(value, |_| true)` (`mpsc.rs:889-936`):

- Receiver dropped: `Err(Disconnected(value))`.
- Free capacity and no queued waiter: plain enqueue, `Ok(None)`.
- Free capacity but a live waiter queued (the free slot belongs to the waiter): `Err(Full(value))`, nothing evicted (`mpsc.rs:903-907`). The old extraction said free capacity always enqueues; that is no longer true.
- No free capacity: remove the oldest queued (committed) message the predicate accepts, never a reserved slot, and enqueue, `Ok(Some(evicted))` (`mpsc.rs:910-918`).
- No free capacity and nothing evictable (all capacity reserved, or every queued message protected by the predicate): `Err(Full(value))` (`mpsc.rs:919-923`).

Evidence (`mpsc_tests.rs`): `send_evict_oldest_returns_full_when_all_capacity_reserved` (`:2145`), `send_evict_oldest_evicts_committed_not_reserved` (`:2180`), `send_evict_oldest_does_not_drop_messages_when_waiter_owns_free_slot` (`:2284`), `send_evict_oldest_no_eviction_with_capacity` (`:2260`).

**C status:** spec (Rust), not implemented in C: no eviction function exists in `src/channel/`. A full C channel reports `ASX_E_CHANNEL_FULL` to `asx_channel_try_reserve` and parks a waiting `asx_channel_reserve`/`asx_channel_send` (which returns `ASX_E_PENDING`) (`src/channel/mpsc.c:534-540`, `:827-859`).

## 3. Deterministic Timer Semantics

Two timer structures exist in Rust at `5e60b1c4c`:

- `runtime::timer::TimerHeap` (`src/runtime/timer.rs`): a min-heap of `(deadline, task)` pairs. `src/runtime/mod.rs:200` exports the module, but no other file under the Rust `src/` names `TimerHeap` (checked with `git grep`), so it is a standalone structure, not the timer behind task sleeps.
- `time::wheel::TimerWheel` (`src/time/wheel.rs`): the hierarchical wheel with overflow and optional coalescing.

C has two as well: the flat-arena `src/time/timer_wheel.c`, used by the deadline helpers (`src/time/deadline.c:45`, `:57`), and the scheduler's task-timer min-heap in `src/runtime/scheduler.c`, which drives sleeps (see `docs/CHANNEL_TIMER_SEMANTICS.md` section 2).

### 3.1 TimerHeap Equal-Deadline Tie-Break Contract

`TimerHeap` ordering (`Ord for TimerEntry`, `timer.rs:47-61`):

- primary key: earliest deadline first,
- tie-break key: insertion generation, compared by wrapping signed difference (earlier insertions pop first for equal deadlines),
- final key: task id (to keep `Ord` consistent with `Eq`).

Evidence:

- `insert` takes `next_generation` and increments it with `wrapping_add(1)` (`timer.rs:131-142`). It also makes the task's new entry its only live one: an earlier entry of the same task becomes stale and is skipped on pop (module doc `timer.rs:6-33`; `cancel`, `timer.rs:166`; `pop_expired_into`, `timer.rs:199`). The old extraction did not mention this per-task dedup.
- deterministic equal-deadline test: `same_deadline_pops_in_insertion_order` (`timer.rs:336`)

**C status:** the scheduler's task-timer heap is keyed by (deadline, arm sequence) (`timer_less`, `src/runtime/scheduler.c:101-106`; header comment `src/runtime/scheduler.c:21-22`), the same contract without the task-id key (arm sequences never tie). Under lab dispatch, due sleep timers instead fire in (1 ms tick, registration) order to match the Rust wheel's ready vector, for timers within level 0's 256 ms (`timers_fire_wheel_order`, `src/runtime/scheduler.c:257-325`). `src/time/timer_wheel.c` fires in (deadline, insertion_seq) order (`src/time/timer_wheel.c:230-248`). Rust parity: `lab-dispatch-timer-wheel-order-001` (timers registered out of deadline order that come due in one auto-advance wake their tasks in registration order), `timers-same-deadline-001` (two sleeps to the same instant both fire at 100 ns; dispatch order matches Rust). Unit tests: `tests/unit/time/test_timer_wheel.c` `timer_tiebreak_insertion_order`, `timer_deadline_plus_seq_tiebreak`.

### 3.2 TimerWheel Registration, Cancellation, and Expiry

Registration:

- `try_register` returns `TimerDurationExceeded` when `deadline - current > max_timer_duration` (`wheel.rs:530-546`; default 7 days, `wheel.rs:80-105`). `register` does not reject: it clamps the deadline to the maximum (`wheel.rs:515-524`), so the timer fires early and the caller must re-check.
- Each timer gets `generation = next_generation` (then `wrapping_add(1)`) and `id = active.insert(generation)`, a slab index that is reused after the timer leaves `active` (`insert_validated`, `wheel.rs:550-566`). Live timers are tracked in the slab `active`, not a map.

Cancellation:

- `cancel(handle)` succeeds only when `active[handle.id] == handle.generation`, then removes it (`wheel.rs:577-597`).
- A generation mismatch is rejected without removing the live timer (test `wheel_cancel_rejects_generation_mismatch_without_removing`, `wheel.rs:1377`; wrap test `wheel_register_wraps_id_and_generation_without_immediate_collision`, `wheel.rs:1407`).
- The stored entry stays until it is skipped by `is_live` (`wheel.rs:1071-1075`), purged when `active` empties (`purge_inactive_storage`, `wheel.rs:1100-1110`) or compacted once cancellations since the last compaction exceed `2 * live + 1024` (`wheel.rs:588-591`; `compact_inactive_storage`, `wheel.rs:1116-1140`).

Expiry:

- `collect_expired(now)` (`wheel.rs:699-703`) advances the wheel (`synchronize`, `wheel.rs:502-507`; `advance_to`, `wheel.rs:756-783`) and drains `ready` (`drain_ready`, `wheel.rs:921-968`).
- The deadline boundary is inclusive: a live ready entry fires when `deadline <= now`, and one with `deadline > now` is re-inserted (`wheel.rs:948-956`), subject to the coalescing hold in section 3.3. `drain_ready` drains the ready vector in order (no `swap_remove`), so due entries fire in ready-vector order.

**C status:** `asx_timer_register` (`src/time/timer_wheel.c:139-168`) returns `ASX_E_TIMER_DURATION_EXCEEDED` when `deadline - current > max_duration` (`src/time/timer_wheel.c:118-121`, `:145`); the default maximum is 24 h (`ASX_TIMER_MAX_DURATION_NS`, `include/asx/time/timer_wheel.h:38-39`), not Rust's 7 days, and there is no clamping variant. Each arm advances the slot's 32-bit generation, skipping 0 (`src/time/timer_wheel.c:54-58`, `:124-137`). `asx_timer_cancel` (`src/time/timer_wheel.c:174-192`) requires a live slot with a matching generation and marks it dead at once (no lazy storage). `asx_timer_collect_expired` (`src/time/timer_wheel.c:202-266`) fires live slots with `deadline <= now` (`src/time/timer_wheel.c:222`). Unit tests: `test_timer_wheel.c` `timer_duration_exceeded`, `timer_cancel_prevents_fire`, `timer_stale_handle_cancel_returns_false`, `timer_double_cancel_returns_false`, `timer_generation_increments_on_reuse`, `timer_stale_handle_rejected_after_reset`. No v2 fixture exercises stale-handle cancellation.

### 3.3 Overflow and Coalescing

Overflow:

- A timer whose delta is at or beyond `max_range` (the smaller of `max_wheel_duration`, default 24 h, and the level-3 range) goes to the overflow min-heap (`insert_entry_at`, `wheel.rs:710-754`, overflow at `:718-726`; `max_range_ns`, `wheel.rs:1091-1093`).
- The overflow heap orders by deadline, then lower generation (wrapping signed difference), then id (`Ord for OverflowEntry`, `wheel.rs:276-294`).
- Each tick of `advance_to` calls `refill_overflow` (`wheel.rs:781`), which moves live entries that came within range (or are due) back into the wheel (`wheel.rs:903-919`).

Coalescing (off by default; window 1 ms, `min_group_size` 1; `CoalescingConfig`, `wheel.rs:137-165`):

- When the window containing `now` holds at least `min_group_size` live timers, due entries in that window are held until the window end (`drain_ready`, `wheel.rs:930-933`, `:950-953`; `window_holds_group`, `wheel.rs:1010-1014`). A coalesced timer therefore fires at the first window boundary at or after its deadline (`window_end`, `wheel.rs:63-72`), never before it. The old extraction's "coalesced boundary" with in-window ready counts is replaced by this hold rule.

**C status:** spec (Rust), not implemented in C: `src/time/timer_wheel.c` is a flat arena with no levels, overflow heap or coalescing (header comment `src/time/timer_wheel.c:1-10`). No v2 fixture covers overflow or coalescing.

### 3.4 Reconciliation with Outcome/Budget/Exhaustion Semantics (`bd-296.16`)

Deadline-driven cancellation is superseded by asupersync_v4_formal_semantics.md §3.6 (rule TICK); C status: see C_REFINEMENT_MAP.md row "§3.6 TICK". The spec cancels the region with `Timeout` when its budget deadline passes; Rust's lab and C cancel the task with `Deadline` through a per-task budget-deadline timer (that row rates it Variant). Rust parity: `budget-deadline-sleep-checkpoint-001` (a task with deadline 100 ns sleeps 200 ns; the `Deadline` cancel does not cut the sleep short and the checkpoint after it acknowledges `Deadline`).

The remaining points, checked at `5e60b1c4c`:

- Channel capacity pressure is explicit: `try_reserve`/`try_send` return `Full` (`mpsc.rs:732-794`). `send_evict_oldest` drops a queued message only by caller request and returns it in `Ok(Some(evicted))` (section 2.5). C: `ASX_E_CHANNEL_FULL` (`src/channel/mpsc.c:534-540`).
- `reserve`/`recv` cancellation returns explicit errors (`SendError::Cancelled`, `RecvError::Cancelled`) before any capacity is claimed or message popped (section 2.4). A cancelled reserve still updates the channel's cancellation counter and leaves the waiter queue (`mpsc.rs:1064-1065`), which is bookkeeping, not capacity.
- Timer deadlines are boundary-inclusive (`deadline <= now`, section 3.2).
- Timer duration admission: `try_register` rejects with `TimerDurationExceeded`, but `register` clamps the deadline (truncates the duration) instead of failing (`wheel.rs:515-546`). The old claim "not implicit truncation" holds only for `try_register`. C has only the rejecting form.

Cross-artifact coupling:

- `docs/EXISTING_ASUPERSYNC_STRUCTURE.md` defines budget exhaustion mapping (`deadline`, `poll_quota`, `cost_budget`) and cancellation reason strengthening.
- `docs/LIFECYCLE_TRANSITION_TABLES.md` defines close/quiescence requirements and cancellation propagation.

## 4. Tie-Break Contract for ANSI C Port

Scheduler ordering is superseded by asupersync_v4_formal_semantics.md §1.11 (lanes) and §3.0 (rules ENQUEUE and SCHEDULE-STEP), with §5 INV-SCHED-LANES; C status: see C_REFINEMENT_MAP.md rows "§1.11 lanes, §3.0 ENQUEUE, §5 INV-SCHED-LANES" and "§3.0 SCHEDULE-STEP". The spec has three lanes, Cancel > Timed > Ready, with an EDF timed lane whose deadline ties are broken by task id. The channel and timer rules (1-6 below) are not in the v4 semantics.

Required tie-break and ordering rules, with their status:

1. Channel sender waiters are FIFO; non-blocking reserve must not queue-jump. Rust: section 2.3. C: implemented (section 2.3).
2. Two-phase obligations are linear: every reserve resolves exactly once as commit or abort. Rust: section 2.2 (an unsent drop is an abort). C: send and abort are implemented; an unresolved permit is not aborted at drop (section 2.2).
3. Receiver delivery order is FIFO for committed queue entries. Rust: `VecDeque` `push_back`/`pop_front` (`mpsc.rs:1616`, `:1801`). C: ring buffer (`src/channel/mpsc.c:684-687`, `:747-763`); unit test `test_mpsc.c` `fifo_ordering`.
4. Equal-deadline timer ordering must be deterministic and insertion-stable. Rust: `TimerHeap` (section 3.1) and the wheel's ready vector (section 3.2). C: section 3.1.
5. Timer cancellation must be generation-safe (stale handles fail, live handles remain valid). Rust and C: section 3.2.
6. Coalescing must be deterministic for a fixed logical-time input stream. Rust: the hold rule depends only on the timers and `now` (section 3.3). C: no coalescing.

The plan (`PLAN_TO_PORT_ASUPERSYNC_TO_ANSI_C.md`, section 6.8.3 H) asks for:

- scheduler tie-break key `(lane_priority, logical_deadline, task_id, insertion_seq)`,
- deterministic timer ordering for equal deadlines by insertion sequence,
- no nondeterministic scheduler path in deterministic mode.

What Rust and C actually do differs from that key:

- Rust's cancel and ready lanes order `SchedulerEntry` by higher priority, then earlier generation (insertion order), then task id (`priority.rs:21-41`); the timed lane orders `TimedEntry` by earlier deadline, then generation (wrapping), then task id (`priority.rs:54-75`). Generation comes before task id, and no deadline is involved outside the timed lane. Rust's timed lane breaks deadline ties by generation first, where the spec text says task id.
- Pops with an RNG hint (every `LabRuntime` pop and Phase 2 of the multi-worker scheduler) do not take the heap order among equal-priority entries: `pop_entry_with_rng` collects the top-priority group (up to 256) and takes entry `rng_hint % count` (`priority.rs:759-790`, `tie_break_index` at `priority.rs:417-421`). So equal-priority dispatch is seeded, not FIFO. The `LabRuntime` serves cancel (up to a streak of 16), then timed, then ready, then cancel again (`pop_for_worker`, `lab/runtime.rs:6324-6373`); in `src/lab` only test code feeds its timed lane (`schedule_timed` callers at `lab/runtime.rs:7658` and `:7723`, inside the test module that starts at `:6691`).
- C lab dispatch (the Rust parity path) ports the `LabRuntime` with one worker: entries `{slot, task_gen, priority, gen}` (`src/runtime/lab_dispatch.c:43-48`) with generations from one counter for both lanes (`src/runtime/lab_dispatch.c:58`, `:255`, `:309`); a pop takes the highest-priority group in generation order and picks `r % n` (`lab_pop`, `src/runtime/lab_dispatch.c:344-378`); cancel lane first up to 16 consecutive dispatches, then ready, then cancel (`asx_lab_pick`, `src/runtime/lab_dispatch.c:380-400`). There is no timed lane and no task-id key (generations never tie). The seed and RNG: `src/runtime/lab_dispatch.c:210-239`. Outside lab dispatch, `asx_scheduler_run` polls runnable tasks in ascending arena order each round (`src/runtime/scheduler.c:4-6`).
- Determinism: both engines are deterministic for a fixed seed. Rust parity: `lab-dispatch-tie-break-001` (four tasks contend for a one-permit semaphore at time 0; the seeded tie-break decides which takes it first), `scheduler-yield-interleave-001` (dispatch order follows the seed), `lab-dispatch-cancel-waker-once-001` (a task is dispatched on the cancel lane, then on the ready lane). C status of replay: C_REFINEMENT_MAP.md rows `inv.determinism.replayable` and `def.determinism.seed_equivalence`.

## 5. Fixture Candidate Mapping

None of the candidate IDs below exists as `fixtures/rust_reference_v2/<id>.json` (checked 2026-10-10 against the 66 fixtures in that directory). Each row is marked "not materialized" and, where an existing v2 fixture covers the behaviour (its scenario was read), names it.

| Candidate ID | Rule | Expected result class | Status |
|---|---|---|---|
| `channel-two-phase-001` | reserve -> send -> recv | value delivered, no leaked reservation | Not materialized; covered by `mpsc-two-phase-send-recv-001` |
| `channel-abort-release-002` | reserve -> abort | capacity restored deterministically | Not materialized; no v2 scenario aborts a channel permit (C unit test `abort_returns_capacity`) |
| `channel-drop-permit-abort-003` | reserve -> drop permit | slot released; obligation aborted with `Cancel` (not identical to an explicit abort, which says `Explicit`) | Not materialized; no v2 fixture; not implemented in the C library (section 2.2) |
| `channel-fifo-waiter-004` | queued waiter + try_reserve | try_reserve returns Full; waiter acquires first | Not materialized; no v2 fixture (C unit test `try_reserve_never_jumps_parked_producer`) |
| `channel-reserve-cancel-005` | pending reserve then cancel | waiter removed; no phantom queue entry | Not materialized; no v2 fixture (C unit test `cancelled_waiter_does_not_absorb_wake`) |
| `channel-recv-cancel-nonconsume-006` | recv cancelled with queued value | cancellation returns error; value remains | Not materialized; partly covered by `mpsc-recv-cancel-first-001` (the channel is empty, so non-consumption is not exercised; C unit test `cancelled_cx_wins_over_ready_channel_and_traces` covers it) |
| `channel-evict-reserved-007` | send_evict_oldest with all slots reserved | Full, no reserved-slot eviction | Not materialized; eviction not implemented in C |
| `channel-evict-committed-008` | send_evict_oldest with committed oldest | oldest committed value evicted | Not materialized; eviction not implemented in C |
| `timer-equal-deadline-order-010` | same-deadline inserts | insertion order preserved | Not materialized; covered by `lab-dispatch-timer-wheel-order-001` (wake order is registration order) and `timers-same-deadline-001` (both fire at the same instant) |
| `timer-cancel-generation-011` | stale generation cancel | stale rejected; live handle cancellable | Not materialized; no v2 fixture (C unit tests `timer_stale_handle_cancel_returns_false`, `timer_generation_increments_on_reuse`) |
| `timer-next-deadline-same-tick-012` | same tick sub-ms deadline | actual deadline preserved by next_deadline | Not materialized; no v2 fixture; C's `timer_wheel.c` has no `next_deadline` query (Rust: `wheel.rs:605`) |
| `timer-overflow-promotion-013` | out-of-range timer | promoted and fired when in range | Not materialized; overflow not implemented in C |
| `timer-coalescing-threshold-014` | coalescing min_group_size gate | coalescing only when threshold met | Not materialized; coalescing not implemented in C |

## 6. Implementation Checklist for C

The original checklist, with its status in the C tree at `b412780`:

1. `queue_len + reserved <= capacity` for all channel transitions: holds (section 2.1).
2. FIFO waiter queue and queue-jump prevention (`try_reserve` behavior) are preserved: yes (section 2.3).
3. Permit drop is semantically identical to explicit abort: no. In Rust a drop is an abort with reason `Cancel`, not `Explicit` (`mpsc.rs:1633-1639` vs `:1663-1673`), and the C library has no drop at all: an unresolved permit keeps its slot and its obligation is leaked when the holder completes (section 2.2).
4. Receiver cancellation does not consume committed queue data: yes (section 2.4).
5. Timer handles are generation-safe and stale cancels are rejected: yes, with 32-bit generations (section 3.2).
6. Equal-deadline timer tie-break is insertion-stable and replay-deterministic: yes (section 3.1); fixture-checked for lab sleeps by `lab-dispatch-timer-wheel-order-001`.
7. Overflow/coalescing decisions do not violate deterministic replay identity: not applicable in C, which has neither (section 3.3).
