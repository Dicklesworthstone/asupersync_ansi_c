# Channel and Timer Determinism Contract (Canonical Extraction)

> **Bead:** bd-296.17  
> **Scope:** Deterministic MPSC reserve/send/abort behavior, timer ordering, cancellation handles, and tie-break contract  
> **Status:** Canonical extraction slice (implementation-ready for downstream beads)  
> **Rust baseline commit:** `38c152405bd03e2bd9eecf178bfbbe9472fed861`  
> **Rust toolchain snapshot:** `rustc 1.95.0-nightly (7f99507f57e6c4aa0dce3daf6a13cca8cd4dd312)`  
> **Last verified:** 2026-02-27 by BeigeOtter

This file is the Phase 1 extraction artifact for `bd-296.17`. It captures channel/timer determinism and tie-break behavior required by downstream beads (`bd-296.18`, `bd-296.19`, `bd-1md.13`, `bd-1md.14`, `bd-1md.15`) and by the C runtime kernel phases.

## 1. Source Provenance

Primary Rust sources used:

- `/data/projects/asupersync/src/channel/mpsc.rs`
- `/data/projects/asupersync/src/time/wheel.rs`
- `/data/projects/asupersync/src/runtime/timer.rs`

Cross-check references:

- `/data/projects/asupersync_ansi_c/PLAN_TO_PORT_ASUPERSYNC_TO_ANSI_C.md` (deterministic tie-break and phase contracts)
- `/data/projects/asupersync_ansi_c/docs/LIFECYCLE_TRANSITION_TABLES.md` (lifecycle and quiescence coupling)
- `/data/projects/asupersync_ansi_c/docs/EXISTING_ASUPERSYNC_STRUCTURE.md` (budget/exhaustion coupling from `bd-296.16`)

## 2. Deterministic Channel Semantics (MPSC Two-Phase)

### 2.1 Core State and Capacity Invariant

`mpsc.rs` models capacity with:

- `queue: VecDeque<T>` (committed messages),
- `reserved: usize` (outstanding permits),
- `send_wakers: VecDeque<SendWaiter>` (producer wait queue),
- `capacity: usize` (fixed channel capacity).

Canonical invariant:

`used_slots = queue.len() + reserved` and `used_slots <= capacity`

Evidence:

- `used_slots` and `has_capacity` definitions: `mpsc.rs:141-147`
- reserve/abort/send paths mutate `reserved` symmetrically: `mpsc.rs:227-233`, `mpsc.rs:547-551`, `mpsc.rs:587-592`, `mpsc.rs:607-611`
- regression tests: `capacity_invariant_across_reserve_send_abort` and eviction tests:
  - `mpsc.rs:1407`
  - `mpsc.rs:1266`
  - `mpsc.rs:1301`

### 2.2 Two-Phase Obligation Protocol

Channel send path is explicitly two-phase:

1. `reserve` acquires one capacity slot (`SendPermit` obligation).
2. Obligation must resolve exactly once through:
   - `permit.send(value)` or `permit.try_send(value)` (commit),
   - `permit.abort()` (abort),
   - drop of unresolved permit (RAII abort).

Evidence:

- API and reserve future: `mpsc.rs:194-200`, `mpsc.rs:326-410`
- `SendPermit` commit/abort/drop: `mpsc.rs:526-620`
- contract tests:
  - `two_phase_send_recv` (`mpsc.rs:927`)
  - `permit_abort_releases_slot` (`mpsc.rs:944`)
  - `dropped_permit_releases_capacity` (`mpsc.rs:1153`)

### 2.3 FIFO Waiter Discipline and Queue-Jump Prevention

Deterministic sender fairness rule:

- Async `reserve` waiters are queued FIFO in `send_wakers`.
- A reserve poll can claim capacity only if it is queue head (`is_first`) and capacity exists.
- `try_reserve` must return `Full` if any waiter exists, even when raw capacity is available.

Evidence:

- waiter gating and FIFO head check: `mpsc.rs:348-354`
- waiter enqueue/update by monotonic waiter id: `mpsc.rs:398-405`
- queue-jump prevention in `try_reserve`: `mpsc.rs:223-225`
- explicit fairness test: `try_reserve_respects_fifo_over_capacity` (`mpsc.rs:1442`)

