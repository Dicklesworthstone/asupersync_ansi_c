# Channel/Timer Kernel Semantics and Deterministic Ordering Contract

> **Bead:** bd-296.17 (extraction); truth pass bd-9kll.2.12
> **Status:** Reference for what the Rust code does, with a **C status** line per section saying what the C port implements and where. Normative rules now live in asupersync's `asupersync_v4_formal_semantics.md`; sections it covers say so and point at the matching row of `docs/C_REFINEMENT_MAP.md`.
> **Provenance:** Re-extracted from asupersync `src/channel/mpsc.rs`, `src/time/{wheel.rs,intrusive_wheel.rs,driver.rs,budget_ext.rs,sleep.rs}`, `src/cx/cx.rs`, `src/trace/event.rs` and `src/types/budget.rs`.
> **Rust baseline:** `5e60b1c4c` (asupersync). Every Rust `path:line` below was read with `git -C /dp/asupersync show 5e60b1c4c:<path>`.
> **C baseline:** C `file:line` citations were read in the working tree at asx `b412780`. C line numbers drift; use them as grep anchors and look for the symbol named beside each one.
> **Cross-references:** `docs/CHANNEL_TIMER_SEMANTICS.md` (the fuller channel/timer/scheduler reference, re-verified at the same Rust baseline), `docs/C_REFINEMENT_MAP.md` (C status per v4 rule), `docs/LIFECYCLE_TRANSITION_TABLES.md` (bd-296.15)
> **Last verified:** 2026-10-10 (bd-9kll.2.12)

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

The Rust channel has no state enum. State is derived from two atomics in `ChannelShared` (`src/channel/mpsc.rs:297-311`):

| State | Condition | Description |
|-------|-----------|-------------|
| `Open` | `sender_count > 0 && !receiver_dropped` | Sends and receives possible |
| `SenderClosed` | `sender_count == 0 && !receiver_dropped` | All senders dropped; the queue may still hold messages to drain |
| `ReceiverClosed` | `receiver_dropped == true` | Receiver dropped or `Receiver::close()` called; sends fail |
| `FullyClosed` | `sender_count == 0 && receiver_dropped` | Both sides gone |

**C status:** an explicit enum (`include/asx/core/channel.h:68-73`):

```c
typedef enum {
    ASX_CHANNEL_OPEN = 0,
    ASX_CHANNEL_SENDER_CLOSED = 1,
    ASX_CHANNEL_RECEIVER_CLOSED = 2,
    ASX_CHANNEL_FULLY_CLOSED = 3
} asx_channel_state;
```

C has one sender side and no sender count, clone or weak sender; any task holding the channel id may reserve on it. A `FULLY_CLOSED` slot may be reused by a later `asx_channel_create` under a new generation (`src/channel/mpsc.c:392-397`); stale handles are refused with `ASX_E_STALE_HANDLE` (`src/channel/mpsc.c:121`).

### 1.2 Legal State Transitions

```
Open ──(all senders drop)──> SenderClosed
Open ──(receiver drops)────> ReceiverClosed
SenderClosed ──(receiver drops)──> FullyClosed
ReceiverClosed ──(all senders drop)──> FullyClosed
```

**Forbidden transitions (Rust):**
- `ReceiverClosed -> Open`: `receiver_dropped` goes `false -> true` once (comment at `src/channel/mpsc.rs:304-305`).
- `SenderClosed -> Open`: `WeakSender::upgrade` refuses to raise `sender_count` from 0 (`src/channel/mpsc.rs:1283-1287`). `Sender::clone` raises it while it is non-zero (`src/channel/mpsc.rs:1231-1239`).

**C status:** `asx_channel_close_sender` moves `OPEN -> SENDER_CLOSED` or `RECEIVER_CLOSED -> FULLY_CLOSED` and `asx_channel_close_receiver` moves `OPEN -> RECEIVER_CLOSED` or `SENDER_CLOSED -> FULLY_CLOSED`; closing a side twice returns `ASX_E_INVALID_STATE` (`src/channel/mpsc.c:422-480`). No transition returns to `OPEN`.

### 1.3 Two-Phase Send Protocol (Reserve/Send/Abort)

Superseded for the permit's obligation by asupersync_v4_formal_semantics.md §3.4 (rules RESERVE, COMMIT, ABORT, LEAK: `rule.obligation.reserve` #13, `rule.obligation.commit` #14, `rule.obligation.abort` #15, `rule.obligation.leak` #16); C status: see C_REFINEMENT_MAP.md rows `rule.obligation.reserve`, `rule.obligation.commit`, `rule.obligation.abort`, `rule.obligation.leak`. Summary: a reserve that goes through a `Cx` creates a `SendPermit` obligation held by the task; send commits it, abort aborts it. The channel-specific mechanics follow.

#### Phase 1: Reserve

Rust: `Sender::reserve(&cx)` (`src/channel/mpsc.rs:637`) returns a `Reserve` future; each poll (`Reserve::poll_inner`, `src/channel/mpsc.rs:1040-1172`) checks, in order:
1. Cancellation: `cx.checkpoint().is_err()` -> `SendError::Cancelled(())`, waiter removed (`:1061-1068`).
2. Receiver gone -> `SendError::Disconnected(())` (`:1072-1079`).
3. The caller is the head of the waiter queue (or the queue is empty) and `used_slots < capacity` -> `reserved += 1`, leave the queue, wake the next waiter if capacity remains (`:1081-1101`), then register a `SendPermit` obligation through `cx` (`:1116-1119`).
4. Otherwise register or refresh the waker in the FIFO waiter queue and return `Poll::Pending` (`:1146-1170`).

C signature (`include/asx/core/channel.h:179-180`):

```c
asx_status asx_channel_reserve(asx_channel_id id, asx_cx *cx, asx_send_permit *out);
```

**C status:** implemented. Cancellation is checked first (`channel_wait_cancelled`, `src/channel/mpsc.c:839-846`: trace message `mpsc::reserve cancelled`, task withdrawn from both wait lines, `ASX_E_CANCELLED`). Then `channel_reserve_impl` (`src/channel/mpsc.c:543-616`): sender side closed -> `ASX_E_INVALID_STATE` (`:552-555`, a C-only case, since a Rust `Sender` cannot reserve after it is dropped); receiver closed -> `ASX_E_DISCONNECTED` (`:557-560`); a live producer queued ahead -> full (`:565-567`); no capacity -> full (`:569-576`). Full parks the task (inside a scheduler poll) and `asx_channel_reserve` returns `ASX_E_PENDING` (`:535-541`, `:855`). On success the obligation is registered when the `Cx` has a task (`:859-866`). `asx_channel_try_reserve` (`src/channel/mpsc.c:670-672`) never parks and returns `ASX_E_CHANNEL_FULL`. Rust parity: `mpsc-two-phase-send-recv-001`.

**Capacity invariant:**

```
used_slots = queue.len() + reserved <= capacity
```

Rust: `used_slots` / `has_capacity` (`src/channel/mpsc.rs:351-359`). Reserved slots count against capacity before any value is supplied. C: `queue_len + reserved >= capacity` refuses a reserve (`src/channel/mpsc.c:572-576`; the lock-free backend counts both in `in_use`, `src/channel/mpsc.c:231`).

#### Phase 2: Send (Commit)

Rust: `SendPermit::send` returns `Outcome<(), SendError<T>>` and `SendPermit::try_send` returns `Result` (`src/channel/mpsc.rs:1565-1578`); both run `try_send_deferred_wake` (`src/channel/mpsc.rs:1588-1629`):
1. Mark the permit consumed and decrement `reserved`.
2. If the receiver is gone: return `Err(SendError::Disconnected(value))` and abort the permit's obligation. No sender is woken here: the receiver's drop already woke them (comment at `:1603`).
3. Otherwise push the value to the back of the queue, take the receiver waker, commit the obligation (`:1616-1628`).

