# Channel/Timer Kernel Semantics and Deterministic Ordering Contract

> **Bead:** bd-296.17
> **Status:** Canonical spec artifact for implementation
> **Provenance:** Extracted from Rust source at `/dp/asupersync/src/channel/mpsc.rs` and `/dp/asupersync/src/time/{wheel.rs,intrusive_wheel.rs,driver.rs,deadline.rs,budget_ext.rs}`
> **Cross-references:** `LIFECYCLE_TRANSITION_TABLES.md` (bd-296.15), outcome/budget semantics (bd-296.16)
> **Rust baseline:** `RUST_BASELINE_COMMIT` (to be pinned per Section 1.1)
> **Last verified:** 2026-02-27 by BlueCat (Claude Opus 4.6)

---

## Table of Contents

1. [MPSC Channel Semantics](#1-mpsc-channel-semantics)
2. [Timer Wheel Semantics](#2-timer-wheel-semantics)
3. [Deterministic Tie-Break Ordering Contract](#3-deterministic-tie-break-ordering-contract)
4. [Budget/Deadline Integration](#4-budgetdeadline-integration)
5. [Reconciliation with Outcome/Exhaustion Semantics](#5-reconciliation-with-outcomeexhaustion-semantics)
6. [Fixture Family Mapping](#6-fixture-family-mapping)
7. [C Port Implications](#7-c-port-implications)

---

## 1. MPSC Channel Semantics

### 1.1 Channel Lifecycle States

The channel does not have an explicit state enum. State is derived from two orthogonal conditions:

| State | Condition | Description |
|-------|-----------|-------------|
| `Open` | `sender_count > 0 && !receiver_dropped` | Normal operation; sends and receives are possible |
| `SenderClosed` | `sender_count == 0 && !receiver_dropped` | All senders dropped; queue may still have messages for drain |
| `ReceiverClosed` | `receiver_dropped == true` | Receiver dropped; new sends fail immediately |
| `FullyClosed` | `sender_count == 0 && receiver_dropped` | Both sides gone; resources reclaimed |

**C enum mapping:**

```c
typedef enum {
    ASX_CHANNEL_OPEN,
    ASX_CHANNEL_SENDER_CLOSED,
    ASX_CHANNEL_RECEIVER_CLOSED,
    ASX_CHANNEL_FULLY_CLOSED
} asx_channel_state;
```

### 1.2 Legal State Transitions

```
Open ──(all senders drop)──> SenderClosed
Open ──(receiver drops)────> ReceiverClosed
SenderClosed ──(receiver drops)──> FullyClosed
ReceiverClosed ──(all senders drop)──> FullyClosed
```

**Forbidden transitions:**
- `ReceiverClosed -> Open` (irreversible; `receiver_dropped` is monotone `false -> true`)
- `SenderClosed -> Open` (resurrection from zero is prevented; see 1.8)
- `FullyClosed -> (any)` (terminal state)

### 1.3 Two-Phase Send Protocol (Reserve/Send/Abort)

This is the core differentiator of asupersync's channel model. It provides cancel-safety by separating capacity reservation from value commitment.

#### Phase 1: Reserve

```
asx_status asx_channel_reserve(asx_channel_sender *tx, asx_cx *cx, asx_send_permit *out_permit);
```

**Contract:**
1. Check cancellation: call `asx_checkpoint(cx)`. If cancelled, return `ASX_E_CANCELLED` without modifying channel state.
2. Check receiver alive: if `receiver_dropped`, return `ASX_E_DISCONNECTED`.
3. Check FIFO position: only the head of the sender wait queue (or a new arrival when queue is empty) may claim a slot.
4. If eligible and capacity available: increment `reserved` count, return `ASX_OK` with a valid `asx_send_permit`.
5. If no capacity: register waker in sender wait queue (FIFO), return `ASX_E_WOULD_BLOCK` (caller must re-poll).

**Capacity invariant (must hold at all times):**

```
used_slots = queue_length + reserved_count <= capacity
```

Reserved slots count against capacity immediately, before any value is supplied.

#### Phase 2: Send (Commit)

```
asx_status asx_send_permit_send(asx_send_permit *permit, void *value);
```

**Contract:**
1. Decrement `reserved` count.
2. If `receiver_dropped`: wake all waiting senders, return `ASX_E_DISCONNECTED`. Value is NOT enqueued.
3. Otherwise: push value to back of message queue (FIFO), wake receiver.
4. The send operation is infallible from the caller's perspective when the receiver is alive (capacity was already reserved).

#### Phase 3: Abort

```
void asx_send_permit_abort(asx_send_permit *permit);
```

**Contract:**
1. Decrement `reserved` count.
2. Wake the next waiting sender (return capacity to pool).
3. No value is enqueued.

#### RAII Cleanup (C Equivalent)

In the C port, `asx_send_permit` must be consumed via `send()` or `abort()`. If the permit is not consumed (e.g., due to error path or cancellation), cleanup must call `abort()` implicitly. This maps to the cleanup-stack pattern.

**The permit MUST be resolved exactly once.** Double-send or double-abort is a linearity violation.

### 1.4 Receive Semantics

```
asx_status asx_channel_recv(asx_channel_receiver *rx, asx_cx *cx, void *out_value);
asx_status asx_channel_try_recv(asx_channel_receiver *rx, void *out_value);
```

**`recv` contract:**
1. Check cancellation: `asx_checkpoint(cx)`. If cancelled, return `ASX_E_CANCELLED`. No message is consumed.
2. If queue non-empty: pop front (FIFO), wake next waiting sender, return `ASX_OK`.
3. If queue empty and `sender_count == 0`: return `ASX_E_DISCONNECTED`.
4. If queue empty and senders alive: register waker, return `ASX_E_WOULD_BLOCK`.

**`try_recv` contract:**
- Same as recv but never blocks. Returns `ASX_E_EMPTY` if queue empty and senders alive.

**Critical: `Disconnected` is returned only when queue is empty AND sender_count == 0.** If senders drop but messages remain, those messages are drained first.

### 1.5 Backpressure Behavior

| Capacity State | `reserve()` | `try_reserve()` | `try_send()` |
|---------------|-------------|------------------|--------------|
| Capacity available, no waiters | `ASX_OK` (immediate) | `ASX_OK` | `ASX_OK` |
| Capacity available, waiters queued | Enters wait queue (FIFO fairness) | `ASX_E_FULL` (won't jump queue) | `ASX_E_FULL` |
| No capacity | Enters wait queue | `ASX_E_FULL` | `ASX_E_FULL` |

**Key fairness rule:** `try_reserve()` refuses to jump the waiter queue even when capacity is available. This prevents starvation of queued waiters by bursty `try_send()` callers.

### 1.6 FIFO Ordering Guarantees

| Guarantee | Scope |
|-----------|-------|
| Per-sender message ordering | Guaranteed: messages from a single sender are received in send order |
| Cross-sender message ordering | NOT guaranteed: depends on which sender's commit executes first |
| Waiter queue ordering | Guaranteed: FIFO queue with strict head-of-queue priority |
| Wakeup ordering | Guaranteed: next sender woken is always the queue head |

### 1.7 Error Taxonomy

| Error Code | Meaning | When Returned |
|-----------|---------|--------------|
| `ASX_E_DISCONNECTED` | Channel peer is gone | Receiver dropped (send side), or all senders dropped + queue empty (recv side) |
| `ASX_E_CANCELLED` | Operation cancelled via `cx.checkpoint()` | At start of `reserve` or `recv` polling |
| `ASX_E_FULL` | No capacity (non-blocking operations only) | `try_reserve`, `try_send` |
| `ASX_E_EMPTY` | No messages (non-blocking receive only) | `try_recv` when queue empty but senders alive |

**Construction constraint:** Channel capacity must be > 0. `capacity == 0` is a fatal error at construction time (not a runtime error).

### 1.8 Close Semantics

#### Receiver Drop (Close from Receiver Side)

1. Set `receiver_dropped = true` (irreversible).
2. Clear receiver waker.
3. **Discard all queued messages** (drop items outside lock to prevent deadlock).
4. Wake all waiting senders (they will see `ASX_E_DISCONNECTED`).

**After receiver drop, all queued messages are lost. There is no way to recover them.**

#### Last Sender Drop (Close from Sender Side)

1. Decrement `sender_count` to 0.
2. Double-check under lock (prevents race with weak-sender upgrade).
3. Wake receiver (it will drain remaining queue, then see `ASX_E_DISCONNECTED`).

**Queue is NOT cleared.** Receiver can drain all buffered messages before seeing disconnect.

| Event | Queue cleared? | Pending senders notified? | Pending receiver notified? |
|-------|---------------|--------------------------|---------------------------|
| Receiver dropped | YES (immediate) | YES (all woken) | N/A |
| Last sender dropped | NO (drain available) | N/A | YES (woken) |

### 1.9 Cancellation Interaction

| Scenario | Effect on Channel State | Value/Slot Disposition |
|----------|------------------------|----------------------|
| Cancel during `reserve` (no permit yet) | None — channel completely unaffected | N/A |
| Cancel while pending in waiter queue | Waiter removed from queue; next waiter woken if capacity available | Reserved slot restored |
| Permit held, sender cancelled/dropped without `send()` | Permit's cleanup calls `abort()` | Reserved slot restored; no value enqueued |
| Cancel during `recv` | None — no message consumed | Message remains in queue |
| Permit committed (`send()`), then receiver cancelled | Message stays in queue until dequeued or receiver dropped | Message may be discarded on receiver drop |

**Critical cancel-safety guarantee:** A cancelled `reserve` leaves zero state change. A cancelled `recv` does not consume any message.

### 1.10 Determinism Requirements

For deterministic replay, the following must hold:

1. **Per-sender FIFO is deterministic** by construction (queue push/pop).
2. **Waiter queue is deterministic** by construction (FIFO queue with monotonic IDs).
3. **Cancellation is deterministic** given deterministic `cx.checkpoint()` results.
4. **Multi-sender interleaving is NOT deterministic** under concurrent execution. Under a single-thread deterministic scheduler with fixed polling order, it becomes deterministic.
5. **Wakeup order is deterministic** (always head-of-queue).

**C port rule:** The single-thread kernel profile (Phase 5) naturally serializes all channel operations, making multi-sender ordering deterministic. The parallel profile (deferred per ADR-001) must address multi-sender interleaving determinism separately.

---

## 2. Timer Wheel Semantics

### 2.1 Timer Wheel Structure

4-level hierarchical timer wheel with 256 slots per level:

| Level | Resolution | Range (256 slots) | Purpose |
|-------|-----------|-------------------|---------|
| L0 | 1 ms | 256 ms | Near-term timers |
| L1 | 256 ms | ~65.5 s | Short-term timers |
| L2 | ~65.5 s | ~4.66 h | Medium-term timers |
| L3 | ~4.66 h | ~49.8 days | Long-term timers (capped at 24h by default) |

**Overflow heap:** Timers beyond `max_wheel_duration` (default 24h) but within `max_timer_duration` (default 7 days) go to a min-heap sorted by deadline.

**Occupied bitmap:** 256-bit bitmap (4 x uint64) per level for O(1) skip optimization.

### 2.2 Timer Insertion

```
asx_status asx_timer_register(asx_timer_wheel *wheel, asx_time deadline, asx_waker waker, asx_timer_handle *out_handle);
asx_status asx_timer_try_register(asx_timer_wheel *wheel, asx_time deadline, asx_waker waker, asx_timer_handle *out_handle);
```

**Contract:**
1. Validate duration: `deadline - current_time`. If exceeds `max_timer_duration`, return `ASX_E_TIMER_DURATION_EXCEEDED`.
2. Assign monotonic `id` and `generation` (both wrapping `uint64_t`).
3. Record `(id, generation)` in active timer map.
4. Place timer in appropriate level:
   - If `deadline <= current_time`: push to ready list (fire immediately on next collect).
   - If `delta >= max_wheel_duration`: push to overflow heap.
   - Otherwise: compute `slot = (deadline_ns / level_resolution_ns) % 256`, insert at coarsest level whose range covers `delta`.
5. Return `asx_timer_handle { id, generation }`.

**Handle semantics:** The handle is an opaque, copyable token. `id` identifies the timer; `generation` prevents stale-handle operations.

### 2.3 Timer Firing

```
size_t asx_timer_collect_expired(asx_timer_wheel *wheel, asx_time now, asx_waker *out_wakers, size_t max_wakers);
```

**Contract:**
1. Advance wheel to `now` (tick L0, cascade L1/L2/L3 as needed).
2. Skip empty ticks using occupied bitmap (O(1) scan per level).
3. Cascade: when L0 cursor wraps, promote entries from L1 current slot back to finer levels. Repeat for L1->L2, L2->L3.
4. Refill from overflow heap: entries whose delta is now within wheel range are moved back into the wheel.
5. Drain ready list: for each entry with `deadline <= now`, check liveness via `active` map. If live, remove from active map and collect waker. If not live (cancelled), skip silently.
6. Return collected wakers. Caller must wake them outside any lock.

**Skip optimization:** If no timers are active (`active` map empty), the entire advance is a single counter update — O(1) regardless of time gap.

**No timer can be lost:** The wheel is purely deadline-based. A timer with `deadline = T` checked at `now = T + N` fires correctly for any `N > 0`.

### 2.4 Timer Cancellation (O(1) Logical Cancel)

```
bool asx_timer_cancel(asx_timer_wheel *wheel, const asx_timer_handle *handle);
```

**Contract:**
1. Look up `handle.id` in active map.
2. If found AND `active_generation == handle.generation`: remove from active map, return `true`.
3. If not found or generation mismatch: return `false` (stale handle or already fired/cancelled).

**This is O(1) (hash map removal).** The physical timer entry in the wheel slot is NOT removed; it is silently skipped at drain time via the liveness check.

**Generation-safe stale-handle prevention:** A stale handle (from a fired or cancelled timer) carries a generation that no longer matches the active map. Even if the `id` has been reused by a new timer, the generation will differ. Stale cancel returns `false` without affecting the live timer.

**Update is cancel + re-register:**

```
asx_timer_handle asx_timer_update(asx_timer_wheel *wheel, const asx_timer_handle *old_handle, asx_time new_deadline, asx_waker waker);
```

Cancel the old handle (logically), register a new timer with fresh `id`/`generation`.

### 2.5 Timer Error Conditions

| Error | When | Recovery |
|-------|------|----------|
| `ASX_E_TIMER_DURATION_EXCEEDED` | `deadline - now > max_timer_duration` (default 7 days) | Reduce deadline or increase `max_timer_duration` in config |
| Generation mismatch on cancel | Stale handle | Return `false`; no action needed |
| Capacity exhaustion | No hard limit in Rust; C port adds resource contract | See Section 5 |

**Wrapping:** Both `id` and `generation` are wrapping `uint64_t`. Collision at wrap-around is practically impossible (requires a single timer to survive 2^64 registrations).

### 2.6 Timer Wheel Invariants

1. Every live timer (in `active` map) has exactly one physical entry in the wheel or ready/overflow structures.
2. `active.len()` is the authoritative count of live timers.
3. Physical entries without matching `active` entries are dead (cancelled or stale) and silently skipped.
4. When `active` map becomes empty, all physical storage is purged to prevent stale waker retention.
5. Cascade never loses entries: re-inserted entries land at finer granularity as they approach their deadline.

---

## 3. Deterministic Tie-Break Ordering Contract

### 3.1 Rust Behavior (Reference)

**Timer wheel (wheel.rs):** Same-deadline timer firing order is **NOT deterministic** in the Rust reference. `drain_ready` uses `swap_remove` which shuffles entries. No ordering guarantee is documented or enforced.

**Intrusive wheel (intrusive_wheel.rs):** Same-deadline firing order is **FIFO within a slot** (insertion order). However, this is the low-level layer, not the production-facing API.

**MPSC channel:** Multi-sender commit ordering is non-deterministic under concurrent execution.

### 3.2 C Port Contract (STRONGER than Rust)

The C port must provide **deterministic tie-break ordering** as specified in Plan Section 6.8.3(H):

**Scheduler tie-break key:**

```
(lane_priority, logical_deadline, task_id, insertion_seq)
```

Where:
- `lane_priority`: numeric priority of the scheduling lane (lower = higher priority)
- `logical_deadline`: deadline timestamp in logical time units
- `task_id`: deterministic task identifier
- `insertion_seq`: monotonic insertion sequence number (tie-break of last resort)

**Timer tie-break key (derived):**

For equal-deadline timers, the C port MUST fire in `insertion_seq` order. This is STRONGER than the Rust reference behavior and is required for deterministic replay.

**Implementation strategy:**
- Replace `swap_remove` semantics with stable ordering in the ready list.
- Use insertion sequence number as final tie-break for equal deadlines.
- The single-thread kernel profile naturally provides deterministic ordering.
- This guarantee must be preserved in the optional parallel profile (when implemented).

### 3.3 Event Sequencing Contract

Every runtime event emitted to the trace journal must carry:

| Field | Type | Purpose |
|-------|------|---------|
| `event_seq` | `uint64_t` | Monotonic global event sequence number |
| `logical_time` | `asx_time` | Logical time at event emission |
| `task_id` | `asx_id` | Task that triggered the event (if applicable) |
| `timer_id` | `uint64_t` | Timer that triggered the event (if timer-related) |
| `channel_id` | `asx_id` | Channel involved (if channel-related) |

The `event_seq` is the definitive ordering key. For any two events, if `event_seq_a < event_seq_b`, then event A is canonically before event B, regardless of their logical time values.

### 3.4 Determinism Summary

| Aspect | Rust Reference | C Port Requirement |
|--------|---------------|-------------------|
| Timer same-deadline order | Non-deterministic (swap_remove) | Deterministic (insertion_seq) |
| Multi-sender channel interleaving | Non-deterministic (OS scheduler) | Deterministic in single-thread profile (scheduler-driven) |
| Event journal ordering | Event sequence number | Event sequence number (identical contract) |
| Cancellation result | Deterministic | Deterministic |
| PRNG streams | Per-seed deterministic | Per-seed deterministic |
| Timer cascade ordering | Deterministic | Deterministic |

---

## 4. Budget/Deadline Integration

### 4.1 Deadline Propagation Rule

```
effective_deadline = min(existing_deadline, new_deadline)
```

**Tighter constraint always wins.** This applies at every scope boundary (region, task, timer).

### 4.2 Budget-Timer Interaction

Budget deadlines are `asx_time` values stored in the budget structure. They do NOT automatically register timers. Integration works as follows:

1. When a task sleeps or waits with a timeout, the effective timeout is `min(requested_timeout, budget_remaining)`.
2. If budget deadline has already elapsed at call time, the operation returns `ASX_E_BUDGET_EXHAUSTED` immediately.
3. Budget exhaustion triggers the cancellation protocol (see LIFECYCLE_TRANSITION_TABLES.md, Cancellation Protocol).

### 4.3 Timer-Cancellation Interaction

When a task is cancelled:
1. Any pending timers associated with the task should be cancelled (O(1) via generation-safe handles).
2. Timer cancellation is cooperative — the timer handle's generation check prevents stale cancellation from affecting unrelated timers.
3. If a timer fires after its task has been cancelled, the task's poll function will observe cancellation via `asx_checkpoint()` and begin cleanup.

### 4.4 Exhaustion Behavior

| Resource | Exhaustion Trigger | Behavior |
|----------|-------------------|----------|
| Timer capacity | `timer_count >= max_timer_nodes` | `ASX_E_RESOURCE_EXHAUSTED` returned from `register` |
| Timer duration | `deadline - now > max_timer_duration` | `ASX_E_TIMER_DURATION_EXCEEDED` |
| Channel capacity | `used_slots >= capacity` | `ASX_E_FULL` (non-blocking) or waiter queue (blocking) |
| Channel queue | N/A in Rust; C port adds hard limit | `ASX_E_RESOURCE_EXHAUSTED` when queue memory exceeds contract |

---

## 5. Reconciliation with Outcome/Exhaustion Semantics

### 5.1 Channel Errors and Outcome Lattice

Channel errors map to the outcome severity lattice:

| Channel Error | Outcome | Severity |
|--------------|---------|----------|
| `ASX_E_DISCONNECTED` | `Err` | Normal error; peer disappeared |
| `ASX_E_CANCELLED` | `Cancelled` | Cancellation signal; higher severity than Err |
| `ASX_E_FULL` | N/A | Not a terminal outcome; caller retries or uses backpressure |
| `ASX_E_EMPTY` | N/A | Not a terminal outcome; caller retries |

**Outcome join rule:** If a task encounters `Cancelled` on a channel operation, and its natural outcome would have been `Ok` or `Err`, the worst severity (`Cancelled`) is the final outcome.

### 5.2 Timer Errors and Outcome Lattice

| Timer Error | Outcome | Severity |
|------------|---------|----------|
| `ASX_E_TIMER_DURATION_EXCEEDED` | `Err` | Configuration error |
| `ASX_E_RESOURCE_EXHAUSTED` | `Err` | Resource contract violation |
| `ASX_E_BUDGET_EXHAUSTED` | Triggers cancellation | Escalates to `Cancelled` outcome |

### 5.3 Exhaustion Semantics Contract

Per the resource-contract engine (Plan Section 6.13, Risk 8):

1. **Failure-atomic:** Channel and timer operations that fail due to exhaustion must leave all data structures in a consistent state. No partial updates.
2. **Deterministic error codes:** The specific error returned must be deterministic given the same sequence of operations and resource limits.
3. **No silent degradation:** Exhaustion is never masked. The caller always receives an explicit error code.

### 5.4 Channel/Timer Interaction with Region Close

During region close (Closing -> Draining -> Finalizing -> Closed):

| Phase | Channel Behavior | Timer Behavior |
|-------|-----------------|----------------|
| `Closing` | No new channels created; existing channels continue | No new timers created; existing timers continue |
| `Draining` | Senders may complete pending sends; receivers drain | Timers fire normally; cancelled tasks' timers are cancelled |
| `Finalizing` | Channel cleanup; permits aborted; queues drained | Remaining timers cancelled via handle generation check |
| `Closed` | All channel resources reclaimed | All timer resources reclaimed |

**Quiescence invariant extension:** A region cannot reach `Closed` if any channel has live reserved permits or if any timer is still active in the region's scope.

---

## 6. Fixture Family Mapping

### 6.1 Channel Fixture Families

| Fixture Family ID | Description | Key Invariants Tested |
|-------------------|-------------|----------------------|
| `ch-reserve-001` | Reserve, send, recv happy path | FIFO order, capacity accounting |
| `ch-reserve-002` | Reserve, abort | Slot restoration, next waiter wakeup |
| `ch-reserve-003` | Reserve, cancel before send | RAII cleanup, zero state change |
| `ch-backpressure-001` | Full channel, waiter queue ordering | FIFO waiter order, no queue jumping |
| `ch-backpressure-002` | Backpressure release on recv | Wakeup propagation, cascade |
| `ch-disconnect-001` | Receiver drop, message discard | All messages lost, all senders notified |
| `ch-disconnect-002` | Last sender drop, queue drain | Messages preserved, receiver sees Disconnected after drain |
| `ch-cancel-001` | Cancel during reserve (pending) | Waiter removed, capacity restored |
| `ch-cancel-002` | Cancel during recv | No message consumed, message still available |
| `ch-cancel-003` | Permit held, sender cancelled | Abort via cleanup-stack |
| `ch-multi-sender-001` | Multiple senders, deterministic order | Per-sender FIFO preserved |
| `ch-exhaustion-001` | Resource limit hit on channel | Failure-atomic, deterministic error |
| `ch-linearity-001` | Permit resolved exactly once | No double-send, no double-abort |

### 6.2 Timer Fixture Families

| Fixture Family ID | Description | Key Invariants Tested |
|-------------------|-------------|----------------------|
| `tm-insert-fire-001` | Register, advance, fire | Correct deadline, waker invoked |
| `tm-cancel-001` | Register, cancel, advance | Timer does not fire, handle returns true |
| `tm-cancel-002` | Stale handle cancel | Returns false, no effect on live timers |
| `tm-cancel-003` | Cancel after fire | Returns false (already fired) |
| `tm-tiebreak-001` | Same-deadline timers, deterministic order | Insertion-seq ordering (C port contract) |
| `tm-cascade-001` | Timer placement across levels | Correct level selection and cascade |
| `tm-overflow-001` | Timer beyond max_wheel_duration | Overflow heap, refill on approach |
| `tm-skip-001` | Large time jump, skip optimization | All deadlines still fire correctly |
| `tm-update-001` | Update timer deadline | Old handle invalid, new handle valid |
| `tm-exhaustion-001` | Resource limit hit on timer pool | Failure-atomic, deterministic error |
| `tm-budget-001` | Budget deadline triggers cancellation | Cancel propagation, outcome = Cancelled |
| `tm-duration-exceeded-001` | Register beyond max_timer_duration | Error returned, no timer created |
| `tm-generation-wrap-001` | ID/generation wrapping at uint64 max | No collision, correct liveness check |

### 6.3 Integration Fixture Families

| Fixture Family ID | Description | Key Invariants Tested |
|-------------------|-------------|----------------------|
| `int-channel-cancel-001` | Channel send cancelled by timer timeout | Budget exhaustion -> cancel -> abort permit |
| `int-timer-region-close-001` | Region close with active timers | Timers cancelled during finalization |
| `int-channel-region-close-001` | Region close with active channel | Permits aborted, queue drained |
| `int-replay-channel-001` | Channel operations replay with identical digest | Deterministic ordering preserved |
| `int-replay-timer-001` | Timer operations replay with identical digest | Tie-break ordering preserved |

---

## 7. C Port Implications

### 7.1 MPSC Channel C Design Notes

1. **Bounded ring buffer or array-backed queue** replaces Rust's `VecDeque<T>`. Capacity is fixed at construction.
2. **Reserved count** is a simple integer counter. The two-phase protocol is the same.
3. **Waiter queue** is an intrusive linked list or fixed array with FIFO ordering. Monotonic waiter IDs prevent ABA problems.
4. **No weak references:** The C port uses generation-safe handles for channel lifetime, not reference counting. Channel handle validity is checked via the handle's generation against the channel's current generation.
5. **Cleanup-stack integration:** `asx_send_permit` is registered on the cleanup stack at creation. If the owning task is cancelled before the permit is resolved, the cleanup stack calls `abort()`.
6. **Lock discipline:** All wakeups must happen outside the channel's internal lock. The C port must follow the same pattern to prevent deadlock.

### 7.2 Timer Wheel C Design Notes

1. **Fixed-size arena for timer entries** replaces Rust's dynamic allocation. The resource contract `max_timer_nodes` from `asx_runtime_config` sets the hard limit.
2. **Generation-safe handles** work identically: `(id, generation)` pair with O(1) cancel via hash map (or direct-mapped array if IDs are dense).
3. **Deterministic tie-break** requires stable ordering in the ready list. Use insertion-sequence-ordered data structure instead of `swap_remove` semantics.
4. **Occupied bitmap** for skip optimization translates directly to C: 4 x `uint64_t` per level with `__builtin_ctzll` or portable equivalent.
5. **VirtualClock** for deterministic testing: the C port's time abstraction (`asx_clock` vtable) must support a virtual clock mode that advances only when explicitly stepped.
6. **Overflow handling:** Use a min-heap with fixed capacity from the resource contract.

### 7.3 Determinism Strengthening

The C port is **stronger than Rust** on determinism:

| Aspect | Rust | C Port |
|--------|------|--------|
| Same-deadline timer order | Unspecified | Insertion-sequence order (deterministic) |
| Channel multi-sender order | OS-scheduler-dependent | Scheduler-driven (deterministic in single-thread) |
| Event journal | Sequence-numbered | Sequence-numbered (identical) |

This strengthening is intentional and required by Plan Section 6.8.3(H). It does NOT create a parity gap because:
- The C behavior is a strict refinement of the Rust behavior (every C ordering is also a valid Rust ordering).
- Conformance fixtures test for deterministic C ordering, which is a subset of valid Rust orderings.

### 7.4 Invalid Handle Behavior

| Operation | Invalid Handle | Stale Handle (generation mismatch) |
|-----------|---------------|-------------------------------------|
| Timer cancel | Return `false` | Return `false` |
| Timer update | Treat as new registration | Cancel returns false; new registration proceeds |
| Channel send via permit | Undefined (permit is RAII — must not be reused) | N/A (permits don't use handle pattern) |

**Ghost monitor coverage:** In debug builds, the ghost protocol monitor must detect and flag:
- Double-cancel of same timer handle
- Use of permit after send/abort
- Channel operation on closed channel (beyond simple error return)