### 2.4 Cancellation and Disconnect Behavior

Reserve path:

- Cancellation checkpoint failure returns `SendError::Cancelled(())`.
- Receiver drop returns `SendError::Disconnected(())`.
- Cancelled/dropped pending waiters are removed from waiter queue (no phantom blockers).

Receiver path:

- `recv` cancellation returns `RecvError::Cancelled` without consuming queued message.
- `sender_count == 0` yields `RecvError::Disconnected`.

Evidence:

- reserve cancellation/disconnect: `mpsc.rs:336-346`
- waiter cleanup on drop/cancel: `mpsc.rs:413-440`, `mpsc.rs:1499`, `mpsc.rs:1546`
- receiver cancellation semantics: `mpsc.rs:647-700`, `mpsc.rs:1130`

### 2.5 Backpressure and Drop-Oldest Policy

`send_evict_oldest` deterministic behavior:

- If capacity exists: enqueue directly (`Ok(None)`).
- If full with committed queue entries: evict oldest committed entry (`pop_front`), then enqueue (`Ok(Some(evicted)))`).
- If full due only to reserved permits: cannot evict reserved slots; return `Err(Full(value))`.

Evidence:

- implementation: `mpsc.rs:276-314`
- reserved-only full rejection: `mpsc.rs:1266`
- committed-not-reserved eviction: `mpsc.rs:1301`

## 3. Deterministic Timer Semantics

Two timer structures are relevant in baseline Rust:

- `runtime::timer::TimerHeap` (`src/runtime/timer.rs`) for deadline-ordered task wakeups.
- `time::wheel::TimerWheel` (`src/time/wheel.rs`) for hierarchical timer management, overflow, and optional coalescing.

### 3.1 TimerHeap Equal-Deadline Tie-Break Contract

`TimerHeap` ordering:

- primary key: earliest deadline first,
- tie-break key: insertion generation (earlier insertions pop first for equal deadlines).

Evidence:

- comparator uses `(deadline, generation)` in min-heap orientation: `runtime/timer.rs:17-24`
- generation increments on insertion: `runtime/timer.rs:60-67`
- deterministic equal-deadline test: `same_deadline_pops_in_insertion_order` (`runtime/timer.rs:183`)

### 3.2 TimerWheel Registration, Cancellation, and Expiry

Registration:

- `try_register` validates `deadline - current_time <= max_timer_duration`.
- each timer receives `(id, generation)`; active timers tracked in `active` map.

Cancellation:

- `cancel(handle)` succeeds only when both id and generation match active entry.
- generation mismatch is explicitly rejected without removing live timer.

Expiry:

- `collect_expired(now)` advances logical wheel ticks to `now` and drains ready entries.
- exact deadline boundary is inclusive (`deadline <= now` fires).

Evidence:

- register/validate/handle creation: `wheel.rs:457-490`
- cancel path: `wheel.rs:502-513`
- generation mismatch test: `wheel_cancel_rejects_generation_mismatch_without_removing` (`wheel.rs:1099`)
- expiry collection and boundary checks: `wheel.rs:570-579`, `wheel.rs:742-743`, `wheel.rs:863-868`

### 3.3 Overflow and Coalescing

Overflow:

- timers beyond wheel direct range enter overflow min-heap keyed by deadline.
- overflow entries are promoted back into wheel when within direct range.

Coalescing:

- optional window-based coalescing can fire timers at a coalesced boundary.
- coalescing activates only when in-window ready count reaches `min_group_size`.

Evidence:

- overflow ordering and insertion: `wheel.rs:255-259`, `wheel.rs:592-597`, `wheel.rs:622-625`
- overflow refill/promotion: `wheel.rs:750-763`, `wheel.rs:807-810`
- coalescing boundary and activation: `wheel.rs:822-853`

### 3.4 Reconciliation with Outcome/Budget/Exhaustion Semantics (`bd-296.16`)

Channel/timer semantics must plug into the canonical outcome/budget model without introducing hidden behavior forks:

- Channel capacity pressure is explicit (`Full`) rather than silent drop, matching deterministic exhaustion requirements.
- `reserve`/`recv` cancellation paths preserve explicit cancellation outcomes (`SendError::Cancelled`, `RecvError::Cancelled`) and avoid partial mutation.
- Timer deadline semantics are boundary-inclusive (`deadline <= now`) and feed deterministic deadline-driven cancellation paths.
- Timer duration admission (`TimerDurationExceeded`) is an explicit deterministic rejection surface, not implicit truncation.

Cross-artifact coupling:

- `docs/EXISTING_ASUPERSYNC_STRUCTURE.md` defines budget exhaustion mapping (`deadline`, `poll_quota`, `cost_budget`) and cancellation reason strengthening.
- `docs/LIFECYCLE_TRANSITION_TABLES.md` defines close/quiescence requirements that include drained timers/channels and deterministic cancellation propagation.

This bead therefore fixes channel/timer kernel mechanics as deterministic inputs to those already-extracted outcome/budget/finalization semantics.

## 4. Tie-Break Contract for ANSI C Port

The C port must preserve deterministic timer/channel behavior across profiles and codecs.

Required tie-break and ordering rules:

1. Channel sender waiters are FIFO; non-blocking reserve must not queue-jump.
2. Two-phase obligations are linear: every reserve resolves exactly once as commit or abort.
3. Receiver delivery order is FIFO for committed queue entries.
4. Equal-deadline timer ordering must be deterministic and insertion-stable.
5. Timer cancellation must be generation-safe (stale handles fail, live handles remain valid).
6. Coalescing must be deterministic for a fixed logical-time input stream.

Normative cross-check from project plan:

- scheduler tie-break key: `(lane_priority, logical_deadline, task_id, insertion_seq)`
- deterministic timer ordering for equal deadlines by insertion sequence
- no nondeterministic scheduler path in deterministic mode

Reference: `PLAN_TO_PORT_ASUPERSYNC_TO_ANSI_C.md` section "Preserving Determinism (Not Just Best Effort)".

## 5. Fixture Candidate Mapping

Proposed canonical fixture IDs for conformance/invariant layers:

| Candidate ID | Rule | Expected result class |
|---|---|---|
| `channel-two-phase-001` | reserve -> send -> recv | value delivered, no leaked reservation |
| `channel-abort-release-002` | reserve -> abort | capacity restored deterministically |
| `channel-drop-permit-abort-003` | reserve -> drop permit | equivalent to abort; no leaked reservation |
| `channel-fifo-waiter-004` | queued waiter + try_reserve | try_reserve returns Full; waiter acquires first |
| `channel-reserve-cancel-005` | pending reserve then cancel | waiter removed; no phantom queue entry |
| `channel-recv-cancel-nonconsume-006` | recv cancelled with queued value | cancellation returns error; value remains |
| `channel-evict-reserved-007` | send_evict_oldest with all slots reserved | Full, no reserved-slot eviction |
| `channel-evict-committed-008` | send_evict_oldest with committed oldest | oldest committed value evicted |
| `timer-equal-deadline-order-010` | same-deadline inserts | insertion order preserved |
| `timer-cancel-generation-011` | stale generation cancel | stale rejected; live handle cancellable |
| `timer-next-deadline-same-tick-012` | same tick sub-ms deadline | actual deadline preserved by next_deadline |
| `timer-overflow-promotion-013` | out-of-range timer | promoted and fired when in range |
| `timer-coalescing-threshold-014` | coalescing min_group_size gate | coalescing only when threshold met |

## 6. Implementation Checklist for C

Before closing `bd-296.17`, C design and tests should prove:

1. `queue_len + reserved <= capacity` for all channel transitions.
2. FIFO waiter queue and queue-jump prevention (`try_reserve` behavior) are preserved.
3. Permit drop is semantically identical to explicit abort.
4. Receiver cancellation does not consume committed queue data.
5. Timer handles are generation-safe and stale cancels are rejected.
6. Equal-deadline timer tie-break is insertion-stable and replay-deterministic.
7. Overflow/coalescing decisions do not violate deterministic replay identity.