The send cannot fail for lack of capacity: the slot was reserved.

C signature (`include/asx/core/channel.h:141`):

```c
asx_status asx_send_permit_send(asx_send_permit *permit, uint64_t value);
```

**C status:** implemented; messages are `uint64_t` tokens. An already consumed permit returns `ASX_E_INVALID_STATE` (`src/channel/mpsc.c:699`); a forged or stale token is refused (`:707-711`). Receiver closed -> obligation aborted with `ASX_OBLIGATION_ABORT_ERROR`, `ASX_E_DISCONNECTED` (`:715-721`). Otherwise the value is enqueued (`:737-739`), the obligation committed (`:742`) and the wait lines settled (`:743`). C also has `asx_channel_send(id, cx, value)` (`include/asx/core/channel.h:206`, `src/channel/mpsc.c:926-936`), Rust's `Sender::send(&cx, v)` (`src/channel/mpsc.rs:715`): a reserve with no obligation, then a commit.

#### Phase 3: Abort

Rust: `SendPermit::abort` (`src/channel/mpsc.rs:1633-1639`) marks the permit consumed, releases the slot through `release_capacity` (decrements `reserved`, records a cancellation, wakes the head waiter; `:1641-1653`) and aborts the obligation with reason `Explicit`.

C signature (`include/asx/core/channel.h:145`):

```c
void asx_send_permit_abort(asx_send_permit *permit);
```

**C status:** implemented (`src/channel/mpsc.c:700-723`): returns the slot, aborts the obligation with `ASX_OBLIGATION_ABORT_EXPLICIT` (`:715`) and settles the wait lines (`:716`). A second abort, or an abort after send, is a no-op (`:704`).

#### RAII Cleanup

Rust: `Drop for SendPermit` (`src/channel/mpsc.rs:1663-1673`) releases the slot (same head-of-queue wake) and aborts the obligation with reason `Cancel`.

**C status:** not implemented in the library; C has no destructors and no cleanup-stack registration for permits (`src/channel/mpsc.c` never touches a cleanup stack). A permit that is neither sent nor aborted keeps its slot reserved; if it carries an obligation, that obligation leaks when its holder completes (C_REFINEMENT_MAP.md row `rule.obligation.leak`). The conformance interpreter emulates the Rust drop by aborting the permit and its obligation with reason `Cancel` (`drop_send_permit`, `src/conformance/interpreter.c:797-808`).

**Linearity:** Rust consumes the permit by value, so a second send or abort does not compile. C enforces it at run time with the `consumed` flag and the permit token (`src/channel/mpsc.c:647-661`, `:704-708`).

### 1.4 Receive Semantics

Rust: `Receiver::recv(&cx)` (`src/channel/mpsc.rs:1719`) polls `poll_recv` (`src/channel/mpsc.rs:1778-1859`):
1. Cancellation: `cx.checkpoint().is_err()` -> `RecvError::Cancelled` (`:1786-1797`). No message is consumed.
2. Queue non-empty: `pop_front()` -> `Ok(value)`, head sender woken (`:1801-1810`).
3. Queue empty and (`sender_count == 0` or receiver closed) -> `RecvError::Disconnected` (`:1813-1821`).
4. Otherwise register the waker and return `Poll::Pending`.

`try_recv` (`src/channel/mpsc.rs:1956-1983`) returns `RecvError::Empty` instead of waiting. `Disconnected` is reported only once the queue is empty: messages committed before the last sender dropped are drained first.

C signatures (`include/asx/core/channel.h:157`, `:189-190`):

```c
asx_status asx_channel_try_recv(asx_channel_id id, uint64_t *out_value);
asx_status asx_channel_recv(asx_channel_id id, asx_cx *cx, uint64_t *out_value);
```

**C status:** implemented. `asx_channel_recv` (`src/channel/mpsc.c:938-946`) checks cancellation first, then `channel_recv_impl` (`src/channel/mpsc.c:811-878`): FIFO dequeue (`:748-764`); receiver closed -> `ASX_E_DISCONNECTED` (`:767-770`); sender closed and no outstanding permit -> `ASX_E_DISCONNECTED` (`:772-797`); otherwise park and return `ASX_E_PENDING`. `asx_channel_try_recv` returns `ASX_E_WOULD_BLOCK` when empty; the conformance interpreter reports that as `ASX_E_CHANNEL_EMPTY` (`src/conformance/interpreter.c:2415`). Rust parity: `mpsc-recv-cancel-first-001`, `mpsc-try-ops-001`, `mpsc-two-phase-send-recv-001`.

### 1.5 Backpressure Behavior

| Capacity state | Rust `reserve()` | Rust `try_reserve()` | Rust `try_send()` | C `asx_channel_reserve` | C `asx_channel_try_reserve` |
|---|---|---|---|---|---|
| Free capacity, no waiter | Permit | Permit | Sent | `ASX_OK` | `ASX_OK` |
| Free capacity, live waiter queued | Joins the FIFO queue | `Full` | `Full` | Parks, `ASX_E_PENDING` | `ASX_E_CHANNEL_FULL` |
| No capacity | Joins the FIFO queue | `Full` | `Full` | Parks, `ASX_E_PENDING` | `ASX_E_CHANNEL_FULL` |

Rust: `try_reserve` returns `Full` while `has_waiting_sender()` (`src/channel/mpsc.rs:732-754`, `:374-377`); `try_send` (`src/channel/mpsc.rs:773-794`) applies the same rule. **Key fairness rule:** a non-waiting reserve never jumps the waiter queue, even when capacity is free.

**C status:** implemented (`src/channel/mpsc.c:670-672`). C has no `Sender::try_send`; the conformance interpreter runs it as `asx_channel_try_reserve` plus `asx_send_permit_send` (`src/conformance/interpreter.c:2389-2402`). One difference in who counts as queued: Rust's queue keeps every registered waiter until its `Reserve` is polled or dropped (`prune_stale_waiter_front` drops only removed registrations, `src/channel/mpsc.rs:363-377`), so a cancel-requested or masked waiter still blocks `try_reserve`. C's `asx_wait_queue_live_ahead` counts only tasks in `CREATED`/`RUNNING` (`src/sync/wait_queue.c:321-334`, `:43-58`), so a cancel-requested waiter does not. See §7.4.

### 1.6 FIFO Ordering Guarantees

| Guarantee | Rust | C |
|-----------|------|---|
| Message delivery | FIFO (`VecDeque` push_back / pop_front) | FIFO ring buffer (`src/channel/mpsc.c:685-687`, `:748-764`) |
| Messages from different producers | Commit order | Commit order; under lab dispatch the commit order follows the seeded dispatch order |
| Waiter queue | FIFO token queue, head served first (`src/channel/mpsc.rs:386-395`) | FIFO by arrival (`src/sync/wait_queue.h:23`) |
| Wakeup | Head only; a head that takes a slot with capacity left wakes the next (`src/channel/mpsc.rs:1099-1101`) | `channel_settle` hands free capacity to the head of the line (`src/channel/mpsc.c:358-365`) |

C unit tests: `tests/unit/channel/test_mpsc.c` `fifo_ordering`; `tests/unit/channel/test_channel_wake.c` `reserve_waiters_served_in_arrival_order_deterministically`. No v2 fixture has more than one producer on a channel, so FIFO among several parked producers is not fixture-checked.

### 1.7 Error Taxonomy

