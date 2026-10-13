# Channel, Timer, and Scheduler Semantics — Canonical Reference

> **Bead:** bd-296.17
> **Scope:** Deterministic channel/timer kernel semantics and tie-break ordering contract
> **Status:** Reference for what the Rust code does, with a **C status** line in each section saying what the C port implements and where. "spec (Rust) — not implemented in C" means no C code implements that behaviour.
> **Rust baseline commit:** `5e60b1c4c` (asupersync). Every Rust `file:line` below was re-read at this commit with `git -C /dp/asupersync show 5e60b1c4c:<path>`.
> **Last verified:** 2026-10-10 (bd-9kll.1.8)
> **C anchor refresh:** Channel, permit settlement, scheduler, lifecycle, parallel and lab-dispatch anchors below were checked against the merged 2026-10-11 working tree. This refresh supplies source references, not additional Rust execution evidence.
> **Purpose:** Semantic contracts for the MPSC channel, the hierarchical timer wheel and the multi-worker scheduler, and the C port's status for each.

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

Primary Rust sources (line counts from `git -C /dp/asupersync show 5e60b1c4c:<path> | wc -l`):

| Source File | Domain | Lines |
|-------------|--------|-------|
| `/dp/asupersync/src/channel/mpsc.rs` | MPSC channel core | 2279 |
| `/dp/asupersync/src/channel/session.rs` | Obligation-tracked channel wrappers | 2382 |
| `/dp/asupersync/src/obligation/graded.rs` | `ObligationToken` and its leak panic | 2481 |
| `/dp/asupersync/src/time/wheel.rs` | Hierarchical timer wheel | 3509 |
| `/dp/asupersync/src/time/driver.rs` | Timer driver (registrations over the wheel) | 3063 |
| `/dp/asupersync/src/time/deadline.rs` | Deadline propagation utilities | 554 |
| `/dp/asupersync/src/time/sleep.rs` | `Sleep` future | 3050 |
| `/dp/asupersync/src/runtime/timer.rs` | Runtime timer heap | 903 |
| `/dp/asupersync/src/runtime/scheduler/three_lane.rs` | Multi-worker scheduler loop | 10031 |
| `/dp/asupersync/src/runtime/scheduler/priority.rs` | Per-worker lane heaps (`Scheduler`, re-exported as `PriorityScheduler`) | 3970 |
| `/dp/asupersync/src/runtime/scheduler/global_injector.rs` | Global task injection | 1341 |
| `/dp/asupersync/src/runtime/scheduler/global_queue.rs` | `GlobalFifoQueue` | 1134 |
| `/dp/asupersync/src/runtime/scheduler/stealing.rs` | Work stealing | 434 |
| `/dp/asupersync/src/runtime/scheduler/intrusive.rs` | Intrusive ring and stack | 1837 |
| `/dp/asupersync/src/lab/runtime.rs` | `LabRuntime` (the dispatch model the C lab ports) | 12988 |

Cross-check context:
- `docs/LIFECYCLE_TRANSITION_TABLES.md` (bd-296.15)
- `docs/EXISTING_ASUPERSYNC_STRUCTURE.md` (bd-296.16)

### How C parity is checked

- Rust parity evidence is the set of Rust-captured fixtures in `fixtures/rust_reference_v2`, which `make conformance` runs through the C runtime (`build/bin/asx-conformance`, `tools/conformance/runner.c`) and compares trace, snapshot, observations and dispatch order with the capture. Fixtures named by the original extraction passed in CI run 38049908914 (commit `ac050e9`, 61 of 61) and again in run 38051289151 (commit `cd69958`, 62 of 62). Later fixtures, including the counted-sender capture, are described by their scenario inputs here; this merge review supplies no new executed comparison. A fixture is named only after its scenario in `tests/conformance/scenarios_v2/` was read and found to exercise the behaviour.
- C parity runs use lab dispatch (`src/runtime/lab_dispatch.c`), a port of the Rust `LabRuntime` single-worker dispatch, not of the multi-worker `three_lane.rs` scheduler described in section 3.

---

## 1. MPSC Channel Semantics

### 1.1 Channel State Model

The Rust channel uses **implicit state encoding** via atomic fields (no explicit state enum):

| Implicit State | Condition | Description |
|----------------|-----------|-------------|
| **Open** | `receiver_dropped == false` AND `sender_count > 0` | Both sides active |
| **Half-Closed (Rx)** | `receiver_dropped == true` AND `sender_count > 0` | Receiver dropped or closed; senders observe disconnect |
| **Half-Closed (Tx)** | `receiver_dropped == false` AND `sender_count == 0` | All senders dropped; receiver drains queue |
| **Closed** | `receiver_dropped == true` AND `sender_count == 0` | Both sides gone |

**Rust source:** `mpsc.rs:297-311` (`ChannelShared`)

**Monotonicity:** `receiver_dropped` goes `false -> true` once (comment at `mpsc.rs:304-305`; set by `Receiver::close`, `mpsc.rs:1698-1711`, and `Drop for Receiver`, `mpsc.rs:2122-2149`). `sender_count` is not decrement-only: `Sender::clone` increments it (`mpsc.rs:1231-1239`) and `WeakSender::upgrade` increments it but refuses to raise it from 0 (`mpsc.rs:1269-1300`). Once it reaches 0 it stays 0.

**C status:** implemented with an explicit state enum, `ASX_CHANNEL_OPEN / SENDER_CLOSED / RECEIVER_CLOSED / FULLY_CLOSED` (`include/asx/core/channel.h:69-74`). The channel starts with one sender, the creator's (`channel_create_impl`, `src/channel/mpsc.c:500`); `asx_channel_sender_clone` adds one, with an identity of its own, while the sender side is live and refuses once the count reached 0 (`src/channel/mpsc.c:551-590`). Keeping the bare channel id without counting it represents a weak sender; upgrading uses the same clone operation and yields a sender handle, including after the receiver closes (`include/asx/core/channel.h`). A clone is dropped with `asx_channel_sender_drop` and the creator's sender with `asx_channel_close_sender` (`src/channel/mpsc.c:625-662`), each at most once: a second drop is refused with `ASX_E_INVALID_STATE` and the count unchanged, so one producer's double drop cannot remove another's sender, as Rust's owned `Sender` cannot be dropped twice (bd-9kll.5.12). Only the last drop moves `OPEN -> SENDER_CLOSED` or `RECEIVER_CLOSED -> FULLY_CLOSED` (`channel_sender_dropped`, `src/channel/mpsc.c:595-620`), and a drop after the sender side has closed returns `ASX_E_INVALID_STATE`. A C handle is not a move-only value: a copy of a dropped handle drops nothing (its token is no longer live). The scenario `mpsc-counted-senders-close-at-last-drop-001` records a first producer's drop, a second producer's later successful send, and disconnect only after the last sender drops.

### 1.2 Capacity Model

Capacity is **fixed at creation** and never changes:

```
channel(capacity) where capacity > 0  // panics on zero
used_slots = queue.len() + reserved
INVARIANT: used_slots <= capacity
```

Both queued messages and reserved (uncommitted) permits consume capacity.

**Rust source:** `mpsc.rs:564-579` (`channel`, `assert!(capacity > 0)` at `:565`), `mpsc.rs:351-359` (`used_slots`, `has_capacity`), `mpsc.rs:307-309` (write-once `capacity`). `unbounded_channel` is `channel(usize::MAX)` (`mpsc.rs:599-600`).

**C status:** implemented. Capacity 0 or above `ASX_CHANNEL_MAX_CAPACITY` (default 64, `include/asx/asx_config.h:362-363`) returns `ASX_E_INVALID_ARGUMENT` instead of panicking (`src/channel/mpsc.c:430`). A reserve is refused when `queue_len + reserved >= capacity` (`channel_reserve_impl`; lock-free backend `channel_lf_claim_capacity`). `asx_channel_create_unbounded` is `unbounded_channel` (bd-9kll.5.7, DEC-008): the channel's arena slice, `ASX_CHANNEL_MAX_CAPACITY` messages, stands for `usize::MAX`; a reserve or send past it is `ASX_E_RESOURCE_EXHAUSTED` with nothing changed and never parks; creation is `ASX_E_PERMISSION_DENIED` where the profile's overload catalog entry declares `ASX_FORBID_UNBOUNDED_QUEUE`. Unit tests: `tests/unit/channel/test_mpsc.c` `unbounded_takes_its_arena_then_exhausts_without_changing_anything`, `unbounded_sender_in_a_poll_never_parks`, `unbounded_creation_follows_the_profile_catalog`. Bounded unit tests: `tests/unit/channel/test_mpsc.c` `capacity_enforcement`, `capacity_mixed_reserved_and_queued`, `create_zero_capacity`.

### 1.3 Two-Phase Send Protocol (Reserve/Send/Abort)

#### Phase 1: Reserve

```
Sender::reserve(cx) -> Reserve future -> Result<SendPermit, SendError<()>>
```

**Checked on each poll, in order** (`Reserve::poll_inner`, `mpsc.rs:1040-1172`):
1. Cancellation checkpoint: `cx.checkpoint().is_err()` -> `SendError::Cancelled(())`, waiter removed (`:1061-1068`)
2. Receiver gone: `receiver_dropped == true` -> `SendError::Disconnected(())` (`:1072-1079`)
3. Caller is the head of the waiter queue (or the queue is empty and the caller is not queued) and `used_slots < capacity` -> `reserved += 1`, leave the queue, wake the next waiter if capacity remains (`:1081-1101`), then register a `SendPermit` runtime obligation through `cx` (`:1116-1119`, `:1175-1181`)
4. Otherwise: register or refresh the waker in the FIFO waiter queue and return `Poll::Pending` (`:1146-1170`)

A pending reserve re-checks cancellation after registering (`mpsc.rs:1020-1031`).

**C status:** implemented. `asx_channel_reserve` (`src/channel/mpsc.c:1267-1291`) checks cancellation first (`channel_wait_cancelled`, `mpsc.c:1258-1265`), then `channel_reserve_impl` (`mpsc.c:789-862`): sender closed -> `ASX_E_INVALID_STATE` (`:798-801`), receiver closed -> `ASX_E_DISCONNECTED` (`:803-806`), an unfinished producer queued ahead -> full (`:811-813`), no capacity -> full (`:815-821`). Full parks the task in the reserve wait queue and returns `ASX_E_PENDING` (`:777-786`, `:1274`). Success registers a `SendPermit` obligation when the `Cx` has a task (`:1282-1289`). Rust parity: `mpsc-two-phase-send-recv-001`.

#### Phase 2a: Send (Commit)

```
SendPermit::send(self, value: T) -> Outcome<(), SendError<T>>
SendPermit::try_send(self, value: T) -> Result<(), SendError<T>>
```

