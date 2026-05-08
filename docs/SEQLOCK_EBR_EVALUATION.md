# Seqlock Metadata and EBR/Hazard Reclamation Evaluation (bd-3vt.7)

## Summary

Evaluated two optional alpha tracks for the parallel profile, then promoted
the proven task-metadata subset in `bd-pweu.4`:

1. **Seqlock metadata** — optimistic reader-writer synchronization for
   task-slot metadata snapshots
2. **EBR (Epoch-Based Reclamation)** — deferred safe reclamation for
   ownership-sensitive arena slots

Both were prototyped in single-threaded simulation mode, validated against
a baseline spinlock, and benchmarked for overhead. The promoted subset now
uses the production portable atomic abstraction in both single-thread and
multi-thread litmus builds.

## 1. Seqlock Metadata

### How It Works

A seqlock protects a fixed-size metadata block (task state, generation,
alive flag, cancel epoch) for concurrent readers and an exclusive writer.

```
Writer:  seq++ (odd)  →  write data  →  seq++ (even)
Reader:  s1=load seq  →  [if odd, retry]  →  copy data  →  s2=load seq  →  [if s1≠s2, retry]
```

**Key property:** Readers never block the writer. Reads are optimistic and
retry on torn access. Optimal for infrequent writes with frequent reads.

### API

| Function | Purpose |
|----------|---------|
| `asx_seqlock_init()` | Initialize with data size |
| `asx_seqlock_write_begin/end()` | Manual write bracket |
| `asx_seqlock_write()` | Convenience: begin + memcpy + end |
| `asx_seqlock_read()` | Consistent snapshot with retry |
| `asx_seqlock_sequence()` | Query current sequence number |

### Applicability to asx

The task-slot metadata pattern (state + generation + alive + cancel_epoch)
is an excellent seqlock candidate:
- **12 bytes** of hot metadata (well under 64-byte cache line)
- Writes occur only on state transitions (~6 per task lifetime)
- Reads occur on every scheduler poll (high frequency)
- In multi-threaded mode, scheduler workers read concurrently

### Decision: **PROMOTED FOR INTERNAL TASK METADATA**

Seqlocks are the right primitive for task metadata when parallelism is added.
`bd-pweu.4` adds a guarded task metadata slot that covers state, generation,
alive flag, cancel epoch, lane, owner worker, and trace sequence linkage.
The broader scheduler adoption remains a separate task, but the metadata
primitive is no longer only a research decision.

## 2. EBR (Epoch-Based Reclamation)

### How It Works

EBR provides deferred safe reclamation for shared data. Readers announce
which epoch they are operating in. Writers advance the global epoch.
Items deferred in epoch E are safe to reclaim when all readers have
passed epoch E+2.

```
Epochs:  0 → 1 → 2 → 0 → 1 → ...  (rotating mod 3)

Reader:  reader_epoch[id] = global_epoch  →  ...read...  →  reader_epoch[id] = INACTIVE
Writer:  defer(item, epoch)  →  try_advance()  →  reclaim if quiesced
```

**Key property:** Simpler than hazard pointers (no per-object registration).
Requires bounded critical sections (readers must eventually leave).

### API

| Function | Purpose |
|----------|---------|
| `asx_ebr_init()` | Initialize with reader count |
| `asx_ebr_reader_enter/leave()` | Announce/withdraw from epoch |
| `asx_ebr_defer()` | Queue item for future reclamation |
| `asx_ebr_try_advance()` | Advance epoch and reclaim if safe |
| `asx_ebr_current_epoch()` | Query current global epoch |
| `asx_ebr_pending_count()` | Query pending reclamation count |

### Applicability to asx

The task/region slot arenas use generation counters for safe reclamation.
EBR would replace generation-based stale detection with epoch-based
deferred reclamation:

| Current (Generation) | EBR Alternative |
|---------------------|-----------------|
| Slot has `generation` field | Slot deferred to reclaim ring |
| Reader checks `handle.gen == slot.gen` | Reader enters/leaves epoch |
| Writer increments generation on reclaim | Writer defers + advances epoch |
| Stale access returns ASX_E_STALE_HANDLE | Stale access impossible if in epoch |

### Decision: **BOUNDED INTERNAL GUARD, BROADER RECLAMATION DEFERRED**

