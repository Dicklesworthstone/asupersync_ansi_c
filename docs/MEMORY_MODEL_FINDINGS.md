# Memory-Model Litmus Suite Findings

> **Bead:** bd-3vt.4
> **Status:** Canonical findings artifact
> **Last updated:** 2026-05-08 by SilverFrog

## 1. Executive Summary

The asupersync ANSI C runtime keeps the core semantic contract deterministic:
profiles may change resource envelopes, telemetry, and worker routing, but not
canonical event meaning or replay identity. The newer parallel-runtime
graduation path now uses the shared `include/asx/platform/atomics.h`
portability layer for blocking-pool state, seqlock/EBR metadata, and
lock-free queue foundations. Those atomics are not a license for hidden
semantic drift; they are covered by explicit litmus and bounded scheduler
proofs.

The litmus suite now validates both the original C99 portability assumptions
and production-parallel safety assumptions: release/acquire publication,
single-owner queue slots, seqlock consistency, EBR generation checks,
gap-free trace commit tickets, bounded work stealing, cancel-lane fairness,
and shutdown/drain state reporting.

## 2. Findings

### 2.1 Atomic Synchronization Is Explicit And Bounded

| Category | Found | Expected |
|----------|-------|----------|
| Core transition/lifecycle atomics | 0 | 0 |
| Portable atomic abstraction | present | present |
| Blocking-pool atomic state | present | present |
| Seqlock/EBR metadata atomics | present | present |
| Parallel scheduler semantic commit ordering | explicit counter | explicit counter |
| `__sync_*` builtins | 0 | 0 |
| Inline assembly in kernel/runtime path | 0 | 0 |
| Ad hoc `volatile` synchronization | 0 | 0 |

**Verdict:** Synchronization is concentrated in named portability and runtime
support surfaces. The semantic scheduler still exposes replay-stable ordering
instead of platform-dependent interleavings.

### 2.2 Codegen Stability

Observable behavior of critical kernel functions is **identical** across
all tested optimization levels:

| Compiler | -O0 | -O1 | -O2 | -O3 | -Os |
|----------|-----|-----|-----|-----|-----|
| GCC | ref | match | match | match | match |

Probed functions:
- Transition table lookups (region, task, obligation)
- Cancel severity computation
- Outcome join operation
- Handle validation
- Budget meet/exhaust operations
- Type sizes

**Verdict:** No optimization-level-dependent behavior detected.

### 2.3 Litmus Assumptions Validated

31 litmus tests verify the C99 and parallel-runtime assumptions the runtime
relies on. `make formal-litmus` builds the same suite twice:
`ASX_LOCKFREE_SINGLE_THREAD=1` and `ASX_LOCKFREE_SINGLE_THREAD=0`.

| Assumption | Test | Status |
|------------|------|--------|
| Handle types are 8 bytes | LITMUS-1 | PASS |
| Unsigned overflow wraps | LITMUS-2 | PASS |
| Shift/mask handle packing is portable | LITMUS-3 | PASS |
| Enum values match explicit assignments | LITMUS-4 | PASS |
| Signed↔unsigned cast preserves bits | LITMUS-5 | PASS |
| memset(0) produces valid zero-init | LITMUS-6 | PASS |
| Function pointers are comparable | LITMUS-7 | PASS |
| Enum values are valid array indices | LITMUS-8 | PASS |
| sizeof(struct) is stable per TU | LITMUS-9 | PASS |
| Transition lookup is deterministic | LITMUS-10 | PASS |
| NULL is detectable | LITMUS-11 | PASS |
| Bitwise ops on uint64_t are correct | LITMUS-12 | PASS |
| CHAR_BIT is 8 | LITMUS-13 | PASS |
| Outcome join is optimization-stable | LITMUS-14 | PASS |
| Cancel severity is a pure function | LITMUS-15 | PASS |
| Atomic backend selection is explicit | LITMUS-16 | PASS |
| Atomic load/store/init preserve values | LITMUS-17 | PASS |
| Compare-exchange reports observed value | LITMUS-18 | PASS |
| RMW operations return unique old values | LITMUS-19 | PASS |
| Acquire/release fences are callable boundaries | LITMUS-20 | PASS |
| Seqlock sequence is even and monotone after writes | LITMUS-21 | PASS |
| EBR grace period blocks reclaim under active readers | LITMUS-22 | PASS |
| Release/acquire publication preserves metadata payload | LITMUS-23 | PASS |
| Queue slot reservation is single-owner and terminal | LITMUS-24 | PASS |
| Seqlock readers reject in-progress publications | LITMUS-25 | PASS |
| Stale-generation retirement is fail-closed | LITMUS-26 | PASS |
| Trace commit tickets are gap-free and monotone | LITMUS-27 | PASS |
| Bounded work-steal model preserves ownership accounting | LITMUS-28 | PASS |
| Cancel-lane fairness model yields to live work | LITMUS-29 | PASS |
| Parallel commit sequence covers worker commits | LITMUS-30 | PASS |
| Budget exhaustion leaves workers draining, not drained | LITMUS-31 | PASS |

