# Lock-Free Queue and Shadow-Path Equivalence Evaluation

> **Bead:** bd-pweu.5
> **Status:** Production promotion complete
> **Last updated:** 2026-05-08 by SilverFrog

## 1. Executive Summary

A Vyukov-style bounded MPSC lock-free ring buffer was spiked and then
graduated into the production channel path without changing the public
two-phase contract. The shipped MPSC API still requires
`reserve -> send/abort`; the POSIX/PARALLEL live profile now uses atomic
capacity claims, atomic permit tokens, and an atomic committed-message
ring behind that API.

**Decision: PROMOTE with semantic guardrails.** CORE remains the
deterministic single-thread ring for replay-stable behavior. Live
POSIX/PARALLEL builds select the atomic backend with
`ASX_LOCKFREE_SINGLE_THREAD=0`, preserving FIFO receive, bounded
capacity, abort capacity release, disconnect behavior, and stale-permit
rejection.

## 2. Spike Design

### 2.1 Algorithm

Production two-phase atomic MPSC:
- `in_use` atomically claims capacity before a permit is issued
- `permit_tokens[]` are claimed and consumed with CAS
- abort consumes the permit token and releases the claimed capacity
- send consumes the permit token and publishes to the committed-message ring
- consumer load/recycle follows the single-consumer MPSC sequence protocol

### 2.2 Implementation

| File | Purpose |
|------|---------|
| `src/channel/mpsc.c` | Production two-phase MPSC with CORE ring and POSIX/PARALLEL atomic backend |
| `src/channel/mpsc_lockfree_spike.c` | Superseded direct-enqueue reference spike using shared atomics |
| `include/asx/platform/atomics.h` | Shared portable `u32` atomic/fence shim for spike surfaces |
| `tests/unit/channel/test_mpsc.c` | Unit and contention-style coverage for two-phase channel semantics |
| `tests/unit/channel/test_mpsc_equivalence.c` | 10 tests validating baseline/atomic semantic equivalence |

### 2.3 Atomic Abstraction

Single-threaded mode: atomics degrade to plain loads/stores.
Multi-threaded mode maps through the shared header to `__atomic_*`
builtins (GCC/Clang), `_Interlocked*` (MSVC), or C11 `_Atomic`.

```c
typedef struct { uint32_t value; } asx_atomic_u32;
asx_atomic_u32_load(a)            → backend-specific acquire load
asx_atomic_u32_store(a, v)        → backend-specific release store
asx_atomic_u32_cas(a, exp, des)   → CAS with return code
```

## 3. Equivalence Evidence

### 3.1 Shared Fixture Results

The equivalence suite now includes 10 tests confirming that the atomic
shadow path produces identical observable behavior to the baseline for:

| Test | What it proves |
|------|----------------|
| `fifo_ordering_equivalence` | Identical FIFO order for 16 messages |
| `capacity_limit_equivalence` | Same capacity enforcement (full = ASX_E_CHANNEL_FULL) |
| `empty_dequeue_equivalence` | Same empty behavior (ASX_E_WOULD_BLOCK) |
| `wraparound_equivalence` | Identical after 20 fill/drain cycles (80 messages) |
| `interleaved_equivalence` | Same behavior under mixed send/recv patterns |
| `two_phase_scripted_equivalence` | Same reserve/send/abort, close, queue length, and reserved count outcomes |
| `power_of_2_capacity` | Capacity rounding validated (1,3,5,16,33,100) |
| `throughput_comparison` | Both produce valid results at scale (128K ops) |
| `two_phase_promotion_analysis` | Production guardrails documented |
| `memory_overhead_comparison` | Memory footprint within bounds |

### 3.2 Throughput Comparison

| Metric | Baseline MPSC | Lock-Free Spike | Ratio |
|--------|-------------|-----------------|-------|
| Cycles/op | ~361 | ~49 | 0.14x |
| Ops measured | 128,000 | 128,000 | — |
| Mode | reserve+send+recv | enqueue+dequeue | — |

