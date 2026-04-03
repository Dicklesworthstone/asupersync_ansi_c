# Lock-Free Queue and Shadow-Path Equivalence Evaluation

> **Bead:** bd-3vt.2
> **Status:** Research complete — DEFER recommendation
> **Last updated:** 2026-03-01 by NobleCanyon

## 1. Executive Summary

A Vyukov-style bounded MPSC lock-free ring buffer was spiked and
evaluated against the baseline two-phase MPSC channel. The spike
demonstrates ~7x higher raw throughput in single-threaded mode but
**lacks cancel-safety** (no abort path), which is a non-negotiable
requirement for the asx runtime.

**Decision: DEFER lock-free upgrade.** When parallelism is added
(GS-009/GS-010), extend the baseline channel with atomic wrappers
rather than replacing the queue architecture.

## 2. Spike Design

### 2.1 Algorithm

Vyukov bounded MPSC ring buffer:
- Per-cell sequence numbers coordinate producers and consumer
- Producers: CAS on enqueue_pos, then write value + advance sequence
- Consumer: load sequence, read value, recycle slot
- Power-of-2 capacity (required for mask-based indexing)

### 2.2 Implementation

| File | Purpose |
|------|---------|
| `src/channel/mpsc_lockfree_spike.c` | Standalone spike using shared `include/asx/platform/atomics.h` |
| `include/asx/platform/atomics.h` | Shared portable `u32` atomic/fence shim for spike surfaces |
| `tests/unit/channel/test_mpsc_equivalence.c` | 9 tests validating semantic equivalence |

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

All 9 equivalence tests pass, confirming that the spike produces
identical observable behavior to the baseline for:

| Test | What it proves |
|------|----------------|
| `fifo_ordering_equivalence` | Identical FIFO order for 16 messages |
| `capacity_limit_equivalence` | Same capacity enforcement (full = ASX_E_CHANNEL_FULL) |
| `empty_dequeue_equivalence` | Same empty behavior (ASX_E_WOULD_BLOCK) |
| `wraparound_equivalence` | Identical after 20 fill/drain cycles (80 messages) |
| `interleaved_equivalence` | Same behavior under mixed send/recv patterns |
| `power_of_2_capacity` | Capacity rounding validated (1,3,5,16,33,100) |
| `throughput_comparison` | Both produce valid results at scale (128K ops) |
| `two_phase_tradeoff` | Architectural analysis documented |
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

### 4.2 What lock-free loses

| Risk | Severity | Mitigation |
|------|----------|------------|
| No cancel-safety (no abort path) | **CRITICAL** | Would need CAS-loop rollback protocol |
| Power-of-2 capacity requirement | LOW | Acceptable constraint |
| No generation-safe handles | MEDIUM | Would need separate handle layer |
| Sequence number wraparound | LOW | 2^32 messages before wrap |
| More complex debugging | MEDIUM | Sequence numbers obscure state |
| ABI breakage | HIGH | Different internal layout |

### 4.3 Cancel-Safety Gap (Critical)

The asx runtime requires cancel-safety: a task may be cancelled between
`try_reserve` and `send`, requiring the permit to be aborted and
capacity returned. The lock-free spike has no abort path.

A lock-free two-phase protocol is theoretically possible:
1. CAS to claim a cell (reserve)
2. Write value + advance sequence (send)
3. Reset sequence to reclaim cell (abort)

This adds significant complexity:
- Race between abort and consumer reading the cell
- Need for a "reserved but not committed" state in the sequence
- Consumer must skip uncommitted cells or wait
- Abort under contention requires retry loops

**Estimated additional complexity: ~200 LOC + 15 additional tests.**

## 5. Decision

### 5.1 Expected Value Analysis

| Path | Benefit | Cost | Risk | EV |
|------|---------|------|------|----|
| Adopt lock-free now | ~7x throughput | 200 LOC + ABI break + cancel-safety protocol | HIGH | **Negative** — cancel-safety risk too high for current phase |
| Defer to GS-009/GS-010 | Same throughput gain later | Deferred cost | LOW | **Positive** — aligns with architecture plan |
| Atomic-wrap baseline | ~3-5x throughput (estimated) | ~100 LOC atomic wrappers | LOW | **Positive** — preserves cancel-safety |

### 5.2 Recommendation

**DEFER.** The optimal path when parallelism is needed:

1. Add `asx_atomic_u32` / `asx_atomic_u64` abstraction layer (portable)
2. Wrap baseline `queue_head`, `queue_len`, `reserved` with atomics
3. Protect `permit_tokens[]` with per-channel spinlock (low contention)
4. Benchmark under realistic contention (not just single-threaded)
5. If hot path is still bottleneck, revisit lock-free with two-phase CAS protocol

This preserves:
- Cancel-safety (two-phase protocol unchanged)
- ABI stability (same public API)
- Semantic equivalence (same FIFO + capacity invariants)
- Safe fallback (single-threaded mode always available)

## 6. Safe Fallback Status

The baseline MPSC channel remains the first-class implementation:
- 49 functional tests + 9 exhaustion tests = 58 tests passing
- Ring buffer wraparound validated across 20+ cycles
- Capacity invariant enforced: `queue_len + reserved <= capacity`
- Two-phase protocol (reserve/send/abort) fully cancel-safe
- Generation-safe handle detection prevents stale access

The spike exists as a research artifact only. It is not compiled into
the production library.

## 7. Artifacts

| Artifact | Purpose |
|----------|---------|
| `src/channel/mpsc_lockfree_spike.c` | Vyukov spike implementation |
| `tests/unit/channel/test_mpsc_equivalence.c` | 9-test equivalence suite |
| `docs/LOCKFREE_QUEUE_EVALUATION.md` | This evaluation document |