Neither is infallible: both run `try_send_deferred_wake` (`mpsc.rs:1588-1629`), which
- marks the permit consumed and decrements `reserved`,
- if the receiver has gone, returns `Err(SendError::Disconnected(value))` and aborts the permit's obligation with reason `Error` (`:1602-1614`),
- otherwise pushes the value to the back of the queue, takes the receiver waker and commits the obligation (`:1616-1628`).

**Rust source:** `mpsc.rs:1565-1570` (`send`), `mpsc.rs:1574-1578` (`try_send`)

**C status:** implemented. `asx_send_permit_send` (`src/channel/mpsc.c:891-952`) authenticates the tracked credit's channel/token binding before consuming the permit (`:900-917`), returns `ASX_E_DISCONNECTED` and aborts the obligation with `ASX_OBLIGATION_ABORT_ERROR` when the receiver is closed (`:921-927`), else enqueues (`:929-946`), commits the obligation (`:948`) and settles waiters (`:949`). C has no `Sender::try_send`; the conformance interpreter runs `try_send` as `asx_channel_try_reserve` plus `asx_send_permit_send` (`src/conformance/interpreter.c:3681-3695`).

#### Phase 2b: Abort (Rollback)

```
SendPermit::abort(self)
```

- Marks the permit consumed, decrements `reserved`, records a cancellation event and wakes the head of the waiter queue (`release_capacity`, `mpsc.rs:1641-1653`)
- Aborts the obligation with reason `Explicit` (`mpsc.rs:1633-1639`)

**C status:** implemented. `asx_send_permit_abort` (`src/channel/mpsc.c:989-991`) invokes the binding-checked abort path (`:958-987`), returns the slot, aborts the obligation with `ASX_OBLIGATION_ABORT_EXPLICIT` and settles the wait queues (`:979-985`).

#### RAII Drop Safety

If a `SendPermit` is dropped unsent, `Drop` releases the slot (with the same head-of-queue wake) and aborts the obligation with reason `Cancel`:

```
Drop for SendPermit: if !self.sent { release_capacity(); obligation.abort(Cancel) }
```

**Rust source:** `mpsc.rs:1663-1673`

**C status:** implemented for tracked permits at holder-task completion. Reserve attaches the physical permit's drop to its obligation (`src/channel/mpsc.c:1049-1056`); completion invokes `obligation_drop_permit` (`src/runtime/lifecycle.c:559-569`), which returns the channel slot through `channel_permit_drop` (`src/channel/mpsc.c:866-879`) and aborts the obligation with `Cancel`. C has no lexical destructors or permit cleanup-stack registration: an untracked permit has no holder and stays reserved until it is sent or aborted. The conformance interpreter also explicitly drops its body's permits with `Cancel` (`drop_send_permit`, `src/conformance/interpreter.c:1150`). This source evidence adds no Rust drop fixture coverage.

### 1.4 Backpressure and Waiter Queue

When the channel is full (`used_slots >= capacity`):

1. `reserve()` returns `Poll::Pending`
2. The sender's waker is stored in a `TokenSlab` and its token is appended to a `VecDeque<SlabToken>` FIFO (`mpsc.rs:279-282`); there is no separate monotonic waiter ID
3. A waiter keeps its queue position across re-polls

**Backpressure release triggers:**
- Receiver takes a message: `poll_recv`/`try_recv` wake the head waiter (`mpsc.rs:1801-1810`, `mpsc.rs:1956-1983`)
- Permit aborted or dropped: `release_capacity` wakes the head waiter (`mpsc.rs:1641-1653`)
- A queued waiter leaves (cancelled or dropped `Reserve`): `cleanup_waiter` passes the wake to the next head if it held a queue position and capacity is free (`mpsc.rs:960-998`)

Only the head is woken; when the head takes a slot and capacity remains it wakes the next head (`mpsc.rs:1099-1101`).

**FIFO fairness enforcement:** `try_reserve()` returns `Full` while a live waiter is queued, even if capacity is available (`has_waiting_sender`, `mpsc.rs:732-754`). `try_send` (`mpsc.rs:773-794`) and `send_evict_oldest_where` (`mpsc.rs:889-936`) apply the same rule.

**C status:** implemented. Parked producers wait in `reserve_waiters`, a wait queue that is FIFO by arrival (`src/sync/wait_queue.h:23`). After every state change `channel_settle` hands free capacity to the head of the line (`src/channel/mpsc.c:426-433`); a reserve reports full while an unfinished producer is queued ahead of it (`mpsc.c:811-813`), including a cancel-pending producer until its next poll or completion withdraws it (`src/sync/wait_queue.h:32-48`). A batch receive that frees several slots wakes one parked producer per freed slot, as Rust's `sender_wakers_for_freed_slots` (`mpsc.rs:402-423`) does (`asx_channel_recv_many`, `mpsc.c:1104-1114`). Unit tests: `tests/unit/channel/test_channel_wake.c` `try_reserve_never_jumps_parked_producer`, `reserve_waiters_served_in_arrival_order_deterministically`, `full_channel_parks_producers_and_dequeue_wakes_one`. Rust parity: `mpsc-two-phase-send-recv-001` checks one producer waiting for the consumer; `mpsc-recv-many-wakes-each-freed-slot-001` parks two producers on one channel and checks their FIFO order and wake dispatches against Rust.

### 1.5 Eviction Mode

```
send_evict_oldest(value: T) -> Result<Option<T>, SendError<T>>
```

`send_evict_oldest` (`mpsc.rs:872-874`) is `send_evict_oldest_where(value, |_| true)` (`mpsc.rs:889-936`):

