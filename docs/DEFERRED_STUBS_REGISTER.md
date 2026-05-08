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
| `graduating-internal` | Production-path internals have landed behind gates, but public/operator claims still depend on named follow-up evidence. |
| `graduated` | The source path is no longer deferred for its stated scope; future work is additive or platform-specific. |
| `superseded-reference` | Historical implementation retained for benchmark/design comparison after a different production path landed. |
| `user-supplied` | Runtime hooks/surface intentionally provided by the embedding platform rather than this repo. |

## Current Register

| File | Status | Current Intent | Graduation Trigger |
|---|---|---|---|
| `src/runtime/blocking.c` | `graduating-internal` | Runtime-owned blocking slots preserve deterministic inline execution by default and can submit opaque jobs through a live-mode hook when the platform installs one. | Broader cancellation-aware blocking handles and cross-profile conformance gate remain tracked by `bd-pweu.11`. |
| `src/runtime/io_driver.c` | `walking-skeleton` | Ghost/no-op backend preserves registration semantics without a native reactor. | Platform adapters with real poll/epoll/kqueue/IOCP style integration. |
| `src/runtime/waker.c` | `graduating-internal` | Sequence-tagged wake tracking now provides deterministic ready drains across timer/reactor/worker boundaries. | Platform adapters still own the concrete cross-thread wake transport. |
| `src/runtime/deadline_monitor.c` | `walking-skeleton` | Check-on-poll monitoring against caller-provided timestamps. | Integrated timer/reactor driven monitoring for live-mode profiles. |
| `src/runtime/parallel.c` | `graduating-internal` | Deterministic worker-lane routing, bounded steal ordering, timed-lane wake promotion, ghost/native reactor readiness pumping, worker drain states, replay-stable commit counters, large-swarm telemetry, and formal memory-model proofs are live in the core scheduler. `bd-pweu` tracks remaining production-scale claims. | `bd-pweu.9` locality decision and `bd-pweu.14` RCH-backed benchmark baselines before broad 64-core performance claims; native platform adapters must preserve deterministic commit authority before executing lanes concurrently by default. |
| `src/runtime/arena_locality_spike.c` | `research-spike` | Evaluates arena layout alternatives against baseline task-slot scans. | Separate decision to adopt a proven layout into runtime internals. |
| `src/runtime/barrier_cert_spike.c` | `research-spike` | Evaluates barrier-certificate style scheduler safety checks. | Separate decision to promote a proven runtime safety monitor. |
| `src/platform/freestanding/hooks.c` | `user-supplied` | Freestanding profile intentionally expects embedders to install hooks at runtime init. | No graduation required; this remains an adapter contract rather than an OS implementation. |
| `src/platform/posix/hooks.c` | `graduated` | POSIX clock, entropy, reactor wait, and bounded pthread blocking-submit hooks are installed through `asx_runtime_hooks`; shutdown drains queued/running blocking jobs before reset. | Future work: timed reactor registration and scheduler integration under `bd-pweu.8`. |
| `src/platform/win32/hooks.c` | `walking-skeleton` | Empty Win32 adapter stub. No QPC, BCrypt, IOCP, or thread pool hooks. | Wave B/C: implement Win32 platform hooks. |
| `src/channel/mpsc.c` | `graduating-internal` | Two-phase MPSC channel now has deterministic CORE ring-buffer semantics plus POSIX/PARALLEL atomic committed-message publication behind the same reserve/send/abort API. | Parallel parity and memory-model gates are closed; remaining broad claim depends on platform worker execution paths and `bd-pweu.14` benchmark/admission evidence. |
| `src/channel/oneshot.c` | `walking-skeleton` | Fixed-size arena, single-threaded oneshot channel. | Multi-threaded variant for cross-task communication. |
| `src/channel/broadcast.c` | `walking-skeleton` | Fixed-size arena, single-threaded broadcast channel. | Multi-threaded broadcast with atomic subscriber tracking. |
| `src/channel/watch.c` | `walking-skeleton` | Fixed-size arena, single-threaded watch channel. | Multi-threaded watch with atomic version tracking. |
| `src/channel/session.c` | `walking-skeleton` | Inline ring buffers, single-threaded session channels. | Multi-threaded session endpoints. |
| `src/channel/mpsc_lockfree_spike.c` | `superseded-reference` | Original direct-enqueue Vyukov spike remains as the reference microbenchmark and design artifact. Production two-phase semantics now live in `src/channel/mpsc.c`. | Keep as historical/reference evidence unless a later benchmark replaces it with a public benchmark fixture. |
| `src/runtime/seqlock_ebr_spike.c` | `graduating-internal` | Seqlock task metadata, bounded EBR reader epochs, and spinlock parity harness. `bd-pweu.4` and `bd-pweu.13` promote the metadata contract through focused unit, litmus, and codegen coverage; broader arena adoption remains bounded by scheduler/channel integration evidence. | Next graduation: retire the spike or convert it to a pure reference harness once live task/region/channel arenas no longer need the separate comparison artifact. |
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