| Rust error | C status | When |
|---|---|---|
| `SendError::Disconnected(T)` / `RecvError::Disconnected` | `ASX_E_DISCONNECTED` (`include/asx/asx_status.h:64`) | Receiver dropped or closed (send side); queue empty and every sender dropped, or receiver closed (recv side) |
| `SendError::Cancelled(T)` / `RecvError::Cancelled` | `ASX_E_CANCELLED` (`include/asx/asx_status.h:56`) | Checkpoint failure at the start of a `reserve`/`send`/`recv` poll |
| `SendError::Full(T)` | `ASX_E_CHANNEL_FULL` (`include/asx/asx_status.h:66`) from `asx_channel_try_reserve`; `ASX_E_PENDING` (`include/asx/asx_status.h:23`) from the waiting calls | No slot for the caller |
| `RecvError::Empty` | `ASX_E_WOULD_BLOCK` (`include/asx/asx_status.h:65`) from `asx_channel_try_recv`; `ASX_E_PENDING` from `asx_channel_recv` | Queue empty, senders alive |
| (none) | `ASX_E_INVALID_STATE` | Reserve after `asx_channel_close_sender`; send of a consumed permit |

Rust error enums: `SendError` (`src/channel/mpsc.rs:111-118`), `RecvError` (`src/channel/mpsc.rs:182-189`). Rust errors carry the unsent value; C errors carry none. `ASX_E_CHANNEL_EMPTY` (`include/asx/asx_status.h:69`) is not returned by `src/channel/mpsc.c`.

**Construction constraint:** Rust panics on `capacity == 0` (`assert!` at `src/channel/mpsc.rs:565`); `unbounded_channel` is `channel(usize::MAX)` (`src/channel/mpsc.rs:599-600`). C returns `ASX_E_INVALID_ARGUMENT` for 0 or above `ASX_CHANNEL_MAX_CAPACITY` (default 64, `include/asx/core/channel.h:57-59`; check at `src/channel/mpsc.c:384`), requires an open region (`src/channel/mpsc.c:388-390`, `ASX_E_INVALID_STATE` otherwise) and has no unbounded channel.

### 1.8 Close Semantics

#### Receiver Drop (Close from Receiver Side)

Rust `Drop for Receiver` (`src/channel/mpsc.rs:2122-2149`):
1. Drains the sender waiters from the queue.
2. Sets `receiver_dropped = true`.
3. Takes the receiver waker.
4. Takes the queued messages with `mem::take`; they are dropped outside the lock, not delivered.
5. Wakes every drained sender; they observe `Disconnected` on their next poll.

`Receiver::close()` (`src/channel/mpsc.rs:1698-1711`) differs: it sets `receiver_dropped` and wakes the senders but leaves queued messages receivable.