- Receiver dropped: `Err(Disconnected(value))`
- Free capacity and no queued waiter: normal send, `Ok(None)`
- Free capacity but a waiter is queued (the free slot is the waiter's): `Err(Full(value))`, nothing evicted
- No free capacity: removes the oldest queued message accepted by the predicate, pushes the new one, `Ok(Some(evicted))`
- No free capacity and no evictable queued message (all capacity reserved, or the predicate protects every message): `Err(Full(value))`

**C status:** implemented: `asx_channel_send_evict_oldest_where` (`src/channel/mpsc.c`) takes the predicate (`asx_channel_evict_pred_fn`), and `asx_channel_send_evict_oldest` is its always-accept case, as in Rust. Both have the same outcomes as Rust, reporting `ASX_E_DISCONNECTED`, or `ASX_E_CHANNEL_FULL` when a parked producer owns the free slot, every slot is reserved, or a full channel holds no accepted message, and also `ASX_E_INVALID_STATE` once the sender side is closed (C-only: a Rust `Sender` cannot call it after its drop). The evicted message is the oldest accepted one wherever it sits; the messages behind it keep their order. The lock-free backend does it exactly as the ring backend does: only the consumer touches the cells from `dequeue_pos` up to the first uncommitted one (a producer works on the cell at `enqueue_pos` alone), so the committed messages ahead of the removed one move one cell toward the tail, keeping every sequence number valid, and the head cell is freed as a dequeue frees it. It must be called on the receiver's (scheduler) thread. A GenServer's `try_cast` under `DropOldest` uses it with a predicate that accepts casts only, so a queued call is never evicted (`gen_server.rs:1321`). Unit tests: `test_mpsc.c` `evict_oldest_*` and `evict_where_*`, built in a default and a `PROFILE=POSIX ASX_LOCKFREE_SINGLE_THREAD=0` build. Rust parity: `mpsc-send-evict-oldest-001` (always-accept case).

### 1.6 Receive Semantics

```
recv(cx) -> Recv future -> Result<T, RecvError>
try_recv() -> Result<T, RecvError>
```

**Receive order** (`poll_recv`, `mpsc.rs:1778-1859`):
1. Cancellation checkpoint: `cx.checkpoint().is_err()` -> `RecvError::Cancelled` (`:1786-1797`)
2. Queue non-empty: `queue.pop_front()` -> `Ok(value)`, head sender woken (`:1801-1810`)
3. Queue empty and (`sender_count == 0` or receiver closed): `RecvError::Disconnected` (`:1813-1821`)
4. Otherwise: register the waker, return `Poll::Pending`

`try_recv` (`mpsc.rs:1956-1983`) returns `Empty` instead of waiting.

**Cancel safety:** the checkpoint runs before the pop, so a cancelled receive does not consume a message.

**C status:** implemented. `asx_channel_recv` (`src/channel/mpsc.c:1305-1313`) checks cancellation first, then `channel_recv_impl` (`mpsc.c:1197-1224`): dequeue in FIFO order (`:1206-1210`), receiver closed, or sender closed with no outstanding permit -> `ASX_E_DISCONNECTED` (`:1212-1215`, `channel_drained_for_good`, `:1189-1192`), else park and return `ASX_E_PENDING` (`:1217-1223`, `:1312`). `asx_channel_try_recv` (`mpsc.c:1226-1228`) returns `ASX_E_WOULD_BLOCK` when empty; the conformance interpreter reports that as `ASX_E_CHANNEL_EMPTY` (`src/conformance/interpreter.c:3723`). Rust parity: `mpsc-recv-cancel-first-001` (a pending recv cancelled by the driver returns `ASX_E_CANCELLED`), `mpsc-try-ops-001` (`try_recv` on an empty channel, then `ASX_E_DISCONNECTED` after the sender closes and the queue drains).

**Batch receive** (`recv_many`, `mpsc.rs:1754`; `poll_recv_many`, `:1867-1952`): a `limit` of 0 is `Ok(0)` before anything else (`:1874-1876`); then the cancellation checkpoint (trace `mpsc::recv_many cancelled`, `:1881-1892`); then up to `limit` queued messages are drained oldest first, waking one queued producer per freed slot (`:1894-1907`); an empty queue with no sender left or the receiver closed is `Ok(0)`, not `Disconnected` (`:1909-1917`); otherwise the receiver registers and waits. **C status:** implemented, `asx_channel_recv_many` (`src/channel/mpsc.c:1317-1358`). Unit tests: `test_mpsc.c` `recv_many_*`. Rust parity: `mpsc-recv-many-001`, `mpsc-recv-many-wakes-each-freed-slot-001`.

**Closed wait** (`Sender::closed`, `mpsc_closed.rs:39-48`): resolves once the receiver closed or dropped, without reserving capacity or touching the queue; no `Cx`, so cancellation does not end it. **C status:** implemented, `asx_channel_closed` (`src/channel/mpsc.c:1360-1378`), woken by `asx_channel_seal` and `asx_channel_close_receiver`; a task that stops waiting calls `asx_channel_wait_cancel`. Unit tests: `test_mpsc.c` `closed_*`. Rust parity: `mpsc-closed-001`.

### 1.7 Error Taxonomy

#### Send Errors (`mpsc.rs:111-118`)

| Error | Condition | Carries Value |
|-------|-----------|---------------|
| `SendError::Disconnected(T)` | Receiver dropped or closed | Yes |
| `SendError::Cancelled(T)` | Cancellation checkpoint triggered | Yes |
| `SendError::Full(T)` | No slot available to a non-waiting call (`try_reserve`, `try_send`, `send_evict_oldest*`) | Yes |

`reserve_checked`/`try_reserve_checked`/`send_checked` return `CheckedSendError<T>` (`mpsc.rs:136-146`): `Channel(SendError<T>)` or `Admission { error, value }` when the runtime refuses the permit's obligation.

#### Receive Errors (`mpsc.rs:182-189`)

| Error | Condition |
|-------|-----------|
| `RecvError::Disconnected` | Queue empty and (all senders dropped or receiver closed) |
| `RecvError::Cancelled` | Cancellation checkpoint triggered |
| `RecvError::Empty` | Queue empty (`try_recv` only), senders still alive |

**C status:** mapped to status codes: `ASX_E_DISCONNECTED` (`include/asx/asx_status.h:64`), `ASX_E_CANCELLED` (`asx_status.h:56`), `ASX_E_CHANNEL_FULL` (`asx_status.h:66`, from `asx_channel_try_reserve`), `ASX_E_PENDING` (waiting reserve/send/recv), `ASX_E_WOULD_BLOCK` (`asx_channel_try_recv` on an empty channel; `ASX_E_CHANNEL_EMPTY`, `asx_status.h:69`, is not returned by `mpsc.c`). C does not return values with errors (messages are `uint64_t` passed by the caller). There is no `Admission` error from permit registration: a refused obligation leaves the permit untracked (`src/channel/mpsc.c:1049-1056`). A tracked permit carrying a credit bound to a different channel/token is refused before capacity or ledger mutation (`src/channel/mpsc.c:759-763`, `:822-826`).

### 1.8 Cancellation Interaction

**Checkpoint-based cancellation** at operation boundaries:

| Operation | Cancel Effect |
|-----------|--------------|
| `reserve()` poll | Returns `Cancelled`, no capacity consumed, waiter removed from queue |
| `recv()` poll | Returns `Cancelled`, no message consumed, receiver waker cleared |
| `SendPermit::send()` / `try_send()` | Not cancellation-checked (no checkpoint in `mpsc.rs:1588-1629`) |
| `SendPermit::abort()` | Not cancellation-checked (`mpsc.rs:1633-1653`) |

**Waiter queue cleanup on cancel:** a cancelled or dropped `Reserve` runs `cleanup_waiter` (`mpsc.rs:960-998`; `Drop for Reserve`, `mpsc.rs:1225-1229`), which removes its token and wakes the next head if it held a queue position and capacity is free.

**Rust source:** `mpsc.rs:1061-1068` (reserve cancel), `mpsc.rs:1786-1797` (recv cancel)

**C status:** implemented. `channel_wait_cancelled` (`src/channel/mpsc.c:1258-1265`) checks `asx_cx_checkpoint`, records the trace message `mpsc::reserve cancelled`, `mpsc::recv cancelled` or `mpsc::recv_many cancelled`, and withdraws the task from all three wait queues, passing any wake it held on (`asx_channel_wait_cancel`, `mpsc.c:1234-1248`). Cancellation observation requires no capability flag: the checkpoint observes the identified task's pending cancel before budget consumption (`src/cx/cx.c:416-435`); masking defers delivery but does not hide a recorded request from `asx_cx_is_cancel_requested` (`src/cx/cx.c:402-413`). Permit send and abort authenticate their physical binding without a cancellation checkpoint (`mpsc.c:891-987`). Unit tests: `test_channel_wake.c` `cancelled_waiter_does_not_absorb_wake`, `test_mpsc.c` `cancelled_cx_wins_over_ready_channel_and_traces`. Rust parity: `mpsc-recv-cancel-first-001`. Reserve cancellation has no v2 fixture.

`asx_task_masked` (`src/runtime/cancellation.c:761`) adds a synchronous callback
guard for a task's cancellation mask. It releases its level when the callback
returns, including a channel poll returning `ASX_E_PENDING` or an error, and
returns the body's status unless release fails; it does
not retain the mask across the wait. An outer raw mask still requires explicit
unmasking. Request visibility remains separate from checkpoint delivery, and
nesting after a request or ACK is allowed as in Rust (#12 Variant).
Callbacks must return and balance extra raw masks; no `longjmp` or unwind
cleanup guarantee follows. See `docs/LIFECYCLE_TRANSITION_TABLES.md` §2.5 for
the guarded-return tests and raw-mask-leak liveness boundary.

### 1.9 Obligation Integration

The `session` module wraps the base channel with an `ObligationToken` per permit:

| Component | Base Channel | Session-Tracked |
|-----------|-------------|-----------------|
| Sender type | `Sender<T>` | `TrackedSender<T>` (`session.rs:220-222`) |
| Permit type | `SendPermit<T>` (carries an optional runtime obligation, `mpsc.rs:1461-1472`) | `TrackedPermit<T>` = `SendPermit` + `ObligationToken<SendPermit>` (`session.rs:404-408`) |
| Send result | `Outcome<(), SendError<T>>` | `Result<CommittedProof<SendPermit>, SendError<T>>` (`session.rs:416-426`) |
| Abort result | `()` | `AbortedProof<SendPermit>` (`session.rs:441-445`) |
| Leak behavior | Slot released; a registered runtime obligation is aborted with reason `Cancel` (`mpsc.rs:1663-1673`) | **PANIC**: `[ASUP-E101] OBLIGATION TOKEN LEAKED: ...` (`obligation/graded.rs:991-1006`); if the thread is already panicking the leak is recorded instead (`graded.rs:993-997`) |

`#[must_use]` is on both `SendPermit` (`mpsc.rs:1460`) and `TrackedPermit` (`session.rs:404`). A base permit from `reserve`/`reserve_checked` carries a runtime obligation; one from `try_reserve` does not (doc comment `mpsc.rs:1467-1470`).

**C status:** `TrackedSender`/`TrackedPermit` and the leak panic: spec (Rust) — not implemented in C (`src/channel/session.c` is a different, bidirectional session channel; `include/asx/core/session.h:1-14`). The base-permit obligation is implemented: `asx_send_permit` has an `obligation` field (`include/asx/core/channel.h:69-83`), registered by `asx_channel_reserve` (`src/channel/mpsc.c:1267-1291`), committed on send (`:948`), aborted `Explicit` on abort (`:989-991`) and `Error` on disconnect (`:925`). Rust parity: `mpsc-two-phase-send-recv-001` (its trace records `obligation.reserved` and `obligation.committed` of kind `SendPermit` for both sends).

Typed permit settlement authenticates the channel/token binding through `asx_obligation_check_token_internal` (`src/runtime/lifecycle.c:1983-2003`) and resolves through `asx_obligation_resolve_token_internal` (`src/runtime/lifecycle.c:2005-2024`), as called by `channel_permit_resolve` (`src/channel/mpsc.c:877-889`). A genuine permit may move to another task while retaining its original holder's ledger liability; a plain move does not transfer that holder or region. Raw owned-obligation IDs retain holder-poll authority checks (`asx_obligation_check_access_internal`, `src/runtime/lifecycle.c:1930-1966`); knowing an ID alone does not authenticate a physical permit. This is C source evidence, not Rust type-level or new handoff fixture coverage.

### 1.10 Close/Drain Semantics

#### Receiver Drop (Channel Close)

`Drop for Receiver` (`mpsc.rs:2122-2149`):
1. Drains all sender waiters from the queue
2. Sets `receiver_dropped = true`
3. Takes the receiver waker
4. Takes the queued messages with `mem::take` — they are **dropped**, not delivered — and drops them outside the lock
5. Wakes every drained sender (they observe `Disconnected` on their next poll)

`Receiver::close()` (`mpsc.rs:1698-1711`) differs: it sets `receiver_dropped` and wakes the senders but leaves queued messages receivable; `poll_recv` pops them before reporting `Disconnected` (`mpsc.rs:1801-1821`).

**C status:** both are implemented. The drop: `asx_channel_close_receiver` (`src/channel/mpsc.c:664-691`) discards queued messages and wakes every waiter, also after a seal, as Rust's drop takes the queue after `close()` (`mpsc.rs:2138`; unit test `test_mpsc.c` `receiver_drop_after_seal_discards_the_queue_and_frees_the_slot`). `Receiver::close()`: `asx_channel_seal` (`src/channel/mpsc.c:693-712`) wakes the parked senders and keeps the queue receivable; the GenServer's stop drains its mailbox through it (fixtures `mpsc-seal-drains-001`, `actor-cancel-before-start-drains-001`; unit test `test_mpsc.c` `seal_keeps_the_queue_receivable`). Rust parity of the drop: `lab-dispatch-mpsc-disconnect-order-001` (the receiver is dropped when its task completes; the later send reports `ASX_E_DISCONNECTED`). A sender parked at the moment of the receiver drop is not covered by a v2 fixture; unit test `test_channel_wake.c` `close_wakes_every_waiter`.

#### Last Sender Drop

1. `sender_count` is decremented; when it was 1 the receiver waker is taken and woken (`mpsc.rs:1241-1256`)
2. The receiver returns `Disconnected` after draining the queue

**C status:** implemented. `asx_channel_close_sender` (`src/channel/mpsc.c:651-662`) and `asx_channel_sender_drop` (`:625-647`) share `channel_sender_dropped` (`:595-620`), which decrements a non-last sender reference without closing or waking (`:596-600`); the last reference sets the sender-closed state and wakes every waiter (`:603-614`). The input of `mpsc-counted-senders-close-at-last-drop-001` covers two producers: one drops its sender, the other sends later, and the receiver disconnects only after the last sender drops and queued messages drain. This merge review does not add an executed comparison result.

#### Drain-After-Sender-Drop

The receiver drains queued messages after all senders drop: `Ok(value)` until the queue is empty, then `Disconnected` (`mpsc.rs:1801-1821`).

**C status:** implemented (`channel_recv_impl`, `src/channel/mpsc.c:1097-1124`). Rust parity: `mpsc-two-phase-send-recv-001`, `mpsc-try-ops-001`.

### 1.11 Ordering Guarantees

| Dimension | Ordering | Mechanism |
|-----------|----------|-----------|
| **Message delivery** | FIFO | `VecDeque`: `push_back()` / `pop_front()` |
| **Waiter scheduling** | FIFO | Token queue `VecDeque<SlabToken>`, head served first (`mpsc.rs:279-282`, `:386-395`) |
| **Cascade wake** | FIFO | Head of waiter queue woken first; the head wakes the next if capacity remains |
| **try_reserve / try_send fairness** | Strict FIFO | Return `Full` while a live waiter is queued, even if capacity exists |

**C status:** implemented: ring buffer FIFO (`src/channel/mpsc.c:802-804`, `:339-342`), wait queue FIFO by arrival (`src/sync/wait_queue.h:23`), head-only handoff (`mpsc.c:386-393`), no queue jumping (`mpsc.c:670-672`). Batch receive wakes one queued producer per freed slot (`mpsc.c:1104-1114`). Unit test: `test_mpsc.c` `fifo_ordering`.

---

## 2. Timer Wheel Semantics

**C status (whole section):** ported (bd-9kll.7.1, bd-9kll.7.4). `src/time/timer_wheel.c` is a port of the Rust wheel: four levels of 256 slots, a ready list, `cascade`, `refill_overflow`, `advance_to` with tick skipping, and a deadline-ordered overflow heap, in a fixed arena (`ASX_MAX_TIMERS` entries for the application instance, `asx_timer_wheel_global`, default 128, `include/asx/asx_config.h:277-283`). The scheduler's task timers (sleeps, and under lab dispatch budget deadlines) are registrations in a second instance, the kernel's scheduler wheel (`asx_kernel.wheel`), sized for two per task, so there is one timer structure and one order in every mode (`timers_fire`, `src/runtime/scheduler.c`). Coalescing (off by default in Rust) is not modelled; a cancelled timer leaves its slot at once instead of when the slot is visited, which cannot change what fires or in which order.

### 2.1 Structure: 4-Level Hierarchical Wheel

| Level | Slots | Resolution | Range |
|-------|-------|------------|-------|
| 0 | 256 | 1 ms | 256 ms |
| 1 | 256 | 256 ms | 65.536 s |
| 2 | 256 | 65.536 s | 16 777.216 s (~4.66 h) |
| 3 | 256 | 16 777.216 s (~4.66 h) | 4 294 967.296 s (~49.7 days) |
| Overflow | `BinaryHeap<OverflowEntry>` | N/A | Delta at or beyond `min(max_wheel_duration, level-3 range)`; `max_wheel_duration` defaults to 24 h |

Resolutions and ranges follow from `LEVEL0_RESOLUTION_NS = 1 ms`, `SLOTS_PER_LEVEL = 256` and `range = resolution * 256` (`wheel.rs:43-56`, `wheel.rs:311-320`). The overflow threshold is `max_range_ns` (`wheel.rs:1091-1098`). The module doc (`wheel.rs:10`) says the wheel range is "approximately 37.2 hours"; that figure does not match the constants.

**Slot assignment:** `tick = deadline_ns / level.resolution_ns`, `slot = tick % 256` (`wheel.rs:728-747`).

**Bitmap occupation:** each level has `BITMAP_WORDS = 4` `u64` words for skipping empty slots (`wheel.rs:297`, `wheel.rs:300-309`).

**C status:** spec (Rust) — not implemented in C (see the section note).

### 2.2 Timer Entry Structure

```
TimerEntry {             // wheel.rs:245-250
    deadline: Time,
    waker: Waker,
    id: u64,             // slab index in `active`
    generation: u64,     // use-after-cancel prevention
}
```

Active timers are tracked in `active: slab::Slab<u64>` indexed by `id` and holding the generation (`wheel.rs:258`, field at `:396`), not in a `HashMap`.

**C status:** `tw_entry { deadline, waker_data, seq, generation (uint32_t), prev, next, heap_pos, where, level, slot }` in `src/time/timer_wheel.c`: an arena entry is on exactly one of the ready list, a slot's list or the overflow heap, linked by 16-bit arena indices; handle `{ slot, generation }`, both `uint32_t` (`include/asx/time/timer_wheel.h`). Released slots are reused last-in-first-out, as the Rust slab hands a freed id to the next insert.

### 2.3 Insert Semantics

```
try_register(deadline, waker) -> Result<TimerHandle, TimerDurationExceeded>
register(deadline, waker) -> TimerHandle
```

**Validation:** `try_register` (`wheel.rs:530-546`) returns `TimerDurationExceeded` when `deadline - current > max_timer_duration` (default 7 days, `wheel.rs:98-105`). `register` (`wheel.rs:515-524`) clamps such a deadline to the maximum instead of failing.

**Handle allocation** (`insert_validated`, `wheel.rs:550-566`): `generation = next_generation`, then `next_generation = next_generation.wrapping_add(1)`; `id = active.insert(generation)`, a slab index that is reused after the timer leaves `active`.

**Placement** (`insert_entry_at`, `wheel.rs:710-754`):
1. `deadline <= current`: pushed to the `ready` vector
2. `delta >= max_range`: pushed to the overflow heap (`:718-726`)
3. Otherwise: the first level whose range covers `delta`; a level-0 tick at or before the current tick goes to `ready` (`:735-741`)

**Duplicate handling:** no deduplication. Timers with the same deadline in the same slot keep `Vec::push` order.

**C status:** implemented. `asx_timer_register` is Rust's `register`: a deadline more than `max_timer_duration` past the wheel's time (whole milliseconds) is clamped to that maximum, not refused (default 7 days, `ASX_TIMER_MAX_DURATION_NS`; `asx_timer_set_max_duration` changes it). There is no `try_register` variant: no C caller needs the rejection, and Rust's runtime paths all use `register`. Placement is `insert_entry_at`: ready list, overflow heap past `min(max_wheel_duration, level-3 range)` (default 24 h, `ASX_TIMER_MAX_WHEEL_DURATION_NS`), else the finest level whose range holds the delta, with the level-0 same-tick rule. A full arena returns `ASX_E_RESOURCE_EXHAUSTED` and changes nothing; each arm advances the slot's generation. Unit tests (`tests/unit/time/test_timer_wheel.c`): `timer_beyond_max_duration_is_clamped_to_the_maximum`, `timer_default_max_duration_is_seven_days_and_clamps_beyond_it`, `timer_clamp_measures_from_wheel_time_in_whole_milliseconds`, `timer_resource_exhaustion`, `timer_slot_recycling_after_cancel`, `timer_overflow_timer_promotes_and_fires`.

### 2.4 Fire Semantics

```
collect_expired(now) -> WakerBatch
```

**Process** (`collect_expired`, `wheel.rs:699-703`):
1. **Advance:** `synchronize` (`wheel.rs:500-505`) runs `advance_to` (`wheel.rs:756-783`), which ticks level 0 (`tick_level0`, `wheel.rs:849-863`) and promotes overflow entries that came into range (`refill_overflow`, `wheel.rs:903-919`)
2. **Cascade:** when the level-0 cursor wraps to 0, the next level's cursor advances and its slot's live entries are re-inserted, recursively (`cascade`, `wheel.rs:865-887`)
3. **Generation check:** dead entries are dropped during cascade (`:877-882`) and bucket collection (`collect_bucket`, `wheel.rs:889-901`)
4. **Collect:** `drain_ready` (`wheel.rs:921-968`) returns the wakers of live `ready` entries with `deadline <= now` and re-inserts later ones
5. **Coalescing** (optional, off by default; window 1 ms, `min_group_size` 1, `wheel.rs:157-165`): when the window containing `now` holds a group, due entries in that window wait for the window end (`wheel.rs:925-933`), so a coalesced timer fires at the first window boundary at or after its deadline (`window_end`, `wheel.rs:66-72`), never before it

**Fire criterion:** `entry.deadline <= now` (inclusive), subject to the coalescing hold above.

**C status:** implemented without coalescing. `asx_timer_collect_expired` synchronizes the wheel to `now` (`advance_to`, `tick_level0`, `cascade`, `refill_overflow`, with the same tick skipping) and drains the ready list front to back, firing entries with `deadline <= now` (inclusive) up to the caller's `max_wakers`; a later one is re-inserted, and a due one beyond capacity stays at the front, in order. Rust parity of fire order for task sleeps (the scheduler uses the same wheel): `timers-same-deadline-001`, `lab-dispatch-timer-wheel-order-001` (timers registered out of deadline order that come due in one auto-advance wake their tasks in registration order). Scenarios written to cover what those do not, pending capture from Rust: `timers-same-tick-registration-order-001` (5.7 ms before 5.2 ms), `timers-cascade-order-001`, `timers-multi-level-storm-001` (cascades from levels 1 and 2, overflow promotion), `timers-beyond-wheel-horizon-001` (30 h and clamped 8 d). Unit tests: `timer_same_tick_fires_in_registration_order`, `timer_ticks_fire_in_tick_order`, `timer_cascaded_from_coarser_level_follows_direct_timer_in_slot`, `timer_in_coarse_levels_fires_at_its_deadline`, `timer_churn_never_fires_early_loses_none_and_next_deadline_matches_a_scan`. Coalescing has no C implementation and no fixture.

### 2.5 Cancel Semantics

```
cancel(handle) -> bool          // wheel.rs:577-597
```

**Lazy deletion model:**
1. If `active[handle.id] == handle.generation`: remove it from `active`, return `true`
2. Generation mismatch or missing: return `false` (already fired, cancelled or stale)
3. The entry stays in wheel storage and is skipped later by `is_live()` (`wheel.rs:1071-1075`)
4. When `active` becomes empty, `purge_inactive_storage()` clears `ready`, the overflow heap, all slots and bitmaps (`wheel.rs:1100-1110`)
5. Otherwise, once cancellations since the last compaction exceed `2 * live + COMPACTION_SLACK` (1024, `wheel.rs:47`), `compact_inactive_storage()` drops cancelled entries (`wheel.rs:1116-1140`)

**C status:** implemented as an O(1) cancel without lazy storage: `asx_timer_cancel` checks the slot range, liveness and the generation, then unlinks the entry from the ready list, its slot's list (clearing the occupied bit when the slot empties) or the overflow heap, and releases the slot for the next registration. There is no stored dead entry to purge or compact, which cannot change what fires or in which order. Unit tests: `test_timer_wheel.c` `timer_cancel_prevents_fire`, `timer_stale_handle_cancel_returns_false`, `timer_double_cancel_returns_false`, `timer_overflow_timer_can_be_cancelled`.

### 2.6 Generation-Safe Handle Contract

```
TimerHandle { id: u64, generation: u64 }   // wheel.rs:225-228
```

**Invariants:**
- `id` is the slab index in `active` (`wheel.rs:555`) and is reused after removal; it does not wrap independently
- `generation` comes from `next_generation.wrapping_add(1)` (`wheel.rs:551-552`)
- Cancel requires an exact `(id, generation)` match; a stale handle whose `id` was reused fails because the slab entry holds the new generation
- The test `wheel_register_wraps_id_and_generation_without_immediate_collision` (`wheel.rs:1406-1437`) checks two handles across a generation wrap from `u64::MAX` to 0

**C status:** implemented with 32-bit generations: every arm of a slot advances that slot's generation, skipping 0 on wrap (`tw_next_generation`, `src/time/timer_wheel.c`), and `asx_timer_wheel_reset` advances every used slot's generation so handles from before a reset are stale. Unit tests: `timer_generation_increments_on_reuse`, `timer_stale_handle_rejected_after_reset`.

### 2.7 Configuration

| Parameter | Default | Purpose |
|-----------|---------|---------|
| `max_wheel_duration` | 24 hours | Max delta placed in the wheel (beyond goes to the overflow heap) |
| `max_timer_duration` | 7 days (168 h) | Max duration accepted by `try_register()`; `register()` clamps to it |
| `coalesce_window` / `min_group_size` / `enabled` | 1 ms / 1 / false | Coalescing |

**Rust source:** `wheel.rs:80-105` (`TimerWheelConfig`), `wheel.rs:137-165` (`CoalescingConfig`)

**C status:** `max_timer_duration` is `ASX_TIMER_MAX_DURATION_NS`, default 7 days, settable per wheel with `asx_timer_set_max_duration`; `max_wheel_duration` is `ASX_TIMER_MAX_WHEEL_DURATION_NS`, 24 h, fixed. Coalescing is not configurable (not implemented).

### 2.8 Timer Ordering Within Deadline

| Condition | Ordering | Mechanism |
|-----------|----------|-----------|
| Different deadlines | Deadline order (earliest first) | Wheel cursor advancement |
| Same deadline, same slot | Insertion order | `Vec::push()` appends; iteration preserves order |
| Overflow heap | Deadline, then lower generation (wrapping signed difference), then id | `Ord for OverflowEntry` (`wheel.rs:276-294`) |

**C status:** the same structure, so the same order, for the application wheel and for task sleep timers: a timer's 1 ms tick, then its arrival at the slot (unit tests `timer_tiebreak_insertion_order`, `timer_same_tick_fires_in_registration_order`, `timer_interleaved_ticks_keep_registration_order_within_each`, `timer_ready_list_keeps_registration_order_across_equal_and_earlier_deadlines`, `timer_cascade_preserves_slot_order`). Overflow timers come out by deadline, then registration sequence (`tw_ovf_less`; Rust's generation order), tested by `timer_overflow_timers_fire_by_deadline_then_registration`.

---

## 3. Deterministic Scheduler Semantics

**C status (whole section):** C does not port `three_lane.rs`. Rust parity runs use C's lab dispatch (`src/runtime/lab_dispatch.c`), which ports the Rust `LabRuntime` dispatch with one worker (`lab/runtime.rs` `pop_for_worker`, `:6324-6373`, over `priority.rs` lanes). Outside lab dispatch, `src/runtime/scheduler.c` polls runnable tasks in ascending arena order each round (`scheduler.c:1-22`, loop at `:1424`). The optional parallel profile (`src/runtime/parallel.c`) is a C design of its own, described where relevant below.

### 3.1 Three-Lane Architecture

The Rust multi-worker scheduler has three priority lanes:

| Lane | Priority | Queue Type | Ordering |
|------|----------|------------|----------|
| **Cancel** | Highest | `GlobalFifoQueue` (global; wraps `crossbeam_queue::SegQueue` on native targets, `global_queue.rs:9-26`) + `BinaryHeap<SchedulerEntry>` (local) | Priority, then generation |
| **Timed** | Middle | `Mutex<TimedQueue>` (global, `global_injector.rs:172`, `:194-200`) + `BinaryHeap<TimedEntry>` (local) | EDF, then generation |
| **Ready** | Lowest | `GlobalFifoQueue` (global) + `BinaryHeap<SchedulerEntry>` (local) + fast paths | Priority, then generation |

**Lane order by governor suggestion** (`next_task`, `three_lane.rs:6905-7104`; suggestion from `governor_suggest`, `:7446`):

| Suggestion | Lane Order |
|------------|------------|
| `NoPreference` (default) | Cancel > Timed > Ready |
| `MeetDeadlines` | Timed > Cancel > Ready; after `timed_fairness_limit` consecutive timed dispatches ready work is tried first (`:6975-6984`) |
| `DrainObligations` | Cancel > Timed > Ready (cancel-streak limit doubled, `:6960-6965`) |
| `DrainRegions` | Cancel > Timed > Ready (cancel-streak limit doubled) |

**C status:** spec (Rust) — not implemented in C. The C lab has a cancel lane and a ready lane and no governor (`src/runtime/lab_dispatch.c:611-631`). Rust's `LabRuntime::pop_for_worker` also checks a timed lane (`lab/runtime.rs:6347-6353`), but in `src/lab` nothing outside tests feeds it: `schedule_timed` (`lab/runtime.rs:6242`) is called only at `:7658` and `:7723`, inside the `#[cfg(test)]` module that starts at `:6691`. `parallel.c` polls its lanes in Rust's order, cancel, timed, ready (`g_priority_order` in `src/runtime/parallel.c`, bd-9kll.4.7); its timed lane dispatches the earliest due deadline (the task's armed timer or budget deadline) and consumes the entry. The sweep scheduler (`scheduler.c`, non-lab) dispatches the cancel lane before ready work and orders both by priority (bd-9kll.4.6). Rust parity of cancel-lane-first in the lab: `lab-dispatch-cancel-waker-once-001` (a task is dispatched once on the cancel lane, then on the ready lane).

### 3.2 Scheduler Loop

`next_task` (`three_lane.rs:6905-7104`; phase doc at `:6380-6452`):

```
next_task():
  Phase -0.6/-0.5: apply queued task-handle commands, publish deferred cancel effects
  Phase 0:   process expired timers (fires wakers -> injects tasks)     :6914-6917
  Phase 0.5-0.7: spawn-mailbox admission, local spawns, region commands
  Phase 1:   one global probe in suggestion order (cancel or timed)     :6990-7008
  Phase 2:   local lanes + global under one local lock, suggestion order :7010-7063
  Phase 3:   fast ready paths (local_ready, fast queue, global ready)   :7211-7286
  Phase 3b:  local ready lane with RNG hint                             :7270
  Phase 4:   work stealing                                              :7080-7084
  Phase 5:   fallback cancel when the streak limit deferred it          :7086-7100
run_loop_until (:5570): when next_task finds nothing, spin -> yield -> park (section 3.10)
```

**C status:** spec (Rust) — not implemented in C. The C lab step (`src/runtime/scheduler.c:1246-1296`) follows `LabRuntime::step_inner` (`lab/runtime.rs:4480-`): drain admissions and commands, draw one RNG value (`lab/runtime.rs:4499`), process timers (`:4509-4511`), then pick a task. C does the same in that order (`scheduler.c:1260-1272`).

### 3.3 Entry Ordering Contracts

#### SchedulerEntry (Cancel/Ready lanes)

```
Compare:
  1. Higher priority first (u8, higher value = more important)
  2. Earlier generation first (lower number = earlier insertion)
  3. Lower task id first
```

**Rust source:** `priority.rs:21-41`

#### TimedEntry (Timed lane)

```
Compare:
  1. Earlier deadline first
  2. Earlier generation first (wrapping signed difference)
  3. Lower task id first
```

**Rust source:** `priority.rs:54-75`

**C status:** implemented for the lab's cancel and ready lanes. A `lab_entry` holds `{slot, task_gen, priority, gen}` (`src/runtime/lab_dispatch.c:43-48`); lanes keep entries in push order, which is generation order, and a pop takes the highest-priority group (`lab_dispatch.c:569-609`). Each entry's generation comes from one counter (`:442`, `:534`), so generations never tie and the task-id key is never needed. There are no timed-lane entries in C.

### 3.4 Generation Counter (FIFO Guarantee)

Each `Scheduler` (`priority.rs`, re-exported as `PriorityScheduler` in `scheduler/mod.rs:74`) keeps `next_generation: u64` (`priority.rs:329`), incremented by 1 per insertion (`next_gen`, `priority.rs:485-489`). The global `TimedQueue` keeps its own counter (`global_injector.rs:194-200`).

**C status:** implemented for the lab: one counter `g_lab_next_gen` for both lanes (`src/runtime/lab_dispatch.c:85`, `:442`, `:534`).

### 3.5 RNG-Based Deterministic Tie-Breaking

`pop_entry_with_rng` (`priority.rs:759-790`):

1. Pop the top entry; if the next entry has a different priority, return it
2. Otherwise collect the entries sharing that priority, up to the scratch capacity (at most 256, `priority.rs:341`)
3. Pick index `rng_hint % count` (`tie_break_index`, `priority.rs:417-421`), push the rest back

This is used by the cancel lane (`pop_cancel_with_rng`, `priority.rs:674-685`), the ready lane (`pop_ready_only_with_hint`, `:985-998`) and, with deadlines, the timed lane (`pop_timed_only_with_hint`, `:952-965`), not only the local ready lane.

RNG sources: each `three_lane` worker has `DetRng::new(worker_id)` (`three_lane.rs:2365`) and draws once per `next_task` Phase 2 (`:7014`). The `LabRuntime` has one `DetRng` seeded from the config seed (`lab/runtime.rs:2426`, `lab/config.rs:437-439`) and draws once per step (`lab/runtime.rs:4499`). `DetRng::next_u64` is xorshift64 (`util/det_rng.rs:267-275`).

**C status:** implemented for the lab. `asx_scheduler_use_lab_dispatch` seeds a xorshift64 with the scenario seed (`src/runtime/lab_dispatch.c:410-424`) and remaps degenerate seeds as `DetRng::new` does (`src/runtime/lab_dispatch.c:275-294`); `lab_pop` picks `group[r % n]` among up to 256 highest-priority entries in generation order (`lab_dispatch.c:569-609`). Per-worker RNGs: not implemented (one worker). Rust parity: `lab-dispatch-tie-break-001` (four tasks contend for a one-permit semaphore at time 0; the seeded tie-break decides which takes it first), `scheduler-yield-interleave-001` (dispatch order follows the seed). Unit tests: `tests/unit/runtime/test_lab_dispatch.c` `rng_replicates_rust_xorshift64`, `ties_are_broken_by_the_step_value_in_generation_order`.

### 3.6 Work Stealing

`steal_task` (`stealing.rs:16-72`) is not a plain circular scan:

```
steal_task(stealers, rng):
  if len == 1: return stealers[0].steal()
  idx1 = rng.next_usize(len); idx2 = rng.next_usize(len) (distinct)
  try the candidate with the larger stealable_len_hint, then the other   // power of two choices
  start = rng.next_usize(len)
  for i in 0..len: idx = (start + i) % len, skip idx1/idx2, try steal
  return None
```

**C status:** spec (Rust) — not implemented in C. Lab dispatch has one worker. `parallel.c` moves a task to a worker chosen by a per-lane round-robin cursor and counts that as a steal (`parallel_select_worker`, `src/runtime/parallel.c:358-385`); it uses no RNG and not the power-of-two-choices rule.

### 3.7 Cancel-Streak Fairness

- **Base limit:** `DEFAULT_CANCEL_STREAK_LIMIT = 16` (`three_lane.rs:166`); adaptive mode may change it at epoch boundaries (module doc `three_lane.rs:71-73`)
- **Doubled under:** `DrainObligations` or `DrainRegions` (`three_lane.rs:6960-6965`)
- When the streak reaches the effective limit the cancel lane is skipped (`:6969-6972`); if no other lane has work, Phase 5 dispatches one cancel task and sets the streak to 1 (`:7086-7100`)
- The streak resets to 0 on a timed or ready dispatch (`:7306-7326`) and after backoff/park (`:5830-5833`)

**C status:** implemented in the lab as `LabRuntime` does it, without governor doubling: the cancel lane is served while the streak is below 16 (`LAB_CANCEL_STREAK_LIMIT`, `src/runtime/runtime_internal.h:728`), then the ready lane (streak reset to 0), then the cancel lane again with the streak set to 1 (`lab_dispatch.c:611-631`), as in `lab/runtime.rs:6324-6373`. No v2 scenario description mentions the streak limit; whether any fixture reaches 16 consecutive cancel dispatches was not checked. `parallel.c` has its own limit of 16 (`src/runtime/parallel.c:64`, `:934-942`, setter `:1303`).

### 3.8 Fairness Certificate and Replay

The scheduler produces a `PreemptionFairnessCertificate` (renamed from `FairnessPreemptionCertificate`; `three_lane.rs:4624-4657`) with 16 fields: `base_limit`, `effective_limit`, `observed_max_cancel_streak`, `cancel_dispatches`, `timed_dispatches`, `ready_dispatches`, `fairness_yields`, `observed_max_ready_stall_steps`, `observed_max_timed_stall_steps`, `ready_priority_inversions`, `max_ready_priority_inversion_gap`, `fallback_cancel_dispatches`, `base_limit_exceedances`, `effective_limit_exceedances`, `adaptive_enabled`, `adaptive_current_limit`.

**Witness hash:** `witness_hash()` feeds all 16 fields to a `DetHasher` (`three_lane.rs:4709-4730`). The same field values give the same hash.

**C status:** spec (Rust) — not implemented in C (no fairness certificate or witness hash in `src/runtime/scheduler.c`, `lab_dispatch.c` or `parallel.c`). The v2 fixtures carry a Rust `schedule.certificate_hash`, but the comparator checks `schedule.dispatches` only (`tools/conformance/runner.c:187-197`, `:257-264`).

### 3.9 Queue Data Structures

| Structure | Location | Purpose |
|-----------|----------|---------|
| `BinaryHeap<SchedulerEntry>` | Local cancel/ready (`priority.rs:319-326`) | Priority + generation ordering |
| `BinaryHeap<TimedEntry>` | Local timed | EDF + generation ordering |
| `GlobalFifoQueue<PriorityTask>` | Global cancel/ready (`global_injector.rs:170`, `:174`) | Unbounded FIFO injection (`SegQueue` on native targets) |
| `Mutex<TimedQueue>` | Global timed (`global_injector.rs:172`, `:194-200`) | EDF heap with generation counter |
| `IntrusiveStack` | Local fast ready (`intrusive.rs:378-`) | LIFO pop for the owner (`:576`), FIFO steal (`:612-`) |
| `ScheduledSet` | Membership tracking (`priority.rs:84-96`) | Dense generation tags with a `DetHashSet` overflow |

**C status:** the lab lanes are bounded arrays of `LAB_LANE_CAP = 8 * ASX_MAX_TASKS` entries in push order (`src/runtime/lab_dispatch.c:39`, `:51-54`); a full lane makes the run fail with `ASX_E_RESOURCE_EXHAUSTED` instead of dropping an entry (`lab_dispatch.c:27-29`, `:432-443`). No global injector or intrusive stack.

### 3.10 Backoff/Park Behavior

When `next_task` finds nothing (`advance_empty_backoff`, `three_lane.rs:5517-5527`; constants `:195-197`):

| Phase | Iterations | Action |
|-------|------------|--------|
| Spin | 8 (`SPIN_LIMIT`) | Busy-wait |
| Yield | 2 (`YIELD_LIMIT`) | Thread yield |
| Park | — | Park with a timeout to the nearest deadline, or park without timeout |

The park deadline is chosen from the timer-driver, local timed and global timed deadlines (`select_backoff_deadline`, `three_lane.rs:529-545`, called at `:5741-5746`); with no deadline the worker parks without a timeout (`:5814`).

**C status:** spec (Rust) — not implemented in C. When every task is parked, the native C scheduler fires due timers, jumps a virtual clock to the earliest timer, or waits in the reactor hook until the deadline (`src/runtime/scheduler.c:11-19`; wait converted to whole ms at `:994-1030`, capped by `ASX_SCHED_MAX_IDLE_WAIT_MS` = 1000, `:55`). The lab run moves the virtual clock to the next timer when nothing is scheduled (`scheduler.c:1344-1351`).

---

## 4. Cross-Domain Interactions

### 4.1 Channel <-> Cancellation

| Interaction | Behavior |
|-------------|----------|
| Task cancelled while reserve pending | `Reserve` polls `checkpoint()` -> `SendError::Cancelled`; waiter removed from the queue; no capacity consumed (`mpsc.rs:1061-1068`) |
| Task cancelled while recv pending | `poll_recv()` polls `checkpoint()` -> `RecvError::Cancelled`; no message consumed (`mpsc.rs:1786-1797`) |
| Region closing with channel open | No channel code refers to regions: `mpsc.rs` mentions regions only in a doc comment (`:649`) |
| Permit dropped unresolved | `TrackedPermit` panics (section 1.9). Base `SendPermit` releases the slot and aborts its runtime obligation with reason `Cancel` (`mpsc.rs:1663-1673`) |

**C status:** cancellation rows implemented (section 1.8). A C channel records its region and refuses creation only when that region is already `Closed` (`src/channel/mpsc.c:434-439`); region close does not touch channels (the only channel call in `src/runtime/lifecycle.c` is `asx_channel_reset()` at `:198`). Rust parity: `mpsc-recv-cancel-first-001`.

### 4.2 Timer <-> Cancellation

| Interaction | Behavior |
|-------------|----------|
| Task cancelled with pending sleep | `Sleep` completes early when its task has a cancel whose kind is neither `Timeout` nor `Deadline` and a checkpoint reports it (`time/sleep.rs:789-795`); its terminal cleanup cancels the registered timer (comment `sleep.rs:787-788`) |
| Deadline or Timeout cancel during a sleep | The sleep is not cut short (`sleep.rs:792-793`); the task observes the cancel at a later checkpoint |
| Region closing with timers pending | Not verified in this pass |

**C status:** implemented for sleeps: `sleep_observes_cancel` (`src/time/sleep.c:44-59`) applies the same kind rule; a finished task's sleep timer is removed with it (`asx_task_timer_disarm_internal`, `src/runtime/scheduler.c:165-180`). Rust parity: `budget-deadline-sleep-checkpoint-001` (a task with deadline 100 ns sleeps 200 ns; the Deadline cancel does not cut the sleep short and the checkpoint after it acknowledges Deadline), `lab-dispatch-waker-rearm-001` (a pending sleep's timer is cancelled and registered again, `timer.cancelled` then `timer.scheduled`).

### 4.3 Timer <-> Scheduler

| Interaction | Behavior |
|-------------|----------|
| Timer expires | Phase 0 of `next_task` calls `process_timers()` (`three_lane.rs:6914-6917`); woken tasks are injected into lanes |
| Timer deadline -> timed lane | A woken task goes to the ready or timed lane depending on the task |
| No timers pending | The worker parks without a timeout (`three_lane.rs:5814`) |
| Next timer deadline | Park timeout = time to the nearest timer/timed deadline (`three_lane.rs:5729-5758`) |

**C status:** the lab fires due timers at the start of each step, before the pick (`src/runtime/scheduler.c:1267`), and the native scheduler at the start of each round (`scheduler.c:1417-1422`). C has no timed lane: a woken task is scheduled like any other wake.

### 4.4 Channel <-> Scheduler

| Interaction | Behavior |
|-------------|----------|
| Channel send wakes receiver | The receiver waker is taken and woken after the lock is released (`mpsc.rs:1616-1628`) |
| Channel recv wakes sender | The head waiter is woken (`mpsc.rs:1801-1810`) |
| Backpressure blocks sender | The sender stays pending until a dequeue, an abort or a leaving waiter wakes it (section 1.4) |

**C status:** implemented through the wait queues: `channel_settle` (`src/channel/mpsc.c:426-433`) wakes one parked receiver per committed message and the head producer while capacity is free; woken tasks are rescheduled by the scheduler. Rust parity: `mpsc-two-phase-send-recv-001`.

### 4.5 Channel <-> Obligation (Session Layer)

| Interaction | Behavior |
|-------------|----------|
| Reserve creates obligation | `TrackedPermit` holds an `ObligationToken<SendPermit>` (`session.rs:404-408`) |
| Send resolves obligation | `send()` commits it and returns `CommittedProof`; on a channel error it is aborted (`session.rs:416-426`) |
| Abort resolves obligation | `abort()` returns `AbortedProof` (`session.rs:441-445`) |
| Drop without resolution | **PANIC** (`obligation/graded.rs:991-1006`) |

**C status:** spec (Rust) — not implemented in C. The base-permit obligation that C does implement is described in section 1.9.

### 4.6 Channel/Timer <-> Outcome/Budget (bd-296.16)

| Interaction | Behavior |
|-------------|----------|
| Channel send fails (Disconnected) | The error is returned to the task body; channel code sets no task outcome (`mpsc.rs` uses `Outcome` only as the return type of `SendPermit::send`; `src/channel/mpsc.c` has no outcome code) |
| Channel op cancelled | In `mpsc-recv-cancel-first-001` a recv cancelled by the driver returns `ASX_E_CANCELLED` and the task ends Cancelled |
| Budget deadline reached | Cancels the task with `CancelKind::Deadline` (`types/cancel.rs:270`); there is no `DeadlineMiss` kind. Rust parity: `budget-deadline-sleep-checkpoint-001` |
| Budget exhaustion during channel ops | No channel-specific budget handling: neither `mpsc.rs` nor `src/channel/mpsc.c` reads a `Budget` |

---

## 5. Deterministic Tie-Break Contract

### 5.1 Complete Ordering Summary

| Domain | Primary Key | Secondary Key | Tertiary Key | C |
|--------|------------|---------------|--------------|---|
| **Channel messages** | FIFO (`VecDeque`) | N/A | N/A | FIFO ring buffer (`mpsc.c:802-804`, `:339-342`) |
| **Channel waiters** | FIFO (token queue) | N/A | N/A | FIFO by arrival (`wait_queue.h:23`) |
| **Timer wheel (same slot)** | Insertion order (`Vec`) | N/A | N/A | Slot list order (registration, or cascade order), `timer_wheel.c`, for the application wheel and task sleeps alike |
| **Timer overflow heap** | Deadline | Generation (wrapping) | Id | Deadline, then registration sequence (`tw_ovf_less`) |
| **Scheduler cancel lane** | Priority (u8, higher first) | Generation | Task id | Lab: priority, then `r % n` in generation order |
| **Scheduler timed lane** | Deadline (EDF) | Generation | Task id | Not implemented |
| **Scheduler ready lane** | Priority (u8, higher first) | Generation | Task id | Lab: priority, then `r % n` in generation order |
| **RNG pops (cancel, timed, ready)** | Priority or deadline | `rng_hint % count` among the top group | N/A | Lab: same for cancel and ready |
| **Work stealing** | Two random candidates, larger hint first | Circular scan from a random start | N/A | Not implemented |

The generation key orders the heaps; a pop with an RNG hint (Phase 2 of `next_task`, `three_lane.rs:7014`, and every `LabRuntime` pop) chooses among equal-priority entries by `rng_hint % count` (`priority.rs:759-790`), so dispatch among them is not FIFO.

### 5.2 Determinism Invariants

1. **Same seed -> same execution order:** RNG decisions use `DetRng`: one per `three_lane` worker seeded with the worker id (`three_lane.rs:2365`), one per `LabRuntime` seeded with the config seed (`lab/runtime.rs:2426`). C lab: one xorshift64 seeded with the scenario seed (`src/runtime/lab_dispatch.c:410-424`).
2. **Same inputs -> same outputs:** a general claim about all hot-path data structures; not verified in this pass.
3. **Generation monotonicity:** lane generations are `u64` counters incremented by 1 (`priority.rs:485-489`); timer generations use `wrapping_add` (`wheel.rs:551-552`).
4. **Wrap safety:** timer ids are slab indices and generations wrap (`wheel.rs:550-566`); see section 2.6.
5. **Witness reproducibility:** `witness_hash` is a `DetHasher` over the certificate fields (`three_lane.rs:4709-4730`). Not implemented in C.

### 5.3 C Implementation Requirements

For deterministic parity with Rust, and the current C status of each:

1. Channel waiter queue MUST be FIFO by arrival with head-only handoff — **implemented** (`src/sync/wait_queue.h:23`, `src/channel/mpsc.c:426-433`).
2. Timers MUST fire in the Rust wheel's order (1 ms tick, then arrival at the slot) — **implemented**: `timer_wheel.c` is a port of the hierarchical wheel and the scheduler's task timers use it (`src/runtime/scheduler.c`, `timers_fire`).
3. Scheduler entries MUST carry a generation from one counter and order by priority then generation — **implemented for the lab's cancel and ready lanes** (`src/runtime/lab_dispatch.c:432-443`, `:569-609`); no timed lane.
4. RNG MUST be seeded as Rust seeds it — **implemented for the lab's single RNG** (`lab_dispatch.c:275-294`, `:410-424`); per-worker RNGs are not implemented.
5. Work stealing MUST follow `stealing.rs` — **not implemented**.
6. A fairness certificate MUST produce the same witness hash for the same dispatch counts — **not implemented**.

---

## 6. Failure Paths and Invalid-Handle Behavior

### 6.1 Channel Failure Paths

| Failure | Rust detection | Rust result | C |
|---------|-----------|----------|---|
| Reserve on closed channel | `receiver_dropped` check (`mpsc.rs:1072-1079`) | `Disconnected` | `ASX_E_DISCONNECTED` (`mpsc.c:662-665`) |
| Send on closed channel | `receiver_dropped` check in `try_send_deferred_wake` (`mpsc.rs:1602-1614`) | `Disconnected(value)`, obligation aborted `Error` | `ASX_E_DISCONNECTED`, obligation aborted `Error` (`mpsc.c:780-786`); parity `lab-dispatch-mpsc-disconnect-order-001` |
| Recv on empty, all senders dropped | `sender_count == 0` and queue empty (`mpsc.rs:1813-1821`) | `Disconnected` | `ASX_E_DISCONNECTED` (`mpsc.c:980-983`); parity `mpsc-two-phase-send-recv-001` |
| Reserve cancelled | `cx.checkpoint()` (`mpsc.rs:1061-1068`) | `Cancelled`, waiter cleaned | `ASX_E_CANCELLED`, task withdrawn from the wait queues (`mpsc.c:1158-1165`, `:1039`) |
| Recv cancelled | `cx.checkpoint()` (`mpsc.rs:1786-1797`) | `Cancelled`, message preserved | `ASX_E_CANCELLED` (`mpsc.c:1077`); parity `mpsc-recv-cancel-first-001` |
| Permit leaked (session layer) | `ObligationToken::drop` while armed (`graded.rs:991-1006`) | **PANIC** | Not implemented |
| Zero capacity channel | `assert!(capacity > 0)` (`mpsc.rs:565`) | **PANIC** at creation | `ASX_E_INVALID_ARGUMENT` (`mpsc.c:430`) |

### 6.2 Timer Failure Paths

| Failure | Rust detection | Rust result | C |
|---------|-----------|----------|---|
| Duration exceeded | `deadline - current > max_timer_duration` (`wheel.rs:530-546`) | `TimerDurationExceeded` from `try_register`; `register` clamps | `asx_timer_register` clamps to the maximum (default 7 days); no rejecting variant |
| Cancel with stale handle | Generation mismatch in `active` (`wheel.rs:577-597`) | `false` | Returns 0 (`asx_timer_cancel`) |
| Cancel already-cancelled or fired timer | Id not in `active` | `false` | Returns 0 (slot not live) |
| Fire cancelled timer | `is_live()` false (`wheel.rs:1071-1075`) | Skipped | Never reached: a cancelled timer leaves its list at once |
| Generation wrap | Wrapping `u64` generation with slab ids (`wheel.rs:550-566`) | Handles differ across the wrap (test `wheel.rs:1406-1437`) | 32-bit generation skipping 0 (`tw_next_generation`) |

### 6.3 Scheduler Failure Paths

| Failure | Rust detection | Rust result | C (lab dispatch) |
|---------|-----------|----------|---|
| Cancel-streak limit hit | `cancel_streak >= effective_limit` (`three_lane.rs:6969-6972`) | Skip cancel lane, try timed/ready; fallback cancel if none | Ready lane first after 16 cancel dispatches (`lab_dispatch.c:611-631`) |
| No tasks available | All lanes empty | Spin -> yield -> park (section 3.10) | Step dispatches nothing; the run auto-advances the clock or returns (`scheduler.c:1292-1346`) |
| Work steal failure | All stealers empty | Continue to Phase 5 / backoff | Not applicable (one worker) |
| Shutdown requested | Checked in `run_loop_until` (`three_lane.rs:5601`, `:5647`) | Exit loop | Not applicable |

---

## 7. Fixture Family Mapping

None of the 38 candidate IDs below exists in the repository: `grep -rl <id> fixtures tests tools src` finds no file for any of them (checked 2026-10-10). Each row says "not materialized" and, where a Rust-captured v2 fixture in `fixtures/rust_reference_v2` covers the same behaviour, names it.

### 7.1 Channel Fixtures

| Fixture ID | Description | Tests | Status |
|------------|-------------|-------|--------|
| `ch-reserve-send-001` | Basic reserve -> send -> recv cycle | Happy path | Not materialized; covered by `mpsc-two-phase-send-recv-001` |
| `ch-reserve-abort-001` | Reserve -> abort -> capacity freed | Abort cascade | Not materialized; no v2 scenario aborts a channel permit (C unit test `test_mpsc.c` `abort_returns_capacity`) |
| `ch-backpressure-001` | Full channel blocks sender, recv unblocks | Backpressure | Not materialized; covered by `mpsc-two-phase-send-recv-001` (capacity 1) |
| `ch-fifo-001` | Multiple senders, FIFO delivery order | Ordering | Not materialized; `mpsc-counted-senders-close-at-last-drop-001` has two producers; `mpsc-recv-many-wakes-each-freed-slot-001` checks FIFO and batch wakes with two parked producers |
| `ch-fifo-fairness-001` | try_reserve respects waiter queue FIFO | Fairness | Not materialized; no v2 fixture (C unit test `try_reserve_never_jumps_parked_producer`) |
| `ch-cancel-reserve-001` | Cancel during pending reserve | Cancel safety | Not materialized; no v2 fixture |
| `ch-cancel-recv-001` | Cancel during pending recv, message preserved | Cancel safety | Not materialized; partly covered by `mpsc-recv-cancel-first-001` (empty channel, so message preservation is not exercised) |
| `ch-evict-001` | send_evict_oldest evicts front, returns evicted | Eviction | Covered by `mpsc-send-evict-oldest-001` |
| `ch-evict-reserved-001` | send_evict_oldest fails when all capacity reserved | Eviction boundary | Covered by `mpsc-send-evict-oldest-001` (a channel whose one slot is reserved refuses) |
| `ch-close-drain-001` | Sender drop -> receiver drains remaining | Close/drain | Not materialized; covered by `mpsc-two-phase-send-recv-001`, `mpsc-try-ops-001` |
| `ch-close-wake-001` | Receiver drop -> all senders woken with Disconnected | Close/wake | Not materialized; partly covered by `lab-dispatch-mpsc-disconnect-order-001` (send after the receiver drop; no parked sender) |
| `ch-obligation-leak-001` | TrackedPermit drop without send/abort -> panic | Obligation | Not materialized; `TrackedPermit` not implemented in C |
| `ch-obligation-commit-001` | TrackedPermit send -> CommittedProof | Obligation | Not materialized; `TrackedPermit` not implemented in C (base-permit commit: `mpsc-two-phase-send-recv-001`) |
| `ch-obligation-abort-001` | TrackedPermit abort -> AbortedProof | Obligation | Not materialized; `TrackedPermit` not implemented in C |
| `ch-cascade-001` | Permit abort cascades wake to next waiter | Cascade | Not materialized; no v2 fixture |

### 7.2 Timer Fixtures

| Fixture ID | Description | Tests | Status |
|------------|-------------|-------|--------|
| `tm-insert-fire-001` | Basic insert -> advance -> fire | Happy path | Not materialized; covered by `timers-same-deadline-001` |
| `tm-same-deadline-001` | Multiple timers, same deadline, insertion order | Ordering | Not materialized; covered by `timers-same-deadline-001`, `lab-dispatch-timer-wheel-order-001` |
| `tm-cancel-001` | Insert -> cancel -> advance -> not fired | Cancel | Not materialized; no direct v2 fixture (`lab-dispatch-waker-rearm-001` cancels and re-registers a sleep timer) |
| `tm-cancel-stale-001` | Cancel with stale generation -> rejected | Invalid handle | Not materialized; no v2 fixture (C unit test `timer_stale_handle_cancel_returns_false`) |
| `tm-cancel-reuse-001` | Same ID, different generation -> independent | Generation safety | Not materialized; no v2 fixture (C unit test `timer_generation_increments_on_reuse`) |
| `tm-duration-exceeded-001` | Insert beyond max duration -> clamped (`register`) | Validation | Scenario `timers-beyond-wheel-horizon-001` written, capture pending; C unit tests `timer_beyond_max_duration_is_clamped_to_the_maximum`, `timer_default_max_duration_is_seven_days_and_clamps_beyond_it` |
| `tm-cascade-001` | Level 0 wrap triggers Level 1 cascade | Cascade | Scenarios `timers-cascade-order-001`, `timers-multi-level-storm-001` written, capture pending; C unit test `timer_cascaded_from_coarser_level_follows_direct_timer_in_slot` |
| `tm-overflow-001` | Far-future timer in overflow heap | Overflow | Scenarios `timers-beyond-wheel-horizon-001`, `timers-multi-level-storm-001` written, capture pending; C unit tests `timer_overflow_timer_promotes_and_fires`, `timer_overflow_timers_fire_by_deadline_then_registration` |
| `tm-coalesce-001` | Timers within coalescing window fire together | Coalescing | Not materialized; not implemented in C |
| `tm-wrap-001` | ID and generation u64 wrap without collision | Wrap safety | Not materialized; no v2 fixture |
| `tm-purge-001` | All timers cancelled -> storage purged | Cleanup | Not materialized; not applicable to C (no lazy storage) |
| `tm-immediate-001` | Deadline in past -> immediately ready | Edge case | Not materialized; no v2 fixture (C unit test `timer_zero_deadline_fires_immediately`) |

### 7.3 Scheduler Fixtures

| Fixture ID | Description | Tests | Status |
|------------|-------------|-------|--------|
| `sc-lane-priority-001` | Cancel dispatched before timed before ready | Lane ordering | Not materialized; partly covered by `lab-dispatch-cancel-waker-once-001` (cancel lane before ready lane; the C lab has no timed lane). `parallel.c` has one: `parallel_lanes_dispatch_cancel_then_timed_then_ready` (C only) |
| `sc-fifo-priority-001` | Equal priority tasks dispatched in insertion order | FIFO | Not materialized; under lab dispatch equal-priority tasks are picked by RNG, not FIFO |
| `sc-edf-001` | Timed tasks dispatched earliest-deadline-first | EDF | Not materialized as a fixture (Rust's lab never feeds its timed lane at the pinned rev; the C lab has none). `parallel.c` has an EDF timed lane: `parallel_timed_lane_dispatches_earliest_deadline_first`, `parallel_timed_task_waits_for_its_deadline` (C only) |
| `sc-edf-fifo-001` | Equal deadline timed tasks dispatched FIFO | EDF + FIFO | Not materialized; no timed lane in C |
| `sc-rng-tiebreak-001` | RNG tie-breaking deterministic with seed | Determinism | Not materialized; covered by `lab-dispatch-tie-break-001`, `scheduler-yield-interleave-001` |
| `sc-steal-001` | Work stealing follows RNG-seeded order | Stealing | Not materialized; not implemented in C |
| `sc-cancel-streak-001` | Cancel streak limit triggers fairness yield | Fairness | Not materialized; no v2 fixture identified |
| `sc-governor-meet-001` | MeetDeadlines suggestion reorders timed > cancel | Governor | Not materialized; not implemented in C |
| `sc-governor-drain-001` | DrainObligations doubles cancel-streak limit | Governor | Not materialized; not implemented in C |
| `sc-certificate-001` | Identical traces produce identical witness hash | Replay | Not materialized; not implemented in C |
| `sc-timer-phase0-001` | Expired timers processed before task dispatch | Phase ordering | Not materialized; no v2 fixture named for it (the C lab fires due timers before each pick, `scheduler.c:1240`) |

---

## 8. Invariant Schema Cross-Reference

### 8.1 Channel Invariants

| ID | Invariant | Category | C |
|----|-----------|----------|---|
| `INV-CH-01` | `used_slots = queue.len() + reserved <= capacity` at all times | Capacity | Yes (`mpsc.c:677-679`) |
| `INV-CH-02` | Messages delivered in FIFO order (`VecDeque`) | Ordering | Yes (ring buffer) |
| `INV-CH-03` | Waiters serviced in FIFO order (token queue) | Fairness | Yes (`wait_queue.h:23`) |
| `INV-CH-04` | `try_reserve` returns `Full` while a live waiter is queued, regardless of capacity | Fairness | Yes (`mpsc.c:670-672`) |
| `INV-CH-05` | Cancelled reserve does not consume capacity | Cancel safety | Yes (cancel checked first, `mpsc.c:1039`) |
| `INV-CH-06` | Cancelled recv does not consume message | Cancel safety | Yes (`mpsc.c:1077`) |
| `INV-CH-07` | `receiver_dropped` monotone: `false -> true`, never reverses | State monotonicity | Yes: no transition back to `OPEN` (a slot is reused only after `FULLY_CLOSED` and drained, `mpsc.c:444-445`) |
| `INV-CH-08` | `sender_count` never rises from 0 (`WeakSender::upgrade` refuses, `mpsc.rs:1283-1287`); `Sender::clone` raises it while non-zero | State monotonicity | `asx_channel_sender_clone` increments live references and refuses sender-closed states (`src/channel/mpsc.c:551-590`); last drop closes the sender side (`channel_sender_dropped`, `:595-620`); a bare uncounted channel id represents a weak sender |
| `INV-CH-09` | Permit drop without send/abort releases the slot, wakes the head waiter and aborts the obligation (`Cancel`) | RAII safety | Tracked permits are dropped with their holder task (`src/channel/mpsc.c:866-879`, `src/runtime/lifecycle.c:585-595`); no lexical destructor for untracked permits; interpreter bodies also drop explicitly (`drop_send_permit`, `src/conformance/interpreter.c:1150`) |
| `INV-CH-10` | Session-tracked permit leaked -> panic (obligation linearity) | Obligation | Not implemented |

### 8.2 Timer Invariants

| ID | Invariant | Category | C |
|----|-----------|----------|---|
| `INV-TM-01` | Cancel requires exact `(id, generation)` match | Handle safety | Yes (`asx_timer_cancel`) |
| `INV-TM-02` | Cancelled timers silently skipped at fire time via `is_live()` | Lazy deletion | Cancel unlinks the entry at once, so none is reached at fire time |
| `INV-TM-03` | Timers fire in the wheel's order: tick, then arrival at the slot | Ordering | Yes (ported wheel; `timer_same_tick_fires_in_registration_order`, `timer_cascaded_from_coarser_level_follows_direct_timer_in_slot`) |
| `INV-TM-04` | `try_register` rejects duration > `max_timer_duration`; `register` clamps | Validation | `register` clamps (default 7 days); no rejecting variant |
| `INV-TM-05` | All-cancelled triggers `purge_inactive_storage()`; many cancellations trigger compaction | Cleanup | Not applicable (no lazy storage) |
| `INV-TM-06` | Generation wrap does not make handles collide (slab ids, wrapping `u64` generation) | Wrap safety | 32-bit generation skipping 0 |
| `INV-TM-07` | Cascade respects generation: dead entries dropped during level promotion | Cascade safety | Yes by construction: a cancelled timer is no longer in any slot when its level cascades |
| `INV-TM-08` | Fire criterion: `entry.deadline <= now` (inclusive), except a coalescing hold | Fire semantics | Yes (`tw_drain_ready`), without the coalescing hold |

### 8.3 Scheduler Invariants

| ID | Invariant | Category | C |
|----|-----------|----------|---|
| `INV-SC-01` | Cancel lane dispatched before timed before ready (default) | Lane priority | Lab: cancel before ready, no timed lane; sweep: cancel before ready, by priority; `parallel.c`: cancel, timed, ready |
| `INV-SC-02` | Equal-priority entries are heap-ordered by generation; RNG-hinted pops choose among them by `rng % count` | Ordering | Lab: RNG pick in generation order (`lab_dispatch.c:569-609`) |
| `INV-SC-03` | Equal-deadline timed entries ordered by generation | EDF ordering | Not implemented |
| `INV-SC-04` | Cancel-streak limit prevents cancellation starvation | Fairness | Lab: 16, no doubling; `parallel.c`: 16 |
| `INV-SC-05` | Same seed -> same work-steal choices | Determinism | Not implemented |
| `INV-SC-06` | Identical certificate fields produce identical witness hash | Replay | Not implemented |
| `INV-SC-07` | Phase 0 (timers) executes before task dispatch | Phase ordering | Lab: due timers fire before the pick (`scheduler.c:1240`) |
| `INV-SC-08` | Governor suggestion affects lane order but not correctness | Safety | Not implemented (no governor) |

### 8.4 Coverage Matrix

Counts are rows of the tables above.

| Domain | States | Operations | Errors | Invariants | Fixture IDs |
|--------|--------|------------|--------|------------|-------------|
| Channel | 4 implicit | 8 (reserve, send, try_send, abort, recv, try_recv, send_evict, close) | 6 variants (`SendError` 3 + `RecvError` 3) | 10 | 15 |
| Timer | N/A (stateless entries) | 4 (register, fire, cancel, purge) | 1 type | 8 | 12 |
| Scheduler | 3 lanes x N entries | 6 (inject_cancel, inject_timed, inject_ready, dispatch, steal, park) | 0 | 8 | 11 |
| **Total** | — | **18** | **7** | **26** | **38** |

None of the 38 fixture IDs is materialized (section 7). `docs/LIFECYCLE_TRANSITION_TABLES.md` section 9 lists further candidate fixture IDs for regions, tasks, obligations, cancellation and outcomes.