The current static arena with generation counters is correct and sufficient.
`bd-pweu.4` keeps generation counters as the semantic stale-handle authority
and adds a bounded EBR guard for reader epochs around metadata snapshots and
retirement. Broader arena reclamation still adds value only when:
- Slot memory is dynamically allocated (can't just mark dead + reuse)
- Multiple workers access arenas concurrently (race on generation check)
- Reclamation latency matters (EBR batches, generation is immediate)

Those broader dynamic-allocation conditions still do not apply in the walking
skeleton.

## 3. Baseline Spinlock (Parity Reference)

### How It Works

```
Lock:    CAS(0→1) spin loop
Unlock:  store(0)
```

Simple mutual exclusion. Always correct. Known overhead: cache-line
bouncing under contention, priority inversion risk.

### Comparison

| Property | Seqlock | Spinlock | Raw (no sync) |
|----------|---------|----------|---------------|
| Reader blocks writer | No | Yes | N/A |
| Writer blocks reader | Retry | Yes | N/A |
| Overhead (single-thread) | ~0 | ~0 | 0 |
| Overhead (multi-thread) | Low (memcpy + 2 loads) | High (CAS spin) | N/A |
| Correctness guarantee | Optimistic retry | Mutual exclusion | None |
| Best for | Many readers, rare writes | Balanced read/write | Single-thread |

### Benchmark Results (10,000 rounds, x86_64)

In single-threaded mode, all three strategies have near-identical overhead
because atomic ops degrade to plain loads/stores:

| Strategy | Total cycles | Notes |
|----------|-------------|-------|
| Seqlock | ~baseline | memcpy + 2 sequence loads per read |
| Spinlock | ~baseline | CAS + memcpy + store per lock/unlock |
| Raw | ~baseline | memcpy only (lower bound) |

**Under contention (projected for multi-threaded):**
- Seqlock: readers proceed without CAS, ~1 cache miss per read
- Spinlock: readers spin on CAS, ~N cache misses per contended read
- Expected: seqlock scales linearly with readers, spinlock degrades

## 4. Test Results

36 unit tests across 8 categories, plus seqlock/EBR litmus coverage:

| Category | Tests | What |
|----------|-------|------|
| Seqlock init | 3 | zeros, clamp, null |
| Seqlock function | 6 | write sequence, consistent read, torn-read detection, multiple writes, null safety |
| EBR init | 3 | zeros, clamp, null |
| EBR lifecycle | 8 | enter/leave, defer, full ring, advance, block, reclaim callback, full lifecycle, null |
| Spinlock | 4 | init, lock/unlock, try_lock, null |
| Parity | 2 | same data through all strategies, write-update-read cycle |
| Benchmark + determinism + integration | 5 | benchmark runs, cycles nonzero, seqlock determinism, EBR determinism, seqlock+EBR integration |
| Task metadata slot | 5 | metadata field coverage, publish/snapshot, stale-generation rejection, retire/reclaim, bounded reader epoch |

Formal/litmus coverage:
- `formal-litmus` now links `src/runtime/seqlock_ebr_spike.c` into both
  `ASX_LOCKFREE_SINGLE_THREAD=1` and `ASX_LOCKFREE_SINGLE_THREAD=0` binaries.
- Litmus checks cover seqlock sequence monotonicity/even publication and EBR
  grace-period blocking before reader leave.

## 5. Overall Decision

| Track | Decision | Rationale |
|-------|----------|-----------|
| **Seqlock** | **PROMOTED** | Right primitive for task metadata in parallel profile |
| **EBR** | **BOUNDED GUARD** | Reader epoch guard added; broad reclamation still waits for dynamic arenas |
| **Spinlock** | **FALLBACK** | Always available as safe baseline; no adoption cost |

### When to Revisit EBR

- Dynamic arena allocation replaces static arrays
- Multiple workers share arena access (read-side contention)
- Profiling shows generation check as hot path
- Hazard pointers evaluated and rejected (EBR is simpler)

### Fallback Parity

The baseline locking approach (spinlock or mutex) remains fully verified:
- Parity tests confirm identical observable results through all strategies
- Spinlock implementation is ~30 LOC and provably correct
- No semantic drift from adoption of any strategy

### Preserved Artifacts

- `seqlock_ebr_spike.c`: Seqlock + EBR + guarded task metadata slot + spinlock + benchmark
- `test_seqlock_ebr.c`: 36 validation tests
- `test_memory_model_litmus.c`: seqlock/EBR litmus coverage in single-thread and multi-thread atomic modes
- This document: evaluation and decision rationale