**Verdict:** C99 portability, atomic publication, metadata reclamation, and
parallel scheduler ordering assumptions are covered by the same formal-litmus
gate.

### 2.4 Parallel Scheduler Bounded Proofs

The litmus file now includes small-state proofs and public-runtime checks for
the production parallel scheduler:

| Proof Surface | Coverage | Evidence |
|---------------|----------|----------|
| Queue slot ownership | Empty -> reserved -> committed is single-owner; stale CAS observes the terminal state | LITMUS-24 |
| Work stealing | Exhaustive owner-depth model preserves total ownership and transfers at most one slot | LITMUS-28 |
| Cancel fairness | Exhaustive cancel/ready/timed/streak model yields only when live non-cancel work exists | LITMUS-29 |
| Commit ordering | Worker commit totals sum to the global commit sequence; last worker commit is below the terminal counter | LITMUS-30 |
| Shutdown/drain | Budget exhaustion reports `ASX_WORKER_DRAINING`; quiescent completion reports drained in the parallel unit lane | LITMUS-31 plus `tests/unit/runtime/test_parallel.c` |

**Verdict:** The proof surface covers the scheduler invariants required by
`bd-pweu.13` without claiming that platform adapters may execute arbitrary
interleavings outside the replay-stable commit order.

### 2.5 Translation Validation

102 checks confirm C implementation matches the invariant schema:

| Domain | Ordinals | Terminals | Legal Trans. | Forbidden Trans. |
|--------|----------|-----------|-------------|-----------------|
| Region | 5/5 | 5/5 | 5/5 | 15/15 |
| Task | 6/6 | 6/6 | 10/10 | 23/23 |
| Obligation | 4/4 | 4/4 | 3/3 | 13/13 |

**Verdict:** C code and schema are in perfect agreement.

## 3. Unsafe Assumptions Documented

### 3.1 Current Deterministic Core

These assumptions remain **safe** for the deterministic core model:

1. **No data races possible** — single execution thread
2. **Sequential consistency** — guaranteed by C99 for single-threaded code
3. **Deterministic execution order** — guaranteed by scheduler design
4. **No torn reads/writes** — no concurrent access

### 3.2 Remaining True-Concurrency Risks

These assumptions need continued explicit mitigation as platform adapters move
from replay-stable worker simulation to live concurrent execution:

| Assumption | Risk Level | Mitigation Required |
|------------|-----------|-------------------|
| Global state is thread-local | **HIGH** | Thread-local storage or lock-protected access |
| MPSC channel atomics are incorrectly ordered | **HIGH** | Two-phase slot ownership litmus plus MPSC contention tests |
| Scheduler commit order drifts under live workers | **HIGH** | Replay-stable commit tickets and parallel parity |
| Ghost monitors use global ring buffer | **MEDIUM** | Per-thread rings or atomic ring buffer |
| Handle generation counter is non-atomic | **HIGH** | Atomic increment or per-thread counters |
| Cleanup stack is not thread-safe | **HIGH** | Per-region lock or lock-free stack |

### 3.3 Mitigations in Place

1. **Portable atomics layer** — all production atomics route through
   `include/asx/platform/atomics.h`
2. **Formal litmus gate** — single-thread and multi-atomic builds exercise
   31 assumptions, including publication, queue ownership, seqlock, EBR, and
   scheduler commit ordering
3. **Anti-butchering contract** — any change touching kernel semantics
   requires explicit owner sign-off
4. **Thread-affinity stubs** — debug-mode domain violation detection
   exists but compiles to no-ops in release
5. **Parallel parity and e2e gates** — worker-count changes must preserve
   semantic digests and structured event summaries

## 4. Recommendations

1. **Keep replay-stable commit ordering mandatory** — platform worker adapters
   may execute concurrently only if externally committed events remain
   deterministic.

2. **Litmus suite in CI** — run `make formal-litmus` and `make formal-codegen`
   in CI to catch assumption violations early.

3. **When deepening live parallelism:**
   - Keep all atomics inside the portable abstraction layer
   - Add true threaded stress or sanitizer jobs when the CI matrix supports them
   - Extend litmus coverage with store-buffer, message-passing, and
     load-buffering patterns when the harness can run cross-thread cases
   - Add ThreadSanitizer CI job
   - Keep lock-free MPSC parity tied to the two-phase queue ownership model

4. **Cross-compiler matrix** — currently tested with GCC only. Extend to
   Clang and MSVC when CI matrix supports them.

## 5. Artifacts

| Artifact | Purpose |
|----------|---------|
| `tests/formal/litmus/test_memory_model_litmus.c` | 31 C99, atomics, metadata, and scheduler litmus tests |
| `tools/ci/check_codegen_stability.sh` | Cross-optimization behavioral comparison |
| `make formal-litmus` | Run litmus suite |
| `make formal-codegen` | Run codegen stability check |
| `make formal-check` | Run all formal gates (L2-L4 + litmus + codegen) |
