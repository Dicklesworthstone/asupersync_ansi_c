# Deferred Stubs Register

> Source-level register for intentionally skeletal, ghost-backed, inline, or research-only implementations that are present in-tree by design.

## Purpose

This register complements `docs/DEFERRED_SURFACE_REGISTER.md`.

- `DEFERRED_SURFACE_REGISTER.md` tracks larger product/scope deferrals by wave.
- This file tracks concrete source files that intentionally preserve API shape, research scaffolding, or single-threaded placeholder behavior without claiming full live-mode parity.

## Status Vocabulary

| Status | Meaning |
|---|---|
| `walking-skeleton` | Real API surface exists, but behavior is intentionally reduced to the minimum safe single-threaded or ghost-backed model. |
| `research-spike` | File exists to evaluate a candidate design, not to claim production graduation. |
| `user-supplied` | Runtime hooks/surface intentionally provided by the embedding platform rather than this repo. |

## Current Register

| File | Status | Current Intent | Graduation Trigger |
|---|---|---|---|
| `src/runtime/blocking.c` | `walking-skeleton` | Inline execution preserves blocking API shape without worker-thread dispatch. | Real worker pool for deferred live-mode profiles. |
| `src/runtime/io_driver.c` | `walking-skeleton` | Ghost/no-op backend preserves registration semantics without a native reactor. | Platform adapters with real poll/epoll/kqueue/IOCP style integration. |
| `src/runtime/waker.c` | `walking-skeleton` | Single-threaded flag-based wake tracking for deterministic runtime progress. | Cross-thread wake transport once parallel/profile-specific execution is promoted. |
| `src/runtime/deadline_monitor.c` | `walking-skeleton` | Check-on-poll monitoring against caller-provided timestamps. | Integrated timer/reactor driven monitoring for live-mode profiles. |
| `src/runtime/parallel.c` | `walking-skeleton` | Lane scheduler simulation preserves profile/API shape without enabling real parallel execution. | Post-Wave-A parallel profile promotion under GS-009/GS-010. |
| `src/runtime/arena_locality_spike.c` | `research-spike` | Evaluates arena layout alternatives against baseline task-slot scans. | Separate decision to adopt a proven layout into runtime internals. |
| `src/runtime/barrier_cert_spike.c` | `research-spike` | Evaluates barrier-certificate style scheduler safety checks. | Separate decision to promote a proven runtime safety monitor. |
| `src/platform/freestanding/hooks.c` | `user-supplied` | Freestanding profile intentionally expects embedders to install hooks at runtime init. | No graduation required; this remains an adapter contract rather than an OS implementation. |
| `src/platform/posix/hooks.c` | `walking-skeleton` | Empty POSIX adapter stub. No clock, entropy, reactor, or threading hooks. | Wave B/C: implement clock_gettime, getrandom, epoll, and pthread hooks. |
| `src/platform/win32/hooks.c` | `walking-skeleton` | Empty Win32 adapter stub. No QPC, BCrypt, IOCP, or thread pool hooks. | Wave B/C: implement Win32 platform hooks. |
| `src/channel/mpsc.c` | `walking-skeleton` | Single-threaded non-blocking MPSC channel. | Multi-threaded MPSC with atomic operations (post atomics layer). |
| `src/channel/oneshot.c` | `walking-skeleton` | Fixed-size arena, single-threaded oneshot channel. | Multi-threaded variant for cross-task communication. |
| `src/channel/broadcast.c` | `walking-skeleton` | Fixed-size arena, single-threaded broadcast channel. | Multi-threaded broadcast with atomic subscriber tracking. |
| `src/channel/watch.c` | `walking-skeleton` | Fixed-size arena, single-threaded watch channel. | Multi-threaded watch with atomic version tracking. |
| `src/channel/session.c` | `walking-skeleton` | Inline ring buffers, single-threaded session channels. | Multi-threaded session endpoints. |
| `src/channel/mpsc_lockfree_spike.c` | `research-spike` | Lock-free MPSC ring buffer with portable atomic layer. Single-threaded path functional. | GS-009/GS-010: graduate to production lock-free channel when multi-threaded atomics are validated. |
| `src/runtime/seqlock_ebr_spike.c` | `research-spike` | Seqlock + epoch-based reclamation with portable atomic layer. Single-threaded path functional. | GS-009/GS-010: graduate to production concurrent metadata access when multi-threaded atomics are validated. |
| `src/runtime/lifecycle.c` | `walking-skeleton` | Region/task/obligation lifecycle engine with single-threaded state transitions. | Multi-threaded lifecycle with atomic state transitions for parallel profile. |
| `src/runtime/quiescence.c` | `walking-skeleton` | Close/finalize/quiescence driver. Q2 child-region check now implemented but child-region model is bounded. | Full hierarchical region tree with unbounded children for production. |
| `src/runtime/virtual_time.c` | `walking-skeleton` | Virtual time source for deterministic replay. Callers install via asx_runtime_set_hooks(). | Full virtual-time integration with scheduler time-travel for debug/replay. |
| `src/runtime/runtime_internal.h` | `walking-skeleton` | Fixed-size arena slot types for regions/tasks/obligations. | Dynamic arena sizing or pool-based allocation for production workloads. |
| `src/net/tls.c` | `walking-skeleton` | TLS passthrough wrapper tracking handshake state without real cryptography. | Wave C: integrate platform TLS library (OpenSSL/Mbed TLS/SChannel). |

## Maintenance Rule

If a source file intentionally preserves API shape while deferring full behavior, add it here and state:

1. why it exists now,
2. what it does not yet claim,
3. what would count as graduation.
