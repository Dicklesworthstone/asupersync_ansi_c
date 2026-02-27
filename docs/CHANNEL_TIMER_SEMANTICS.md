# Channel, Timer, and Scheduler Semantics — Canonical Reference

> **Bead:** bd-296.17
> **Scope:** Deterministic channel/timer kernel semantics and tie-break ordering contract
> **Status:** Canonical extraction slice (ready for C implementation)
> **Rust baseline commit:** `38c152405bd03e2bd9eecf178bfbbe9472fed861`
> **Last verified:** 2026-02-27 by MossySeal (Claude Opus 4.6)
> **Purpose:** Implementation-ready semantic contracts for MPSC channels, hierarchical timer wheel, and deterministic scheduler — the foundation for replay parity and bounded cleanup

---

## Table of Contents

1. [MPSC Channel Semantics](#1-mpsc-channel-semantics)
2. [Timer Wheel Semantics](#2-timer-wheel-semantics)
3. [Deterministic Scheduler Semantics](#3-deterministic-scheduler-semantics)
4. [Cross-Domain Interactions](#4-cross-domain-interactions)
5. [Deterministic Tie-Break Contract](#5-deterministic-tie-break-contract)
6. [Failure Paths and Invalid-Handle Behavior](#6-failure-paths-and-invalid-handle-behavior)
7. [Fixture Family Mapping](#7-fixture-family-mapping)
8. [Invariant Schema Cross-Reference](#8-invariant-schema-cross-reference)

---

## Source Provenance

Primary Rust sources for this extraction:

| Source File | Domain | Lines |
|-------------|--------|-------|
| `/dp/asupersync/src/channel/mpsc.rs` | MPSC channel core | 1679 |
| `/dp/asupersync/src/channel/session.rs` | Obligation-tracked channel | ~170 |
| `/dp/asupersync/src/time/wheel.rs` | Hierarchical timer wheel | 2123 |
| `/dp/asupersync/src/time/driver.rs` | Timer driver/integration | ~550 |
| `/dp/asupersync/src/time/deadline.rs` | Deadline abstraction | 168 |
| `/dp/asupersync/src/runtime/timer.rs` | Runtime timer heap | 271 |
| `/dp/asupersync/src/runtime/scheduler/three_lane.rs` | Scheduler loop | ~5200 |
| `/dp/asupersync/src/runtime/scheduler/priority.rs` | Priority scheduling | ~730 |
| `/dp/asupersync/src/runtime/scheduler/global_injector.rs` | Global task injection | ~450 |
| `/dp/asupersync/src/runtime/scheduler/stealing.rs` | Work stealing | ~200 |
| `/dp/asupersync/src/runtime/scheduler/intrusive.rs` | Intrusive queue | ~107 |

Cross-check context:
- `docs/LIFECYCLE_TRANSITION_TABLES.md` (bd-296.15)
- `docs/EXISTING_ASUPERSYNC_STRUCTURE.md` (bd-296.16)

---

## 1. MPSC Channel Semantics

### 1.1 Channel State Model

The channel uses **implicit state encoding** via atomic fields (no explicit state enum):

| Implicit State | Condition | Description |
|----------------|-----------|-------------|
| **Open** | `receiver_dropped == false` AND `sender_count > 0` | Both sides active |
| **Half-Closed (Rx)** | `receiver_dropped == true` AND `sender_count > 0` | Receiver dropped; senders observe disconnect |
| **Half-Closed (Tx)** | `receiver_dropped == false` AND `sender_count == 0` | All senders dropped; receiver drains queue |
| **Closed** | `receiver_dropped == true` AND `sender_count == 0` | Both sides gone |

**Rust source:** `mpsc.rs:106-118` (`ChannelShared` struct)

**State transitions are monotone:** `receiver_dropped` transitions `false -> true` exactly once; `sender_count` decrements monotonically to zero.

### 1.2 Capacity Model

Capacity is **fixed at creation** and never changes:

```
channel(capacity) where capacity > 0  // panics on zero
used_slots = queue.len() + reserved
INVARIANT: used_slots <= capacity
```

**Critical:** Both queued messages AND reserved (uncommitted) permits consume capacity. This prevents a scenario where all slots are reserved but no messages can be sent.

**Rust source:** `mpsc.rs:140-147` (`used_slots`, `has_capacity`), `mpsc.rs:161-175` (channel factory)

### 1.3 Two-Phase Send Protocol (Reserve/Send/Abort)

The channel implements a **two-phase commit** for sends:

#### Phase 1: Reserve

```
reserve(cx) -> Poll<Result<SendPermit, SendError<()>>>
```

**Preconditions checked in order:**
1. Cancellation checkpoint: if `cx.checkpoint().is_err()` -> `SendError::Cancelled(())`
2. Receiver alive: if `receiver_dropped == true` -> `SendError::Disconnected(())`
3. FIFO fairness: if waiter queue non-empty AND caller is not head -> `Poll::Pending`
4. Capacity: if `used_slots < capacity` -> increment `reserved`, return `SendPermit`
5. Otherwise: register in waiter queue with monotonic ID, return `Poll::Pending`

**Rust source:** `mpsc.rs:337-407`

#### Phase 2a: Send (Commit)

```
SendPermit::send(self, value: T)          // infallible after reserve
SendPermit::try_send(self, value: T)      // fails only if receiver dropped post-reserve
```

- Marks permit as consumed (`sent = true`)
- Decrements `reserved`, pushes value to back of queue
- Wakes receiver if waiting

**Rust source:** `mpsc.rs:545-585`

#### Phase 2b: Abort (Rollback)

```
SendPermit::abort(self)
```

- Marks permit as consumed
- Decrements `reserved`, wakes next sender in waiter queue (cascade)

**Rust source:** `mpsc.rs:588-602`

#### RAII Drop Safety

If a `SendPermit` is dropped without calling `send()` or `abort()`, the `Drop` impl runs abort logic:

```
Drop for SendPermit: if !self.sent { reserved -= 1; cascade_wake_next() }
```

**Rust source:** `mpsc.rs:605-623`

### 1.4 Backpressure and Waiter Queue

When the channel is full (`used_slots >= capacity`):

1. `reserve()` returns `Poll::Pending`
2. Sender registered in `send_wakers` queue with monotonic waiter ID
3. Queue is `VecDeque<SendWaiter>` — strict FIFO ordering

**Backpressure release triggers:**
- Receiver consumes a message (`queue.pop_front()`)
- Permit dropped/aborted (decrements `reserved`)
- Both trigger cascade wake of next sender in queue

**FIFO fairness enforcement:** `try_reserve()` returns `Full` when waiters exist, even if capacity is available. This prevents queue-jumping.

**Rust source:** `mpsc.rs:214-225` (try_reserve FIFO check), `mpsc.rs:382-407` (waiter registration)

### 1.5 Eviction Mode

A special `send_evict_oldest()` method can force-evict the oldest queued message:

```
send_evict_oldest(value: T) -> Result<Option<T>, SendError<T>>
```

- If capacity available: normal send, returns `Ok(None)`
- If queue non-empty: pops oldest message, pushes new, returns `Ok(Some(evicted))`
- If all capacity is reserved (no queued messages to evict): returns `Err(Full(value))`
- If receiver dropped: returns `Err(Disconnected(value))`

**Rust source:** `mpsc.rs:281-321`

### 1.6 Receive Semantics

```
recv(cx) -> Poll<Result<T, RecvError>>
try_recv() -> Result<T, RecvError>
```

**Receive order:**
1. Cancellation checkpoint: if `cx.checkpoint().is_err()` -> `RecvError::Cancelled`
2. Queue non-empty: `queue.pop_front()` -> `Ok(value)` (FIFO)
3. All senders dropped AND queue empty: `RecvError::Disconnected`
4. Otherwise: register waker, return `Poll::Pending`

**Cancel safety:** Cancelled receive does NOT consume a message. The message remains in the queue for the next successful receive.

**Rust source:** `mpsc.rs:857-895`

### 1.7 Error Taxonomy

#### Send Errors

| Error | Condition | Carries Value |
|-------|-----------|---------------|
| `SendError::Disconnected(T)` | Receiver dropped | Yes |
| `SendError::Cancelled(T)` | Cancellation checkpoint triggered | Yes |
| `SendError::Full(T)` | Channel full (try_reserve/try_send only) | Yes |

#### Receive Errors

| Error | Condition |
|-------|-----------|
| `RecvError::Disconnected` | All senders dropped AND queue empty |
| `RecvError::Cancelled` | Cancellation checkpoint triggered |
| `RecvError::Empty` | Queue empty (try_recv only), senders still alive |

**Rust source:** `mpsc.rs:32-76`

### 1.8 Cancellation Interaction

**Checkpoint-based cancellation** at operation boundaries:

| Operation | Cancel Effect |
|-----------|--------------|
| `reserve()` poll | Returns `Cancelled`, no capacity consumed, waiter removed from queue |
| `recv()` poll | Returns `Cancelled`, no message consumed |
| `SendPermit::send()` | Not cancellation-checked (already committed) |
| `SendPermit::abort()` | Not cancellation-checked (already releasing) |

**Waiter queue cleanup on cancel:** When a `Reserve` future is cancelled mid-poll, its `Drop` impl removes it from `send_wakers` by ID and cascades wake if capacity freed.

**Rust source:** `mpsc.rs:337-345` (reserve cancel), `mpsc.rs:405-427` (Reserve Drop), `mpsc.rs:879-880` (recv cancel)

### 1.9 Obligation Integration

The `session` module wraps base channel with explicit obligation tracking:

| Component | Base Channel | Session-Tracked |
|-----------|-------------|-----------------|
| Sender type | `Sender<T>` | `TrackedSender<T>` |
| Permit type | `SendPermit<T>` | `TrackedPermit<T>` (contains `ObligationToken<SendPermit>`) |
| Send result | `()` | `CommittedProof<SendPermit>` |
| Abort result | `()` | `AbortedProof<SendPermit>` |
| Leak behavior | Capacity freed silently | **PANIC**: "OBLIGATION TOKEN LEAKED: SendPermit" |

`#[must_use]` on `TrackedPermit` — compiler warning if not consumed.

**Rust source:** `session.rs:57-173`

### 1.10 Close/Drain Semantics

#### Receiver Drop (Channel Close)

1. Sets `receiver_dropped = true` (monotone flag)
2. Drains queue via `mem::take()` — pending messages are **DROPPED** (not delivered)
3. Clears recv waker
4. Wakes ALL waiting senders (they observe disconnect on next poll)
5. Items dropped outside lock (prevents deadlock if `T::drop` touches channel)

**Rust source:** `mpsc.rs:928-967`

#### Last Sender Drop

1. Decrements `sender_count` to 0
2. Wakes receiver (receiver will return `Disconnected` after draining remaining queue)

**Rust source:** `mpsc.rs:481-500`

#### Drain-After-Sender-Drop

Receiver can still drain messages from queue after all senders drop. Returns `Ok(value)` until queue empty, then `Disconnected`.

### 1.11 Ordering Guarantees

| Dimension | Ordering | Mechanism |
|-----------|----------|-----------|
| **Message delivery** | FIFO | `VecDeque`: `push_back()` / `pop_front()` |
| **Waiter scheduling** | FIFO | Monotonic waiter ID, pop-from-front |
| **Cascade wake** | FIFO | Head of waiter queue woken first |
| **try_reserve fairness** | Strict FIFO | Returns `Full` when waiters queued, even if capacity exists |

---

## 2. Timer Wheel Semantics

### 2.1 Structure: 4-Level Hierarchical Wheel

The timer wheel is **hierarchical** with 4 levels plus overflow:

| Level | Slots | Resolution | Range |
|-------|-------|------------|-------|
| 0 | 256 | 1 ms | 256 ms |
| 1 | 256 | 256 ms | 65.536 s |
| 2 | 256 | 65.536 s | ~16.78 min |
| 3 | 256 | ~4295 s | ~37.2 hours |
| Overflow | BinaryHeap | N/A | Beyond wheel range (>24h default, configurable) |

**Slot assignment:** `slot = (deadline_tick % 256)` (hash into level)

**Bitmap occupation:** Each level has 4 x u64 bitmap words for O(1) skip of empty slots.

**Rust source:** `wheel.rs:43-352`

### 2.2 Timer Entry Structure

```
TimerEntry {
    deadline: Time,      // When to fire
    waker: Waker,        // What to wake
    id: u64,             // Unique timer ID
    generation: u64,     // Use-after-cancel prevention
}
```

Active timers tracked in `HashMap<id, generation>` for O(1) cancel validation.

### 2.3 Insert Semantics

```
try_register(deadline, waker) -> Result<TimerHandle, TimerDurationExceeded>
```

**Placement logic:**
1. If `deadline <= current_time`: pushed to `ready` queue (immediately expired)
2. If `delta >= max_wheel_duration`: pushed to overflow `BinaryHeap` (too far)
3. Otherwise: placed in appropriate level slot based on delta magnitude

**Duplicate handling:** No deduplication. Multiple timers with same deadline stored in insertion order in the same slot Vec.

**Validation:** Returns `TimerDurationExceeded` if duration exceeds configured max (default 7 days).

**Generation tracking:** Each registration allocates unique `(id, generation)` pair. Both wrap on u64 overflow using wrapping arithmetic.

**Rust source:** `wheel.rs:448-491`, `wheel.rs:581-626`

### 2.4 Fire Semantics

```
collect_expired(now) -> WakerBatch
```

**Process:**
1. **Advance:** Compute `target_tick`, advance cursor through Level 0 slots
2. **Cascade:** When Level N cursor wraps to 0, advance Level N+1 cursor and re-insert entries
3. **Generation check:** Dead entries (generation mismatch) silently dropped during cascade and drain
4. **Collect:** All live entries with `deadline <= now` added to waker batch
5. **Coalescing** (optional): Timers within coalescing window promoted to batch if group threshold met

**Fire criterion:** `entry.deadline <= now` (inclusive equality — fires at or after deadline)

**Rust source:** `wheel.rs:569-888`

### 2.5 Cancel Semantics

```
cancel(handle) -> bool
```

**Lazy deletion model:**
1. Check `active` map: if `active[handle.id] == handle.generation`, remove entry -> return `true`
2. Generation mismatch or missing: return `false` (already cancelled or stale handle)
3. **No physical removal** from wheel slots — entry remains but is skipped at fire time via `is_live()` check
4. When all timers cancelled (`active` map empty): `purge_inactive_storage()` clears all slot vectors and bitmaps

**Rust source:** `wheel.rs:499-513`

### 2.6 Generation-Safe Handle Contract

```
TimerHandle { id: u64, generation: u64 }
```

**Invariants:**
- `id` wraps independently of `generation` (both use `wrapping_add(1)`)
- Cancel requires exact `(id, generation)` match
- After cancel, same `id` may be reused with different `generation` — old handles cannot cancel new timer
- Safe across u64 wrap: HashMap stores both old and new entries simultaneously

**Rust source:** `wheel.rs:206-225`, `wheel.rs:474-490`

### 2.7 Configuration

| Parameter | Default | Purpose |
|-----------|---------|---------|
| `max_wheel_duration` | 24 hours | Max duration for wheel placement (beyond goes to overflow heap) |
| `max_timer_duration` | 7 days | Max duration accepted by `try_register()` |

**Rust source:** `wheel.rs:63-112`

### 2.8 Timer Ordering Within Deadline

| Condition | Ordering | Mechanism |
|-----------|----------|-----------|
| Different deadlines | Deadline order (earliest first) | Wheel cursor advancement |
| Same deadline, same slot | Insertion order | `Vec::push()` appends; iteration preserves order |
| Overflow heap | Deadline then generation | `BinaryHeap` with `Ord` comparing deadline then generation |

---

## 3. Deterministic Scheduler Semantics

### 3.1 Three-Lane Architecture

The scheduler has three priority lanes with strict ordering:

| Lane | Priority | Queue Type | Ordering |
|------|----------|------------|----------|
| **Cancel** | Highest | Lock-free `SegQueue` (global) + `BinaryHeap` (local) | Priority-ordered, FIFO within priority |
| **Timed** | Middle | `Mutex<TimedQueue>` (global) + `BinaryHeap` (local) | EDF (earliest deadline first), FIFO within deadline |
| **Ready** | Lowest | Lock-free `SegQueue` (global) + `BinaryHeap` (local) | Priority-ordered, FIFO within priority |

**Lane priority override via governor suggestion:**

| Suggestion | Lane Order |
|------------|------------|
| `NoPreference` (default) | Cancel > Timed > Ready |
| `MeetDeadlines` | Timed > Cancel > Ready |
| `DrainObligations` | Cancel > Timed > Ready (doubled cancel-streak limit) |
| `DrainRegions` | Cancel > Timed > Ready (doubled cancel-streak limit) |

**Rust source:** `three_lane.rs:1706-1823`, `global_injector.rs:64-94`

### 3.2 Scheduler Loop (6-Phase)

```
run_loop():
  while !shutdown:
    Phase 0: Process expired timers (fires wakers -> injects tasks)
    Phase 1: Global queues (cancel/timed per suggestion order)
    Phase 2: Local PriorityScheduler (cancel/timed lanes)
    Phase 3: Fast ready paths (lock-free global, local stack)
    Phase 3b: Local ready with RNG hint
    Phase 4: Work stealing (deterministic RNG-seeded target selection)
    Phase 5: Fallback cancel (when cancel-streak limit exceeded)
    Phase 6: Backoff/Park (spin -> yield -> park with timeout)
```

**Rust source:** `three_lane.rs:1706-1993`

### 3.3 Entry Ordering Contracts

#### SchedulerEntry (Cancel/Ready lanes)

```
Compare:
  1. Higher priority first (u8, higher value = more important)
  2. Earlier generation first (lower number = earlier insertion)
```

**Rust source:** `priority.rs:19-36`

#### TimedEntry (Timed lane)

```
Compare:
  1. Earlier deadline first
  2. Earlier generation first (FIFO for equal deadlines)
```

**Rust source:** `priority.rs:54-66`

### 3.4 Generation Counter (FIFO Guarantee)

Each `PriorityScheduler` maintains a monotone `next_generation: u64` counter. Every entry insertion increments and records the generation. This ensures stable FIFO ordering for equal priorities/deadlines.

**Rust source:** `priority.rs:376-380`

### 3.5 RNG-Based Deterministic Tie-Breaking

For the local ready lane, when multiple entries share the same priority:

1. Collect all entries with equal priority from heap
2. Compute `idx = rng_hint % count`
3. Select entry at `idx`, push remaining back

The RNG is seeded deterministically per worker: `DetRng::new(worker_id as u64)`

**Rust source:** `priority.rs:625-673`, `three_lane.rs:818`

### 3.6 Work Stealing

Deterministic work stealing scans other workers in RNG-seeded circular order:

```
steal_task(stealers, rng):
  start = rng.next_usize(stealers.len())
  for i in 0..stealers.len():
    idx = (start + i) % stealers.len()
    if stealers[idx].steal() -> return task
  return None
```

**Rust source:** `stealing.rs:11-27`

### 3.7 Cancel-Streak Fairness

The scheduler limits consecutive cancel-lane dispatches via a streak counter:

- **Base limit:** 16 (configurable)
- **Doubled under:** `DrainObligations` or `DrainRegions` governor suggestions
- When limit exceeded: skip cancel lane for one iteration, dispatch from timed/ready
- Streak resets to 0 on park

This prevents cancellation storms from starving normal tasks.

**Rust source:** `three_lane.rs:1874-1990`

### 3.8 Fairness Certificate and Replay

The scheduler produces a `FairnessPreemptionCertificate` for deterministic replay verification:

```
FairnessPreemptionCertificate {
    cancel_dispatches: u64,
    timed_dispatches: u64,
    ready_dispatches: u64,
    fallback_cancel_dispatches: u64,
    base_limit_exceedances: u64,
    effective_limit_exceedances: u64,
    max_effective_limit_observed: usize,
}
```

**Witness hash:** `DetHasher` over dispatch counts produces a deterministic u64 fingerprint. Same dispatch trace -> same witness hash.

**Rust source:** `three_lane.rs:1512-1540`, `three_lane.rs:1450-1465`

### 3.9 Queue Data Structures

| Structure | Location | Purpose |
|-----------|----------|---------|
| `BinaryHeap<SchedulerEntry>` | Local cancel/ready | Priority + generation ordering |
| `BinaryHeap<TimedEntry>` | Local timed | EDF + generation ordering |
| `SegQueue<PriorityTask>` | Global cancel/ready | Lock-free unbounded FIFO injection |
| `Mutex<TimedQueue>` | Global timed | EDF heap with generation counter |
| `IntrusiveStack` | Local fast ready | LIFO push/pop (owner), FIFO steal (thief) |
| `DetHashSet<TaskId>` | Membership tracking | O(1) "is scheduled?" check |

**Rust source:** `priority.rs:250-380`, `global_injector.rs:72-103`, `intrusive.rs:79-107`

### 3.10 Backoff/Park Behavior

When no tasks are available:

| Phase | Iterations | Action |
|-------|------------|--------|
| Spin | 8 | Busy-wait (check queues between spins) |
| Yield | 2 | Thread yield (OS scheduler cooperative) |
| Park | 1 | Conditional park with timeout based on next deadline |

Park timeout = min(next_timer_deadline, next_timed_entry_deadline).

**Rust source:** `three_lane.rs:1732-1821`

---

## 4. Cross-Domain Interactions

### 4.1 Channel <-> Cancellation

| Interaction | Behavior |
|-------------|----------|
| Task cancelled while reserve pending | `Reserve` future polls `checkpoint()` -> `SendError::Cancelled`. Waiter removed from queue. No capacity consumed. |
| Task cancelled while recv pending | `poll_recv()` polls `checkpoint()` -> `RecvError::Cancelled`. No message consumed. |
| Region closing with channel open | Channel continues operating. Channel closure is independent of region lifecycle. |
| Obligation leak on permit drop | `TrackedPermit` panics if dropped without send/abort. Base `SendPermit` silently aborts. |

### 4.2 Timer <-> Cancellation

| Interaction | Behavior |
|-------------|----------|
| Task cancelled with pending timer | Higher layer (runtime) cancels timer via `cancel(handle)`. Timer entry marked dead in `active` map. |
| Timer fires for cancelled task | Waker called, but task's cancel checkpoint will trigger on next poll. |
| Region closing with timers pending | Timers fire normally during drain phase. Cleanup timers may be registered during Finalizing. |

### 4.3 Timer <-> Scheduler

| Interaction | Behavior |
|-------------|----------|
| Timer expires | `process_timers()` in Phase 0 of scheduler loop. Expired wakers called. Woken tasks injected into scheduler lanes. |
| Timer deadline -> timed lane | Timer fires waker -> task wakes -> injected into ready/timed lane depending on task properties. |
| No timers pending | Scheduler park timeout = infinity (waits for external wake). |
| Next timer deadline | Scheduler park timeout = `min(next_deadline - now)` for responsive timer processing. |

### 4.4 Channel <-> Scheduler

| Interaction | Behavior |
|-------------|----------|
| Channel send wakes receiver | Receiver's waker called. Receiving task injected into scheduler ready lane. |
| Channel recv wakes sender | Next sender in waiter queue woken (cascade). Sending task injected into scheduler ready lane. |
| Backpressure blocks sender | Sender task parks until woken by receiver consumption or abort cascade. |

### 4.5 Channel <-> Obligation (Session Layer)

| Interaction | Behavior |
|-------------|----------|
| Reserve creates obligation | `TrackedPermit` contains `ObligationToken<SendPermit>` in Reserved state |
| Send resolves obligation | `send()` transitions obligation Reserved -> Committed, returns `CommittedProof` |
| Abort resolves obligation | `abort()` transitions obligation Reserved -> Aborted, returns `AbortedProof` |
| Drop without resolution | **PANIC**: obligation leaked. Linearity violation. |

### 4.6 Channel/Timer <-> Outcome/Budget (bd-296.16)

| Interaction | Behavior |
|-------------|----------|
| Channel send fails (Disconnected) | Produces `Err` outcome at task level. Err severity > Ok in outcome lattice. |
| Channel send cancelled | Produces `Cancelled` outcome at task level. Cancelled severity > Err. |
| Timer fires deadline miss | May trigger cancellation with `DeadlineMiss` cancel kind. |
| Budget exhaustion during channel ops | Cleanup budget governs how many pending operations can complete. Exhausted budget -> remaining ops aborted. |

---

## 5. Deterministic Tie-Break Contract

### 5.1 Complete Ordering Summary

| Domain | Primary Key | Secondary Key | Tertiary Key |
|--------|------------|---------------|--------------|
| **Channel messages** | FIFO (VecDeque) | N/A | N/A |
| **Channel waiters** | FIFO (monotonic ID) | N/A | N/A |
| **Timer wheel (same slot)** | Insertion order (Vec) | N/A | N/A |
| **Timer overflow heap** | Deadline | Generation (FIFO) | N/A |
| **Scheduler cancel lane** | Priority (u8, higher first) | Generation (FIFO) | N/A |
| **Scheduler timed lane** | Deadline (EDF) | Generation (FIFO) | N/A |
| **Scheduler ready lane** | Priority (u8, higher first) | Generation (FIFO) | N/A |
| **Scheduler local ready** | Priority | RNG % count (deterministic) | N/A |
| **Work stealing** | RNG-seeded circular scan | First available | N/A |

### 5.2 Determinism Invariants

1. **Same seed -> Same execution order:** All RNG-dependent decisions use `DetRng` seeded from worker ID
2. **Same inputs -> Same outputs:** No non-deterministic data structures in hot paths
3. **Generation monotonicity:** All FIFO ordering backed by monotone u64 counters
4. **Wrap safety:** u64 wrapping for IDs/generations is safe due to HashMap-based tracking
5. **Witness reproducibility:** `FairnessPreemptionCertificate` hash is deterministic over dispatch trace

### 5.3 C Implementation Requirements

For deterministic parity with Rust:

1. Channel waiter queue MUST use monotonic ID assignment with FIFO pop
2. Timer wheel MUST use insertion-ordered arrays per slot (not hash maps)
3. Scheduler MUST implement generation-based FIFO within priority levels
4. RNG MUST be seeded identically per worker ID for tie-breaking
5. Work stealing MUST use same circular scan with deterministic start index
6. Fairness certificate MUST produce identical witness hash for identical dispatch traces

---

## 6. Failure Paths and Invalid-Handle Behavior

### 6.1 Channel Failure Paths

| Failure | Detection | Recovery | Deterministic |
|---------|-----------|----------|---------------|
| Reserve on closed channel | `receiver_dropped` check | Returns `Disconnected` | Yes |
| Send on closed channel | `receiver_dropped` check in `try_send` | Returns `Disconnected(value)` | Yes |
| Recv on empty + all senders dropped | `sender_count == 0` AND `queue.empty()` | Returns `Disconnected` | Yes |
| Reserve cancelled | `cx.checkpoint()` | Returns `Cancelled`, waiter cleaned | Yes |
| Recv cancelled | `cx.checkpoint()` | Returns `Cancelled`, message preserved | Yes |
| Permit leaked (session layer) | `ObligationToken::Drop` in Reserved state | **PANIC** | Yes |
| Zero capacity channel | `assert!(capacity > 0)` | **PANIC** at creation | Yes |

### 6.2 Timer Failure Paths

| Failure | Detection | Recovery | Deterministic |
|---------|-----------|----------|---------------|
| Duration exceeded | `delta > max_timer_duration` | Returns `TimerDurationExceeded` error | Yes |
| Cancel with stale handle | Generation mismatch in `active` map | Returns `false` | Yes |
| Cancel already-cancelled timer | ID not in `active` map | Returns `false` | Yes |
| Fire cancelled timer | `is_live()` returns false | Silently skipped | Yes |
| ID wrap collision | HashMap stores both entries | Both independently tracked | Yes |

### 6.3 Scheduler Failure Paths

| Failure | Detection | Recovery | Deterministic |
|---------|-----------|----------|---------------|
| Cancel-streak limit hit | `cancel_streak >= effective_limit` | Skip cancel lane, dispatch from timed/ready | Yes |
| No tasks available | All lanes empty | Backoff -> spin -> yield -> park | Yes |
| Work steal failure | All stealers empty | Continue to backoff | Yes |
| Shutdown requested | `shutdown.load(Relaxed)` | Exit run_loop | Yes |

---

## 7. Fixture Family Mapping

### 7.1 Channel Fixtures

| Fixture ID | Description | Tests |
|------------|-------------|-------|
| `ch-reserve-send-001` | Basic reserve -> send -> recv cycle | Happy path |
| `ch-reserve-abort-001` | Reserve -> abort -> capacity freed | Abort cascade |
| `ch-backpressure-001` | Full channel blocks sender, recv unblocks | Backpressure |
| `ch-fifo-001` | Multiple senders, FIFO delivery order | Ordering |
| `ch-fifo-fairness-001` | try_reserve respects waiter queue FIFO | Fairness |
| `ch-cancel-reserve-001` | Cancel during pending reserve | Cancel safety |
| `ch-cancel-recv-001` | Cancel during pending recv, message preserved | Cancel safety |
| `ch-evict-001` | send_evict_oldest evicts front, returns evicted | Eviction |
| `ch-evict-reserved-001` | send_evict_oldest fails when all capacity reserved | Eviction boundary |
| `ch-close-drain-001` | Sender drop -> receiver drains remaining | Close/drain |
| `ch-close-wake-001` | Receiver drop -> all senders woken with Disconnected | Close/wake |
| `ch-obligation-leak-001` | TrackedPermit drop without send/abort -> panic | Obligation |
| `ch-obligation-commit-001` | TrackedPermit send -> CommittedProof | Obligation |
| `ch-obligation-abort-001` | TrackedPermit abort -> AbortedProof | Obligation |
| `ch-cascade-001` | Permit abort cascades wake to next waiter | Cascade |

### 7.2 Timer Fixtures

| Fixture ID | Description | Tests |
|------------|-------------|-------|
| `tm-insert-fire-001` | Basic insert -> advance -> fire | Happy path |
| `tm-same-deadline-001` | Multiple timers, same deadline, insertion order | Ordering |
| `tm-cancel-001` | Insert -> cancel -> advance -> not fired | Cancel |
| `tm-cancel-stale-001` | Cancel with stale generation -> rejected | Invalid handle |
| `tm-cancel-reuse-001` | Same ID, different generation -> independent | Generation safety |
| `tm-duration-exceeded-001` | Insert beyond max duration -> error | Validation |
| `tm-cascade-001` | Level 0 wrap triggers Level 1 cascade | Cascade |
| `tm-overflow-001` | Far-future timer in overflow heap | Overflow |
| `tm-coalesce-001` | Timers within coalescing window fire together | Coalescing |
| `tm-wrap-001` | ID and generation u64 wrap without collision | Wrap safety |
| `tm-purge-001` | All timers cancelled -> storage purged | Cleanup |
| `tm-immediate-001` | Deadline in past -> immediately ready | Edge case |

### 7.3 Scheduler Fixtures

| Fixture ID | Description | Tests |
|------------|-------------|-------|
| `sc-lane-priority-001` | Cancel dispatched before timed before ready | Lane ordering |
| `sc-fifo-priority-001` | Equal priority tasks dispatched in insertion order | FIFO |
| `sc-edf-001` | Timed tasks dispatched earliest-deadline-first | EDF |
| `sc-edf-fifo-001` | Equal deadline timed tasks dispatched FIFO | EDF + FIFO |
| `sc-rng-tiebreak-001` | RNG tie-breaking deterministic with seed | Determinism |
| `sc-steal-001` | Work stealing follows RNG-seeded circular order | Stealing |
| `sc-cancel-streak-001` | Cancel streak limit triggers fairness yield | Fairness |
| `sc-governor-meet-001` | MeetDeadlines suggestion reorders timed > cancel | Governor |
| `sc-governor-drain-001` | DrainObligations doubles cancel-streak limit | Governor |
| `sc-certificate-001` | Identical traces produce identical witness hash | Replay |
| `sc-timer-phase0-001` | Expired timers processed before task dispatch | Phase ordering |

---

## 8. Invariant Schema Cross-Reference

### 8.1 Channel Invariants

| ID | Invariant | Category |
|----|-----------|----------|
| `INV-CH-01` | `used_slots = queue.len() + reserved <= capacity` at all times | Capacity |
| `INV-CH-02` | Messages delivered in FIFO order (VecDeque) | Ordering |
| `INV-CH-03` | Waiters serviced in FIFO order (monotonic ID) | Fairness |
| `INV-CH-04` | `try_reserve` returns `Full` when waiter queue non-empty, regardless of capacity | Fairness |
| `INV-CH-05` | Cancelled reserve does not consume capacity | Cancel safety |
| `INV-CH-06` | Cancelled recv does not consume message | Cancel safety |
| `INV-CH-07` | `receiver_dropped` monotone: `false -> true`, never reverses | State monotonicity |
| `INV-CH-08` | `sender_count` monotone: only decrements, never increments after creation | State monotonicity |
| `INV-CH-09` | Permit Drop without send/abort decrements `reserved` and cascades wake | RAII safety |
| `INV-CH-10` | Session-tracked permit leaked -> panic (obligation linearity) | Obligation |

### 8.2 Timer Invariants

| ID | Invariant | Category |
|----|-----------|----------|
| `INV-TM-01` | Cancel requires exact `(id, generation)` match | Handle safety |
| `INV-TM-02` | Cancelled timers silently skipped at fire time via `is_live()` | Lazy deletion |
| `INV-TM-03` | Same-deadline timers fire in insertion order | Ordering |
| `INV-TM-04` | `try_register` rejects duration > `max_timer_duration` | Validation |
| `INV-TM-05` | All-cancelled triggers `purge_inactive_storage()` | Cleanup |
| `INV-TM-06` | u64 ID/generation wrap is collision-safe (HashMap-based) | Wrap safety |
| `INV-TM-07` | Cascade respects generation: dead entries dropped during level promotion | Cascade safety |
| `INV-TM-08` | Fire criterion: `entry.deadline <= now` (inclusive) | Fire semantics |

### 8.3 Scheduler Invariants

| ID | Invariant | Category |
|----|-----------|----------|
| `INV-SC-01` | Cancel lane dispatched before timed before ready (default) | Lane priority |
| `INV-SC-02` | Equal-priority entries dispatched in generation (FIFO) order | Ordering |
| `INV-SC-03` | Equal-deadline timed entries dispatched in generation (FIFO) order | EDF ordering |
| `INV-SC-04` | Cancel-streak limit prevents cancellation starvation | Fairness |
| `INV-SC-05` | Same seed -> same work-steal scan order | Determinism |
| `INV-SC-06` | Identical dispatch traces produce identical fairness certificate witness hash | Replay |
| `INV-SC-07` | Phase 0 (timers) executes before task dispatch | Phase ordering |
| `INV-SC-08` | Governor suggestion affects lane order but not correctness | Safety |

### 8.4 Coverage Matrix

| Domain | States | Operations | Errors | Invariants | Fixture IDs |
|--------|--------|------------|--------|------------|-------------|
| Channel | 4 implicit | 8 (reserve, send, try_send, abort, recv, try_recv, send_evict, close) | 6 variants | 10 | 15 |
| Timer | N/A (stateless entries) | 4 (register, fire, cancel, purge) | 1 type | 8 | 12 |
| Scheduler | 3 lanes x N entries | 6 (inject_cancel, inject_timed, inject_ready, dispatch, steal, park) | 0 | 8 | 11 |
| **Total** | — | **18** | **7** | **26** | **38** |

Combined with bd-296.15 (55 fixture IDs) and bd-296.16 (outcome/budget fixtures), total fixture surface: **93+ fixture ID candidates**.
