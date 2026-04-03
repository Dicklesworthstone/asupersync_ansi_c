# Memory-Model Litmus Suite Findings

> **Bead:** bd-3vt.4
> **Status:** Canonical findings artifact
> **Last updated:** 2026-03-01 by NobleCanyon

## 1. Executive Summary

The asupersync ANSI C runtime is **synchronization-free by architectural
design** at the kernel/runtime level. All shipped execution remains
single-threaded and deterministic. The production runtime surface does
not rely on atomic operations, memory barriers, locks, or
multi-threading primitives.

Research spike/support code now includes a shared
`include/asx/platform/atomics.h` portability layer used by the
lock-free queue and seqlock/EBR evaluation spikes. That does not change
the kernel claim: parallel semantics remain deferred, and the atomics
layer exists to keep those spike surfaces portable and non-duplicative.

This finding is intentional: the architecture explicitly defers parallelism
to post-kernel phases (Wave D, ALPHA-5/6), documented in ADR-001 and the
Guarantee Substitution Matrix (GS-009, GS-010: mapped-deferred).

The litmus suite validates that the C99 assumptions underpinning this design
hold across compilers, targets, and optimization levels.

## 2. Findings

### 2.1 Kernel Runtime Remains Free Of Atomic Synchronization

| Category | Found | Expected |
|----------|-------|----------|
| `_Atomic` types in kernel/runtime path | 0 | 0 |
| `__atomic_*` builtins in kernel/runtime path | 0 | 0 |
| `__sync_*` builtins | 0 | 0 |
| Memory barriers in kernel/runtime path | 0 | 0 |
| Inline assembly in kernel/runtime path | 0 | 0 |
| Locks/mutexes in kernel/runtime path | 0 | 0 |
| `volatile` for sync in kernel/runtime path | 0 | 0 |

**Verdict:** The shipped kernel/runtime is clean. No synchronization code
exists there. Research spikes may carry atomics scaffolding without
changing the single-threaded runtime contract.

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

### 2.3 C99 Assumptions Validated

15 litmus tests verify the C99 assumptions the runtime relies on:

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

**Verdict:** All C99 assumptions hold.

### 2.4 Translation Validation

102 checks confirm C implementation matches the invariant schema:

| Domain | Ordinals | Terminals | Legal Trans. | Forbidden Trans. |
|--------|----------|-----------|-------------|-----------------|
| Region | 5/5 | 5/5 | 5/5 | 15/15 |
| Task | 6/6 | 6/6 | 10/10 | 23/23 |
| Obligation | 4/4 | 4/4 | 3/3 | 13/13 |

**Verdict:** C code and schema are in perfect agreement.

## 3. Unsafe Assumptions Documented

### 3.1 Current (Single-Threaded)

These assumptions are **safe** in the current single-threaded model:

1. **No data races possible** — single execution thread
2. **Sequential consistency** — guaranteed by C99 for single-threaded code
3. **Deterministic execution order** — guaranteed by scheduler design
4. **No torn reads/writes** — no concurrent access

### 3.2 Future (When Parallelism Added)

These assumptions will need **explicit mitigation** when GS-009/GS-010
are implemented:

| Assumption | Risk Level | Mitigation Required |
|------------|-----------|-------------------|
| Global state is thread-local | **HIGH** | Thread-local storage or lock-protected access |
| MPSC channel is single-threaded | **HIGH** | Atomic head/tail with memory ordering |
| Scheduler poll order is deterministic | **MEDIUM** | Per-worker deterministic sub-ordering |
| Ghost monitors use global ring buffer | **MEDIUM** | Per-thread rings or atomic ring buffer |
| Handle generation counter is non-atomic | **HIGH** | Atomic increment or per-thread counters |
| Cleanup stack is not thread-safe | **HIGH** | Per-region lock or lock-free stack |

### 3.3 Mitigations in Place

1. **GS-009/GS-010 are explicitly deferred** — no parallel code ships prematurely
2. **Anti-butchering contract** — any change touching kernel semantics
   requires explicit owner sign-off
3. **Thread-affinity stubs** — debug-mode domain violation detection
   exists but compiles to no-ops in release
4. **Litmus suite** — will catch breakage if assumptions are violated

## 4. Recommendations

1. **Keep single-threaded baseline** — the current model is correct, efficient,
   and verifiable. Do not add synchronization prematurely.

2. **Litmus suite in CI** — run `make formal-litmus` and `make formal-codegen`
   in CI to catch assumption violations early.

3. **When adding parallelism:**
   - Design an atomic abstraction layer (portable across GCC/Clang/MSVC)
   - Extend litmus suite with true memory-ordering tests (store-buffer,
     message-passing, load-buffering patterns)
   - Add ThreadSanitizer CI job
   - Implement lock-free MPSC before parallel scheduler

4. **Cross-compiler matrix** — currently tested with GCC only. Extend to
   Clang and MSVC when CI matrix supports them.

## 5. Artifacts

| Artifact | Purpose |
|----------|---------|
| `tests/formal/litmus/test_memory_model_litmus.c` | 15 C99 assumption tests |
| `tools/ci/check_codegen_stability.sh` | Cross-optimization behavioral comparison |
| `make formal-litmus` | Run litmus suite |
| `make formal-codegen` | Run codegen stability check |
| `make formal-check` | Run all formal gates (L2-L4 + litmus + codegen) |