**C status:** `asx_channel_close_receiver` (`src/channel/mpsc.c:470-503`) is the drop: queued messages are discarded and every waiter is woken. `asx_channel_seal` (`src/channel/mpsc.c:505-522`) is `Receiver::close()`: senders are woken and the queue stays receivable (fixture `actor-cancel-before-start-drains-001`, where a GenServer drains its sealed mailbox). Rust parity of the drop: `lab-dispatch-mpsc-disconnect-order-001` (the receiver's task completes first, dropping the receiver; the later send reports `ASX_E_DISCONNECTED`).

#### Last Sender Drop (Close from Sender Side)

Rust `Drop for Sender` (`src/channel/mpsc.rs:1241-1256`):
1. `sender_count` is decremented.
2. If it was 1, the receiver waker is taken under the lock and woken. There is no second check under the lock; the comment at `:1246-1248` says the receiver is woken even if a `WeakSender::upgrade` raises the count again.
3. The receiver drains the queue, then sees `Disconnected`.

**Queue is not cleared.**

| Event | Queue cleared? | Pending senders notified? | Pending receiver notified? |
|-------|---------------|--------------------------|---------------------------|
| Receiver dropped | Yes | Yes (all) | N/A |
| Last sender dropped | No | N/A | Yes |

**C status:** `asx_channel_close_sender` (`src/channel/mpsc.c:422-445`) keeps the queue and wakes every waiter. Rust parity: `mpsc-two-phase-send-recv-001`, `mpsc-try-ops-001` (drain, then `ASX_E_DISCONNECTED`).

### 1.9 Cancellation Interaction

| Scenario | Rust | C |
|----------|------|---|
| Cancel during `reserve`, before a permit | `Cancelled`; no capacity consumed; the waiter's token is removed (`cleanup_waiter`, `src/channel/mpsc.rs:960-998`) and the next head is woken if it held a queue position and capacity is free | `ASX_E_CANCELLED`; the task leaves both wait lines and any wake it held passes on (`asx_channel_wait_cancel`, `src/channel/mpsc.c:816-829`) |
| `Reserve` future dropped while queued | `Drop for Reserve` runs `cleanup_waiter` (`src/channel/mpsc.rs:1225-1229`) | `asx_channel_wait_cancel` must be called; a completed task's node is reclaimed lazily (`src/sync/wait_queue.h:24-27`) |
| Permit held, sender task cancelled or permit dropped unsent | `Drop for SendPermit` releases the slot and aborts the obligation (`Cancel`) | No drop path (§1.3 RAII); the slot stays reserved until `asx_send_permit_abort` |
| Cancel during `recv` | `Cancelled`; no message consumed (the checkpoint runs before the pop) | `ASX_E_CANCELLED` (`src/channel/mpsc.c:942`) |
| Permit send / abort | Not cancellation-checked (`src/channel/mpsc.rs:1588-1653`) | Not cancellation-checked (`src/channel/mpsc.c:639-723`) |

The cancellation check itself is v4 §3.2 CANCEL-ACKNOWLEDGE at a checkpoint (`rule.cancel.acknowledge` #2; C status: C_REFINEMENT_MAP.md row `rule.cancel.acknowledge`). Rust parity: `mpsc-recv-cancel-first-001` (a pending recv cancelled by the driver returns `ASX_E_CANCELLED` and the task ends Cancelled). Reserve cancellation has no v2 fixture; C unit tests: `tests/unit/channel/test_channel_wake.c` `cancelled_waiter_does_not_absorb_wake`, `cancel_storm_on_full_channel_keeps_fifo_progress`, `masked_producer_is_not_cancelled_until_unmask`; `tests/unit/channel/test_mpsc.c` `cancelled_cx_wins_over_ready_channel_and_traces`.

### 1.10 Determinism Requirements

1. Per-producer FIFO is deterministic by construction (queue push/pop).
2. The waiter queue is FIFO. Rust keeps tokens in a `VecDeque<SlabToken>` with wakers in a `TokenSlab` (`src/channel/mpsc.rs:279-282`); there is no monotonic waiter id.
3. Cancellation is deterministic given deterministic checkpoint results.
4. Interleaving of several producers depends on the executor. Under Rust's `LabRuntime` and C's lab dispatch it follows the seeded dispatch order, which C reproduces (C_REFINEMENT_MAP.md row `inv.determinism.replayable`).
5. Wakeup order is head-of-queue.

**C status:** CORE builds are single-threaded. POSIX and PARALLEL profiles select a lock-free committed-message backend unless `ASX_LOCKFREE_SINGLE_THREAD` is set (`src/channel/mpsc.c:45-51`); its foreign-thread producers must not run while a scheduler task is parked on the same channel (`include/asx/core/channel.h:26-28`). Determinism of foreign-thread interleaving: unverified.

---

## 2. Timer Wheel Semantics

**C status (whole section):** the Rust hierarchical wheel is not ported. `src/time/timer_wheel.c` is a flat arena of `ASX_MAX_TIMERS` slots (default 128, `include/asx/time/timer_wheel.h:27-35`); its header calls the hierarchical wheel a future upgrade (`src/time/timer_wheel.c:1-10`). Its only runtime users are the deadline helpers (`src/time/deadline.c:45`, `:57`), and no runtime code calls `asx_timer_collect_expired` (only tests do); those deadlines are checked by time comparison (`asx_deadline_is_expired_at`, `src/time/deadline.c:78-86`). Task sleeps use the scheduler's timer min-heap keyed by (deadline, arm sequence) (`src/runtime/scheduler.c:100-105`); under lab dispatch due timers fire in (1 ms tick, registration) order to match the Rust wheel (`timers_fire_wheel_order`, `src/runtime/scheduler.c:256-324`).

### 2.1 Timer Wheel Structure

Rust: 4-level hierarchical wheel, 256 slots per level (`LEVEL_COUNT`, `SLOTS_PER_LEVEL`, `LEVEL0_RESOLUTION_NS`: `src/time/wheel.rs:43-56`):

| Level | Resolution | Range (256 slots) |
|-------|-----------|-------------------|
| L0 | 1 ms | 256 ms |
| L1 | 256 ms | 65.536 s |
| L2 | 65.536 s | ~4.66 h |
| L3 | ~4.66 h | ~49.7 days |

**Overflow heap:** a timer whose delta is at or beyond `min(max_wheel_duration, level-3 range)` (`max_range_ns`, `src/time/wheel.rs:1091-1093`) goes to a min-heap. `max_wheel_duration` defaults to 24 h and `max_timer_duration` to 7 days (`src/time/wheel.rs:98-105`). The module doc's "approximately 37.2 hours" (`src/time/wheel.rs:10`) does not match these constants.

**Occupied bitmap:** `BITMAP_WORDS = 4` `u64` words per level (`src/time/wheel.rs:297`).

**C status:** not implemented (see the section note).

### 2.2 Timer Insertion

Rust (`src/time/wheel.rs:515-566`):
1. `try_register` returns `TimerDurationExceeded` when `deadline - current > max_timer_duration` (`:530-546`); `register` clamps the deadline to the maximum instead (`:515-524`).
2. `generation = next_generation`, then `next_generation = next_generation.wrapping_add(1)`; `id = active.insert(generation)`, a slab index that is reused after the timer leaves `active` (`insert_validated`, `:550-566`).
3. Placement (`insert_entry_at`, `src/time/wheel.rs:710-754`): `deadline <= current` -> the `ready` vector; `delta >= max_range` -> overflow; otherwise the **finest** level whose range covers `delta` (levels are tried from L0 up), where a level-0 tick at or before the current tick goes to `ready` (`:735-741`).
4. Returns `TimerHandle { id, generation }` (`src/time/wheel.rs:225-228`).

C signature (`include/asx/time/timer_wheel.h:72-73`):

```c
asx_status asx_timer_register(asx_timer_wheel *wheel, asx_time deadline,
                              void *waker_data, asx_timer_handle *out_handle);
```

**C status:** partly implemented (`src/time/timer_wheel.c:139-168`). `ASX_E_TIMER_DURATION_EXCEEDED` when `deadline - current > max_duration` (`:118-121`, `:145`); the default maximum is Rust's 7 days (`ASX_TIMER_MAX_DURATION_NS`, `include/asx/time/timer_wheel.h:38-40`; 24 h before 2026-10-10), and there is no clamping `register`. A registration takes the first dead slot or a new one (`:147-164`) and fails with `ASX_E_RESOURCE_EXHAUSTED` when every slot is live (`:162`). Each arm records a monotonic `insertion_seq` and advances the slot's 32-bit generation (`:124-137`). The handle is `{ slot, generation }`, both `uint32_t` (`include/asx/time/timer_wheel.h:47-50`). No levels, ready vector or overflow heap.

### 2.3 Timer Firing

Rust `collect_expired` (`src/time/wheel.rs:699-703`):
1. `synchronize` (`:500-505`) runs `advance_to` (`:756-783`), which ticks level 0 (`tick_level0`, `:849-863`) and promotes overflow entries that came into range (`refill_overflow`, `:903-919`).
2. Empty ticks are skipped with the bitmaps (`next_skip_tick`, `:805`). With no live timer the advance is a single cursor update (`:757-760`).
3. Cascade: when the level-0 cursor wraps to 0, the next level's cursor advances and its slot's live entries are re-inserted, recursively (`cascade`, `:865-887`); dead entries are dropped there and in `collect_bucket` (`:889-901`).
4. `drain_ready` (`:921-968`) walks `ready` in order: dead entries are skipped, entries not yet due are re-inserted, and due entries are removed from `active` and their wakers collected. Optional coalescing (off by default; `src/time/wheel.rs:157-165`) holds due entries until the window end.

Fire criterion: `entry.deadline <= now` (inclusive), subject to the coalescing hold.

C signature (`include/asx/time/timer_wheel.h:111-112`):

```c
uint32_t asx_timer_collect_expired(asx_timer_wheel *wheel, asx_time now,
                                   void **out_wakers, uint32_t max_wakers);
```

**C status:** implemented in a different form (`src/time/timer_wheel.c:202-266`): scans live slots with `deadline <= now` (`:222`), sorts them by (deadline, insertion_seq) (`:230-248`) and fires at most `max_wakers` (`:251`); the rest stay live. No cascade, overflow promotion or coalescing. Rust parity of fire order for task sleeps (scheduler heap, not `timer_wheel.c`): `timers-same-deadline-001`, `lab-dispatch-timer-wheel-order-001`.

### 2.4 Timer Cancellation (O(1) Logical Cancel)

Rust `cancel` (`src/time/wheel.rs:577-597`):
1. If `active[handle.id] == handle.generation`: remove it from the `active` slab, return `true`.
2. Otherwise return `false` (fired, cancelled or stale).
3. The stored entry stays in its slot and is skipped later by `is_live` (`src/time/wheel.rs:1071-1075`).
4. When `active` becomes empty, `purge_inactive_storage` clears `ready`, the overflow heap, all slots and bitmaps (`src/time/wheel.rs:1100-1110`); otherwise, past `2 * live + COMPACTION_SLACK` (1024, `src/time/wheel.rs:47`) cancellations, `compact_inactive_storage` drops cancelled entries (`src/time/wheel.rs:1116-1140`).

A stale handle whose `id` was reused fails because the slab entry holds the new generation.

C signature (`include/asx/time/timer_wheel.h:90`):

```c
int asx_timer_cancel(asx_timer_wheel *wheel, const asx_timer_handle *handle);
```

**C status:** implemented without lazy storage (`src/time/timer_wheel.c:174-192`): checks the slot range, `alive` and the generation, then marks the slot dead and returns 1; otherwise 0. The slot is reused by a later registration. Unit tests: `tests/unit/time/test_timer_wheel.c` `timer_cancel_prevents_fire`, `timer_stale_handle_cancel_returns_false`, `timer_double_cancel_returns_false`.

**Update is cancel + re-register.** Rust `TimerDriver::update` (`src/time/driver.rs:522-540`) cancels the old handle and, only if that cancel succeeded, registers the new timer with `register` (which clamps); a stale handle leaves the wheel untouched and is returned as is. The slab reuses the freed `id`, so the replacement gets the same `id` with a new generation.

C signature (`include/asx/time/timer_wheel.h:131-134`):

```c
asx_status asx_timer_update(asx_timer_wheel *wheel, const asx_timer_handle *old_handle,
                            asx_time new_deadline, void *waker_data,
                            asx_timer_handle *out_handle);
```

**C status:** `asx_timer_update` (`src/time/timer_wheel.c:272-291`) refuses a stale handle with `ASX_E_STALE_HANDLE` and registers nothing (`:281-283`); a NULL old handle is a plain register (`:279`). A live timer is re-armed in its own slot under a new generation (`:289`), so a full arena cannot refuse an update. `ASX_E_TIMER_DURATION_EXCEEDED` is checked before anything changes (`:284`), where Rust's update would clamp. Unit tests: `timer_update_refuses_stale_handles_and_registers_nothing`, `timer_update_on_a_full_wheel_reuses_the_slot`, `timer_update_duration_failure_preserves_old_timer`.

### 2.5 Timer Error Conditions

| Error | Rust | C |
|-------|------|---|
| Duration exceeded | `TimerDurationExceeded` from `try_register` (default max 7 days); `register` and `TimerDriver::update` clamp | `ASX_E_TIMER_DURATION_EXCEEDED` (`include/asx/asx_status.h:74`), default max 7 days as in Rust, settable with `asx_timer_set_max_duration` (`src/time/timer_wheel.c:302-305`); no clamping |
| Stale handle on cancel | `false` | 0 |
| Capacity exhaustion | None: the slab grows | `ASX_E_RESOURCE_EXHAUSTED` when all `ASX_MAX_TIMERS` slots are live (`src/time/timer_wheel.c:162`) |

**Wrapping:** Rust generations wrap as `u64` and ids are reused slab indices; the test `wheel_register_wraps_id_and_generation_without_immediate_collision` (`src/time/wheel.rs:1406-1437`) checks handles across a generation wrap. C generations are per slot, 32-bit, and skip 0 on wrap (`src/time/timer_wheel.c:54-58`); `asx_timer_wheel_reset` advances every slot's generation so handles from before a reset are stale (`src/time/timer_wheel.c:94-112`).

### 2.6 Timer Wheel Invariants

Rust:
1. Every live timer (in `active`) has one stored entry in a level slot, `ready` or the overflow heap.
2. `active.len()` is the count of live timers.
3. Stored entries whose `(id, generation)` is not in `active` are dead and skipped (`is_live`).
4. When `active` becomes empty, all storage is purged (`purge_inactive_storage`).
5. Cascade re-inserts every live entry (`src/time/wheel.rs:877-882`), so it loses none.

**C status:** 1 and 2 hold trivially (one slot per timer; `active_count`, `src/time/timer_wheel.c:38-45`); 3-5 do not apply (no stored entries outside the slots, no cascade).

---

## 3. Deterministic Tie-Break Ordering Contract

### 3.1 Rust Behavior (Reference)

**Timer wheel (`wheel.rs`):** the earlier claim that same-deadline firing is nondeterministic because `drain_ready` uses `swap_remove` is wrong at 5e60b1c4c. `drain_ready` walks `ready` with `drain(..)` in order (`src/time/wheel.rs:921-968`); the only `swap_remove` in `wheel.rs` is in a test (`src/time/wheel.rs:1594`). Firing order is deterministic for a given sequence of registrations and clock advances. Within a slot it is push order, and an entry is pushed when it is registered or when it cascades from a coarser level (`src/time/wheel.rs:865-901`), so two equal-deadline timers that were filed at different levels need not fire in registration order. The overflow heap orders by deadline, then generation (wrapping signed difference), then id (`Ord for OverflowEntry`, `src/time/wheel.rs:276-294`).

**Intrusive wheel (`intrusive_wheel.rs`):** slot lists append at the back (`push_back`, `src/time/intrusive_wheel.rs:437`) and drain from the front (`src/time/intrusive_wheel.rs:251-270`), so they are FIFO within a slot. Outside its own module it is used only by end-to-end test files (`src/real_time_intrusive_wheel_*_e2e_tests.rs`).

**MPSC channel:** interleaving of several producers depends on the executor (section 1.10).

### 3.2 C Port Contract

Superseded by asupersync_v4_formal_semantics.md §1.11 (Scheduler lanes) and §3.0 (rules ENQUEUE, SCHEDULE-STEP), with §5 INV-SCHED-LANES; C status: see C_REFINEMENT_MAP.md rows "§1.11 lanes, §3.0 ENQUEUE, §5 INV-SCHED-LANES" and "§3.0 SCHEDULE-STEP". Summary: v4 models three lanes, Cancel > Timed > Ready, with the timed lane EDF and deterministic tie-breaking; replay determinism is `inv.determinism.replayable` (#46) and `def.determinism.seed_equivalence` (#47) (C_REFINEMENT_MAP.md rows of the same names).

The project plan's key `(lane_priority, logical_deadline, task_id, insertion_seq)` (`PLAN_TO_PORT_ASUPERSYNC_TO_ANSI_C.md` §6.8.3(H), line 471) is not what either engine implements:
- Rust lanes order entries by priority, then generation, then task id, and every `LabRuntime` pop picks among equal-priority entries with `rng_hint % count` (`docs/CHANNEL_TIMER_SEMANTICS.md` §3.3-3.5).
- C's lab dispatch ports that: entries `{slot, task_gen, priority, gen}`, one generation counter, a seeded xorshift64 pick among the top-priority group, a cancel lane and a ready lane and no timed lane (`src/runtime/lab_dispatch.c:43-48`, `:344-400`). Outside lab dispatch `src/runtime/scheduler.c` polls runnable tasks in arena order each round (`src/runtime/scheduler.c:1-22`).

**Timer tie-break:** C fires equal-deadline timers in insertion order: `timer_wheel.c` by (deadline, insertion_seq) (`src/time/timer_wheel.c:230-248`), the scheduler heap by (deadline, arm sequence) (`src/runtime/scheduler.c:100-105`), and lab dispatch by (1 ms tick, registration) (`src/runtime/scheduler.c:256-324`), the last matching the Rust wheel's level-0 behaviour for timers within 256 ms (comment at `src/runtime/scheduler.c:256-265`). This is not stronger than Rust: Rust's order is also deterministic (section 3.1).

### 3.3 Event Sequencing Contract

The field list this section used to require (`event_seq`, `logical_time`, `task_id`, `timer_id`, `channel_id`) matches neither engine:

- Rust `TraceEvent` (`src/trace/event.rs:1779-1796`): `version`, `seq` (monotonic), `time`, optional `logical_time`, `kind`, `data`.
- C `asx_trace_event` (`include/asx/runtime/trace.h:83-88`): `sequence` (monotonic per trace), `kind`, `entity_id` (the handle of the task, region, obligation, channel or timer), `aux` (kind-dependent payload). There is no time field.

Neither raw sequence is the parity key. The conformance runner compares the Foata-canonical form of the vocabulary-v2 trace, the end snapshot, the observations and the lab dispatch order with the Rust capture, canonical byte for byte (`tools/conformance/runner.c:1-20`; layering in `src/conformance/canon.c`), so events that commute may appear in either order.

### 3.4 Determinism Summary

| Aspect | Rust reference | C port |
|--------|---------------|--------|
| Same-deadline timer order | Deterministic: push order within a slot (section 3.1) | Insertion order (section 3.2) |
| Several producers on one channel | Executor order; seeded in `LabRuntime` | Commit order; seeded under lab dispatch |
| Trace ordering | `seq` per event; parity compares the canonical trace | `sequence` per event; same comparison |
| Cancellation result | Deterministic | Deterministic |
| PRNG streams | `DetRng` per seed | xorshift64 per seed, as `DetRng` (C_REFINEMENT_MAP.md row `def.determinism.seed_equivalence`) |
| Timer cascade ordering | Deterministic | No cascade |

---

## 4. Budget/Deadline Integration

### 4.1 Deadline Propagation Rule

```
effective_deadline = min(existing_deadline, new_deadline)
```

Superseded by asupersync_v4_formal_semantics.md §5 INV-DEADLINE-MONOTONE (children's deadlines are tighter or equal); C status: see C_REFINEMENT_MAP.md row "§5 INV-DEADLINE-MONOTONE". The combine rule itself is v4 §1.4, which the spec marks [Explanatory]: componentwise min, except priority, which takes the max. Rust `Budget::combine` / `meet` (`src/types/budget.rs:502`, `:556-570`, `:592-594`); C `asx_budget_meet` (`src/core/budget.c:49-58`). It applies to region and task budgets; timers carry no budget.

### 4.2 Budget-Timer Interaction

Superseded by asupersync_v4_formal_semantics.md §3.6 (rule TICK); C status: see C_REFINEMENT_MAP.md row "§3.6 TICK" (Variant). Summary: the spec cancels the region with `Timeout` when its deadline passes; Rust's lab and C cancel the task with `Deadline` through a per-task budget-deadline timer.

Rust details:
1. A task with a budget deadline gets a budget-deadline timer (`Cx::arm_budget_deadline`, `src/cx/cx.rs:3860`); when it fires, `BudgetDeadlineWake` requests a `Deadline` cancel stamped with the deadline (`src/cx/cx.rs:308-367`, reason built at `:357`). A checkpoint past the deadline also requests one (`src/cx/cx.rs:3124`).
2. A plain sleep does not shorten itself to the budget. The opt-in helper `budget_sleep` (`src/time/budget_ext.rs:50-77`) sleeps `min(requested, remaining)` and returns `Err(Elapsed)` when the deadline has already passed or cut the sleep short; `budget_timeout` does the same for a timeout (`src/time/budget_ext.rs:80-95`).

**C status:** under lab dispatch the scheduler arms a budget-deadline timer per task (`asx_lab_arm_budget_deadline_internal`, `src/runtime/scheduler.c:206-218`) whose firing requests a `DEADLINE` cancel stamped with the deadline (`lab_deadline_fire`, `src/runtime/scheduler.c:223-235`); without lab dispatch the scheduler checks the deadline before each poll (`sched_enforce_budget`, `src/runtime/scheduler.c:750-765`); `asx_checkpoint` also observes a passed deadline (`src/runtime/cancellation.c:527-539`). C has no `budget_sleep`/`budget_timeout` helper (`src/time/` reads no budget), and there is no `ASX_E_BUDGET_EXHAUSTED` status code: the earlier text that said an elapsed deadline returns it was wrong. Rust parity: `budget-deadline-sleep-checkpoint-001` (a task with deadline 100 ns sleeps 200 ns; the `Deadline` cancel does not cut the sleep short and the checkpoint after it acknowledges `Deadline`).

### 4.3 Timer-Cancellation Interaction

Rust: a `Sleep` completes early when its task has a cancel whose kind is neither `Timeout` nor `Deadline` and a checkpoint reports it (`src/time/sleep.rs:789-795`); its terminal cleanup cancels the registered timer (comment at `src/time/sleep.rs:786-788`). A `Timeout` or `Deadline` cancel does not cut the sleep short; the task observes it at a later checkpoint.

**C status:** implemented for sleeps: `sleep_observes_cancel` (`src/time/sleep.c:49-56`) applies the same kind rule, and a sleep that observes the cancel cancels its timer (`asx_task_cancel_timer`, `src/time/sleep.c:73-84`). A finished task's sleep timer and budget-deadline timer are removed with it (`asx_task_timer_disarm_internal`, `src/runtime/scheduler.c:165-180`). Rust parity: `budget-deadline-sleep-checkpoint-001`, `lab-dispatch-waker-rearm-001` (a pending sleep's timer is cancelled and registered again).

### 4.3a Interval Ticks

Rust's `Interval` (`src/time/interval.rs`) is a synchronous schedule the caller ticks with the current time. Its first tick is at its start: `interval(now, period)` starts now and `interval_at(start, period)` at `start` (`:443-471`). Each tick moves the deadline as its `MissedTickBehavior` says (`advance_deadline`, `:385-420`): `Burst`, the default, adds one period to the deadline, so a late caller gets the missed ticks back to back; `Delay` sets it a period after the tick's time; `Skip` moves it to the first period boundary after that time. Deadlines saturate at `Time::MAX`; the tick there is the last (`exhausted`).

**C status:** `asx_interval_poll` (`src/time/sleep.c:247`) is a task poll function over the same schedule: the first tick is at the first poll (`asx_interval_init`) or at a given time (`asx_interval_init_at`, `src/time/sleep.c:186`), `asx_interval_set_missed_tick_behavior` (`src/time/sleep.c:195`) picks Burst (default), Delay or Skip, and a poll counts every tick due at its time (`interval_take_due`, `src/time/sleep.c:215`), up to `max_ticks`. Rust has no async wrapper to compare with past the last representable tick: C returns `ASX_E_TIMER_DURATION_EXCEEDED` there instead of waiting forever. Unit tests in `tests/unit/time/test_sleep.c` (first tick, `interval_at`, the three behaviours at a 7 ms stall with a 2 ms period, `max_ticks` during a burst, saturation) match the tick counts and deadlines that Rust's `Interval` gives at the pinned revision for the same schedules (checked once with a scratch program against the pinned crate, 2026-10-10). No DSL op drives an interval, so there is no fixture (`bd-9kll.7.3`).

### 4.4 Exhaustion Behavior

| Resource | Rust | C |
|----------|------|---|
| Timer capacity | No limit (slab) | `ASX_MAX_TIMERS` live timers (default 128, `include/asx/time/timer_wheel.h:27-35`); then `ASX_E_RESOURCE_EXHAUSTED` from `asx_timer_register`. The earlier `max_timer_nodes` config field does not exist |
| Task timers | One per task | One heap entry per task slot (`g_timer_heap[ASX_MAX_TASKS]`, `src/runtime/scheduler.c:91`) |
| Timer duration | `TimerDurationExceeded` (try_register) or clamp | `ASX_E_TIMER_DURATION_EXCEEDED` |
| Channel capacity | `Full` (try) or wait | `ASX_E_CHANNEL_FULL` (try) or park with `ASX_E_PENDING` |
| Channel count / size | Unbounded | `ASX_MAX_CHANNELS` slots (default 16, `include/asx/core/channel.h:50-56`; `asx_channel_create` returns `ASX_E_RESOURCE_EXHAUSTED`, `src/channel/mpsc.c:419`); capacity at most `ASX_CHANNEL_MAX_CAPACITY` (64) |
| Waiter nodes | Unbounded queues | One pool of `ASX_WAIT_NODE_CAPACITY` nodes, `4 * ASX_MAX_TASKS` by default (`src/sync/wait_queue.h:88-90`); on exhaustion a waiter is not parked and is re-polled each round (`src/sync/wait_queue.h:44-46`) |

---

## 5. Reconciliation with Outcome/Exhaustion Semantics

### 5.1 Channel Errors and Outcome Lattice

Superseded for the lattice itself by asupersync_v4_formal_semantics.md §1.2 (`def.outcome.four_valued` #29, `def.outcome.severity_lattice` #30, `def.outcome.join_semantics` #31); C status: see C_REFINEMENT_MAP.md rows of the same names.

Channel code sets no task outcome in either engine: the error is returned to the task body, which decides. Rust `mpsc.rs` uses `Outcome` only as the return type of `SendPermit::send`; `src/channel/mpsc.c` has no outcome code. In C, a poll function that returns an error completes its task `Err`, unless a pending cancel dominates, in which case the task completes `Cancelled` (`src/runtime/scheduler.c:1118-1126`). So a task that returns `ASX_E_CANCELLED` from a cancelled `recv` ends Cancelled, as in `mpsc-recv-cancel-first-001`, and one that returns `ASX_E_DISCONNECTED` ends Err. `ASX_E_CHANNEL_FULL`, `ASX_E_WOULD_BLOCK` and `ASX_E_PENDING` are not terminal: the caller retries or waits.

### 5.2 Timer Errors and Outcome Lattice

| Timer condition | C status |
|------------|---------|
| `ASX_E_TIMER_DURATION_EXCEEDED` | Returned to the caller; terminal only if the task returns it (then Err) |
| `ASX_E_RESOURCE_EXHAUSTED` | Same |
| Budget deadline passed | A `Deadline` cancel (section 4.2), so the task ends Cancelled when it acknowledges it; there is no `ASX_E_BUDGET_EXHAUSTED` |

### 5.3 Exhaustion Semantics Contract

Per the plan's resource-contract engine (`PLAN_TO_PORT_ASUPERSYNC_TO_ANSI_C.md` "Risk 8: Resource-Exhaustion Undefined Behavior"):

1. **Failure-atomic:** a channel or timer operation that fails for exhaustion must leave its structures unchanged. C: `asx_timer_register` checks duration and slot availability before arming (`src/time/timer_wheel.c:145-164`); `channel_reserve_impl` returns the claimed lock-free capacity when no permit token is free (`src/channel/mpsc.c:581-608`).
2. **Deterministic error codes** for the same operation sequence and limits.
3. **No silent degradation:** exhaustion is reported with an explicit code, with one designed exception: wait-node pool exhaustion degrades a waiter to polling instead of failing (`src/sync/wait_queue.h:44-46`).

### 5.4 Channel/Timer Interaction with Region Close

Superseded for the close precondition by asupersync_v4_formal_semantics.md §3.3 CLOSE-COMPLETE and §3.4.5 (`rule.region.close_complete` #26, `inv.obligation.ledger_empty_on_close` #20); C status: see C_REFINEMENT_MAP.md rows `rule.region.close_complete` and `inv.obligation.ledger_empty_on_close`.

The earlier phase table (channels drained and permits aborted in `Finalizing`, timers cancelled, all channel and timer resources reclaimed at `Closed`) and its "quiescence invariant extension" do not hold in either engine:

- **Rust:** channel code does not refer to regions (`src/channel/mpsc.rs` mentions them only in a doc comment, `:649`). What ties a channel to close is the permit's `SendPermit` obligation, which is in the region's ledger: a region cannot close while it is `Reserved`, and the holder's completion leaks or aborts it. Timers belong to the tasks that sleep on them; a sleeping task keeps the region from closing, a timer by itself does not.
- **C:** a channel records its region and requires it to be open at creation (`src/channel/mpsc.c:388-390`); region close does not touch channels (the only channel call in `src/runtime/lifecycle.c` is `asx_channel_reset()` in runtime reset, `src/runtime/lifecycle.c:161`). An obligation-tracked permit left `Reserved` keeps the region in `FINALIZING` (`src/runtime/quiescence.c:256-257`) until its holder completes and leaks it. Task timers go away with their task (section 4.3). Channel slots are reclaimed only when both sides are closed (section 1.1).

---

## 6. Fixture Family Mapping

None of the candidate IDs below exists as `fixtures/rust_reference_v2/<id>.json` (checked 2026-10-10); each row says "not materialized" and names a v2 fixture that covers the behaviour where one exists (its scenario was read).

### 6.1 Channel Fixture Families

| Fixture Family ID | Description | Status |
|-------------------|-------------|--------|
| `ch-reserve-001` | Reserve, send, recv happy path | Not materialized; covered by `mpsc-two-phase-send-recv-001` |
| `ch-reserve-002` | Reserve, abort | Not materialized; no v2 scenario aborts a channel permit (C unit test `tests/unit/channel/test_mpsc.c` `abort_returns_capacity`) |
| `ch-reserve-003` | Reserve, cancel before send | Not materialized; no v2 fixture |
| `ch-backpressure-001` | Full channel, waiter queue ordering | Not materialized; one producer only in v2: `mpsc-two-phase-send-recv-001` (capacity 1) |
| `ch-backpressure-002` | Backpressure release on recv | Not materialized; covered by `mpsc-two-phase-send-recv-001` |
| `ch-disconnect-001` | Receiver drop, message discard | Not materialized; partly covered by `lab-dispatch-mpsc-disconnect-order-001` (send after the drop; no queued message, no parked sender) |
| `ch-disconnect-002` | Last sender drop, queue drain | Not materialized; covered by `mpsc-two-phase-send-recv-001`, `mpsc-try-ops-001` |
| `ch-cancel-001` | Cancel during pending reserve | Not materialized; no v2 fixture (C unit tests `tests/unit/channel/test_channel_wake.c` `cancelled_waiter_that_ends_passes_its_wake_on`, `cancel_pending_front_producer_keeps_its_place`) |
| `ch-cancel-002` | Cancel during recv | Not materialized; partly covered by `mpsc-recv-cancel-first-001` (empty channel, so message preservation is not exercised) |
| `ch-cancel-003` | Permit held, sender cancelled | Not materialized; no v2 fixture; C has no permit drop path |
| `ch-multi-sender-001` | Multiple senders, deterministic order | Not materialized; no v2 channel has more than one producer |
| `ch-exhaustion-001` | Resource limit hit on channel | Not materialized; no v2 fixture |
| `ch-linearity-001` | Permit resolved exactly once | Not materialized; no v2 fixture |

### 6.2 Timer Fixture Families

| Fixture Family ID | Description | Status |
|-------------------|-------------|--------|
| `tm-insert-fire-001` | Register, advance, fire | Not materialized; covered for task sleeps by `timers-same-deadline-001`, `quiescence-pending-timer-001` |
| `tm-cancel-001` | Register, cancel, advance | Not materialized; nearest is `lab-dispatch-waker-rearm-001` (a sleep timer cancelled and registered again) |
| `tm-cancel-002` | Stale handle cancel | Not materialized; no v2 fixture (C unit test `timer_stale_handle_cancel_returns_false`) |
| `tm-cancel-003` | Cancel after fire | Not materialized; no v2 fixture |
| `tm-tiebreak-001` | Same-deadline timers, deterministic order | Not materialized; covered by `timers-same-deadline-001`, `lab-dispatch-timer-wheel-order-001` |
| `tm-cascade-001` | Placement across levels | Not materialized; no cascade in C |
| `tm-overflow-001` | Beyond max_wheel_duration | Not materialized; no overflow heap in C |
| `tm-skip-001` | Large time jump | Not materialized; no v2 fixture |
| `tm-update-001` | Update timer deadline | Not materialized; no v2 fixture (C unit test `timer_update_cancels_old_and_registers_new`) |
| `tm-exhaustion-001` | Timer pool limit | Not materialized; no v2 fixture (C unit test `timer_resource_exhaustion`) |
| `tm-budget-001` | Budget deadline triggers cancellation | Not materialized; covered by `budget-deadline-sleep-checkpoint-001` |
| `tm-duration-exceeded-001` | Register beyond max_timer_duration | Not materialized; no v2 fixture (C unit test `timer_duration_exceeded`) |
| `tm-generation-wrap-001` | Generation wrapping | Not materialized; no v2 fixture |

### 6.3 Integration Fixture Families

| Fixture Family ID | Description | Status |
|-------------------|-------------|--------|
| `int-channel-cancel-001` | Channel send cancelled by timeout | Not materialized; no v2 fixture |
| `int-timer-region-close-001` | Region close with active timers | Not materialized; the premise (timers cancelled at finalization) does not hold (section 5.4) |
| `int-channel-region-close-001` | Region close with active channel | Not materialized; the premise (permits aborted, queue drained at close) does not hold (section 5.4) |
| `int-replay-channel-001` | Channel replay digest | Not materialized; every v2 channel fixture is replayed and compared (`mpsc-two-phase-send-recv-001`, `mpsc-try-ops-001`, `mpsc-recv-cancel-first-001`, `lab-dispatch-mpsc-disconnect-order-001`) |
| `int-replay-timer-001` | Timer replay digest | Not materialized; same for `timers-same-deadline-001`, `lab-dispatch-timer-wheel-order-001` |

---

## 7. C Port Implications

### 7.1 MPSC Channel C Design Notes

1. **Bounded ring buffer:** `queue[ASX_CHANNEL_MAX_CAPACITY]` with `queue_head`/`queue_len` (`src/channel/mpsc.c:73-97`); capacity fixed at creation. POSIX/PARALLEL builds use a lock-free committed-message queue instead (`src/channel/mpsc.c:45-71`).
2. **Reserved count:** an atomic counter plus a permit-token table (`src/channel/mpsc.c:84-87`).
3. **Waiter queue:** nodes from the shared wait-node pool, FIFO by arrival (`src/sync/wait_queue.h:14-27`); there are no waiter ids.
4. **No weak references:** handles carry a generation checked against the slot (`src/channel/mpsc.c:106-124`).
5. **Cleanup-stack integration:** not implemented. Permits are not registered on any cleanup stack (section 1.3, RAII).
6. **Lock discipline:** C channel code takes no lock; queue code is scheduler-thread state (`src/sync/wait_queue.h:66-69`).

### 7.2 Timer Wheel C Design Notes

1. **Fixed-size arena:** `ASX_MAX_TIMERS` slots (`include/asx/time/timer_wheel.h:27-35`); there is no `max_timer_nodes` config field.
2. **Generation-safe handles:** `{ slot, generation }`, both `uint32_t`, looked up by direct slot index, not a hash map (`src/time/timer_wheel.c:174-192`).
3. **Deterministic tie-break:** (deadline, insertion_seq) sort at collection (`src/time/timer_wheel.c:230-248`).
4. **Occupied bitmap:** not implemented (flat scan).
5. **Virtual clock:** the runtime's default clock hooks read a virtual counter that only moves when advanced (`src/runtime/hooks.c:238-260`); the scheduler jumps it to the next timer when every task is parked (`src/runtime/scheduler.c:11-19`).
6. **Overflow handling:** not implemented.

### 7.3 Determinism Strengthening

The earlier claim that C is stronger than Rust on determinism, and that conformance fixtures test a deterministic C ordering that is a subset of Rust's, is out of date:

| Aspect | Rust | C port |
|--------|------|--------|
| Same-deadline timer order | Deterministic (section 3.1) | Insertion order |
| Channel multi-producer order | Executor order; seeded in `LabRuntime` | Seeded under lab dispatch |
| Event journal | Sequence-numbered | Sequence-numbered |

The conformance runner compares C with the exact Rust capture, including the lab dispatch order (`tools/conformance/runner.c:7-11`), so a C ordering that Rust would also allow but did not produce fails. Under lab dispatch C reproduces Rust's `LabRuntime` order; it does not impose its own.

### 7.4 Invalid Handle Behavior

| Operation | Invalid handle | Stale handle (generation mismatch) |
|-----------|---------------|-------------------------------------|
| Timer cancel | 0 (`src/time/timer_wheel.c:178-184`) | 0 |
| Timer update | NULL old handle: plain register; otherwise `ASX_E_STALE_HANDLE`, nothing registered (`src/time/timer_wheel.c:279-283`) | `ASX_E_STALE_HANDLE` |
| Permit send | Consumed permit: `ASX_E_INVALID_STATE`; forged or stale token: refused (`src/channel/mpsc.c:647-659`) | Channel handle: `ASX_E_STALE_HANDLE` (`src/channel/mpsc.c:121`) |
| Permit abort | Consumed or unknown permit: no-op (`src/channel/mpsc.c:704-711`) | No-op |

**Ghost monitor coverage:** the ghost monitors in `include/asx/core/ghost.h` cover region, task and obligation transitions, obligation linearity, borrows and determinism; none covers timer handles, permits or channel state. The list this section gave (double timer cancel, permit reuse, operations on a closed channel) is not implemented; those cases are plain return codes.

**Queue position of a cancel-requested waiter:** Rust keeps a cancel-requested or masked waiter in the reserve queue until its `Reserve` is polled or dropped (`src/channel/mpsc.rs:363-377`), so `try_reserve` by another producer returns `Full` meanwhile and a freed slot wakes that waiter, not the one behind it. C does the same since 2026-10-10 (bd-wy6n): `asx_wait_queue_live_ahead` counts every waiter whose task has not finished (`src/sync/wait_queue.c:344`), `asx_wait_queue_settle` lets a cancel-pending waiter absorb its unit (`:309`; contract `src/sync/wait_queue.h:32-40`), and the waiter passes it on when its next poll observes the cancel (`channel_wait_cancelled`) or, if its task ends without that poll, when the task finishes (`asx_wait_task_finished`, `src/sync/wait_queue.c:245`, reaping the queue through `channel_reap_queue`, `src/channel/mpsc.c:372`), as dropping a Rust `Reserve` does. Before, C skipped such a waiter: it woke it but gave the unit to the next live producer as well. Unit test: `tests/unit/channel/test_channel_wake.c` `cancel_pending_front_producer_keeps_its_place`. No v2 fixture has two producers on one channel.

### 7.5 Wake-Driven Waiting (C Port)

The C API stays poll-style (`try_*` / `poll_*` calls that report "not
yet"), but waiting is wake-driven, mirroring Rust's waker registration
(`include/asx/core/channel.h:16-28`, `src/sync/wait_queue.h:1-69`):

| Rule | C realization |
|------|---------------|
| Waker registration | A "not yet" result returned while `asx_task_current() != ASX_INVALID_ID` enqueues the current task in the primitive's FIFO wait queue and parks it (`asx_task_park`). The waiting channel calls return `ASX_E_PENDING`; the scheduler does not poll the task again until it is woken. The `try_*` calls never queue or park, inside or outside a poll, as Rust's `try_*` register no waker. |
| No registration outside a poll | Callers outside a scheduler poll are never queued or parked. |
| Wake on state change | A commit wakes one parked receiver per committed message; a dequeue or abort wakes the head of the producer line; closing either side wakes every waiter (`src/channel/mpsc.c:358-371`). |
| FIFO + no queue jumping | Waiters are served in arrival order. As in Rust (`try_reserve`, `src/channel/mpsc.rs:732-754`), `asx_channel_try_reserve` reports `ASX_E_CHANNEL_FULL` while a producer whose task has not finished is parked ahead of the caller, even with free capacity; a cancel-requested producer counts, as in Rust (section 7.4). |
| Drop of a wait future | `asx_channel_wait_cancel(id, task)` (`include/asx/core/channel.h:201`) withdraws a task that stops waiting (select branch lost, timeout); a wake it held passes to the next waiter. A task that ends while still queued is withdrawn the same way when it finishes (`asx_wait_task_finished`, called from task completion), so a cancelled waiter that never polls again needs no explicit call either (`src/sync/wait_queue.h:144-150`). |
| Queue capacity | A queue has no limit of its own (Rust's waiter queues are unbounded): waiters are nodes in the runtime's shared pool (`ASX_WAIT_NODE_CAPACITY`, four per task slot by default, `src/sync/wait_queue.h:88-90`). Only if the pool is exhausted, after the nodes of dead tasks are reclaimed, is a waiter not parked: it yields and is re-polled each round (degraded to polling, never a lost wakeup). |
| Determinism | Queue order and wake order depend only on the call sequence (`src/sync/wait_queue.h:63-64`). Runnable tasks are then polled in arena order by `asx_scheduler_run`, or in the seeded lab order under lab dispatch. With every task parked and no wake source, `asx_scheduler_run` returns `ASX_E_WOULD_BLOCK` instead of spinning to budget exhaustion (`src/runtime/scheduler.c:11-19`). |
| Threading | Wait queues are scheduler-thread state (like `asx_task_wake`). Foreign-thread producers of the lock-free backend only reach queue code when a scheduler task is parked on that channel and must not race with it (`src/sync/wait_queue.h:66-69`). |

The same contract covers oneshot/broadcast/watch/session receives (send or
drop wakes every parked receiver task; `asx_watch_poll_changed`,
`include/asx/core/watch.h:124`, is the wake-driven form of watch
`changed()`), and the sync primitives in `src/sync/`. Their policies follow
their Rust counterparts and are not all plain arrival order: the semaphore
serves the front of its line, a release waking the front waiter only and
each acquire waking the next, and cancel-pending waiters keep their place
(`src/sync/semaphore.c:9-20`); the mutex hands the lock to the front waiter
whatever its cancel state (`src/sync/semaphore.c:22-30`); the rwlock follows
Rust's writer-preference grant policy with bounded reader admission
(`src/sync/rwlock.c:1-26`); notify, barrier trip, async
`asx_once_get_or_init` (`include/asx/sync/once.h:70`) and pool checkout,
with the same no-queue-jumping window for the pool (`src/sync/pool.c:166`).
The shared mechanism lives in `src/sync/wait_queue.h` and
`src/sync/wait_queue.c`. Rust parity: `sync-waiters-unbounded-001` (34
waiters on one mutex, one semaphore and one barrier).