**Why spike is faster:**
1. 1 op/message vs 2 (no reserve/send split)
2. No token tracking (baseline scans O(capacity) permit array)
3. No handle validation per operation
4. No generation-safe lookup per operation

## 4. Risk Profile

### 4.1 What lock-free gains

| Benefit | Impact | Confidence |
|---------|--------|------------|
| Higher raw throughput | ~7x in microbenchmark | HIGH |
| Better scaling under contention | Expected but unmeasured | MEDIUM |
| Lower per-operation overhead | Removes token tracking | HIGH |
| Simpler hot path | 1 CAS vs 2 function calls | HIGH |

### 4.2 Guardrails that remain mandatory

| Risk | Severity | Mitigation |
|------|----------|------------|
| Cancel-safety drift | **CRITICAL** | Capacity is claimed before reserve and released by abort/disconnect; tests cover mixed send/abort |
| Semantic fork by profile | **HIGH** | CORE and atomic paths share the public API and are compared by scripted equivalence |
| Producer contention | MEDIUM | Atomic CAS loops claim capacity, tokens, and enqueue slots; pthread stress is available behind `ASX_MPSC_PTHREAD_STRESS` |
| Sequence number wraparound | LOW | Bounded `uint32_t` sequence model remains documented; long-run fuzz/benchmark gates should continue to watch it |
| Debug complexity | MEDIUM | Queue length/reserved count remain observable through existing query APIs |

### 4.3 Cancel-Safety Resolution

The production path does not expose a direct enqueue API. Reservation
claims `in_use` capacity first, then records a permit token. Abort
consumes the token and releases `in_use`; send consumes the token and
only then publishes a committed message. If the receiver is closed after
reserve, send returns `ASX_E_DISCONNECTED` and releases the claimed
capacity without enqueuing a value.

## 5. Decision

### 5.1 Production Selection

| Build | Backend | Purpose |
|------|---------|---------|
| CORE/default | deterministic ring | replay-stable canonical semantics |
| POSIX with `ASX_LOCKFREE_SINGLE_THREAD=0` | atomic committed-message ring | live profile producer contention |
| PARALLEL with `ASX_LOCKFREE_SINGLE_THREAD=0` | atomic committed-message ring | large-swarm runtime path |

### 5.2 Recommendation

Keep this profile split until `bd-pweu.11` lands single-vs-multi-worker
digest parity. After that, platform adapters can execute channel traffic
concurrently by default only if the parity gate remains green.

This preserves:
- Cancel-safety (two-phase protocol unchanged)
- ABI stability (same public API)
- Semantic equivalence (same FIFO + capacity invariants)
- Safe fallback (single-threaded mode always available)

## 6. Safe Fallback Status

The public MPSC channel remains the first-class implementation:
- CORE unit path: `test_mpsc` covers 51 default channel tests
- Atomic live path: the same unit test passes under `PROFILE=PARALLEL`
  with `ASX_LOCKFREE_SINGLE_THREAD=0`
- Optional pthread stress: `ASX_MPSC_PTHREAD_STRESS=1` adds a
  multi-producer/single-consumer contention test
- Equivalence path: `test_mpsc_equivalence` covers 10 baseline/atomic
  scripted parity checks
- Capacity invariant remains `queue_len + reserved_count <= capacity`
  from the public observer perspective

## 7. Artifacts

| Artifact | Purpose |
|----------|---------|
| `src/channel/mpsc.c` | Production two-phase atomic backend selection |
| `src/channel/mpsc_lockfree_spike.c` | Superseded direct-enqueue reference spike |
| `tests/unit/channel/test_mpsc.c` | Unit + scripted/optional pthread stress coverage |
| `tests/unit/channel/test_mpsc_equivalence.c` | 10-test equivalence suite |
| `docs/LOCKFREE_QUEUE_EVALUATION.md` | This evaluation document |
