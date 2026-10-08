# Reality Check and Bridge Plan: `asx` vs asupersync v0.6.0

> **Date:** 2026-10-08
> **C port HEAD:** `c808a6e` (main)
> **Rust reference HEAD:** `/dp/asupersync` `5e60b1c4c` (Cargo version `0.6.0`, unreleased; crates.io max is `0.5.0`)
> **Port spec baseline:** Rust `38c15240` (2026-02-26); fixture provenance `a9e737d8` (2026-03-12, 19,722 commits behind HEAD)
> **Beads at time of check:** 583 total, 583 closed, 0 open, 0 in progress
> **Status:** living document. Revise in place; do not fork into new plan files.

This document answers one question honestly: does the implemented C code deliver the vision in
`README.md` and `PLAN_TO_PORT_ASUPERSYNC_TO_ANSI_C.md`, measured against the *current* Rust
asupersync, which is the authoritative base for this port? It then lays out the plan to close every
gap. Code is the ground truth for current state. The README and plan are the measuring stick.

---

## 1. Executive Summary

The C tree is large, compiles cleanly under strict flags, and its local test suites pass. The
headline promise is that `asx` ports asupersync's semantic model with Rust parity proven by
conformance fixtures, and it is **not met**.

1. **The Rust-parity proof is self-referential.**
   - `make conformance` ran on 2026-10-08 and reported `fixture_records=70 parity_records=0
     comparable_parity_records=0 ... pass=true`. It compared nothing and passed.
   - No C code executes a fixture's `input.ops`. The runner only round-trips each fixture through
     the codec (`tools/ci/run_conformance.sh:339`, `asx_codec_cross_codec_verify`).
   - The Rust capture tool mostly writes the expected events itself rather than running asupersync:
     - channel ops become `user_trace` strings (`tools/fixture_capture/src/main.rs:381-401`);
     - the cancel kind is hardcoded to `User` (`:309`);
     - `Budget::meet` is never called;
     - unknown ops become `unknown_op:` traces (`:446-452`).
   - The four combinator fixtures were written by hand, and their digests do not recompute.
   - A sabotaged fixture (op `launch_missiles`, 50 nonexistent event kinds, `tasks: -1`) passes the
     gate.
   - `codec-equivalence` and `profile-parity` compare digest strings already stored in the fixture
     files.
   - The Rust fuzz differential is skipped by default and is informational even when enabled
     (`tests/fuzz/fuzz_differential.c:1296`).

2. **Real semantic drift exists, and the fake harness hid it.** Verified examples:
   - **Budget priority is reversed.** C meets with `min` and uses `infinite=255`
     (`src/core/budget.c:46,15`). Rust meets with `max` and uses `INFINITE=0`, `ZERO=255`
     (`/dp/asupersync/src/types/budget.rs:569,222,230`). Upstream changed this in March; the C
     README documents the old rule, and the C algebraic "proofs" check the wrong lattice.
   - The cleanup budget is enforced as a hard kill counted from the request, where Rust counts from
     acknowledgement and treats it as advisory.
   - Region close and drain ordering and outcomes differ.
   - C admits normal spawns while a region is Finalizing; Rust rejects them.
   - The default leak policy differs: C uses LOG, Rust uses Panic.
   - C has no cancel attribution chains and its cancel timestamps are always 0.
   - Poll-based race does not drain losers.
   - FIRST_OK, tie-break and owner-reason semantics differ in task groups.
   - **The scheduler** sweeps tasks in arena-index order. Rust's lab uses priority lanes and a
     seeded tie-break, one task per step. Trace parity is impossible for any scenario with two or
     more runnable tasks.
   - Confirmed C bugs:
     - **the trace digest ignores everything after the first 1024 events** (`src/runtime/trace.c:97-180`),
       so "deterministic replay" verification is vacuous for any non-trivial run;
     - the HTTP server is starved by 4 idle connections and never enforces its drain timeout;
     - the scheduler run deadline is ignored while blocked in epoll;
     - barrier is not reusable (`arrived` is never reset, `src/sync/barrier.c:137,145`);
     - `notify_one` with no waiter is lost (`src/sync/notify.c:179`);
     - watch receivers report a change that never happened;
     - an actor reply is lost if the actor exits right after replying (`src/actor/actor.c:404`).

3. **CI on `main` has never passed.**
   - `gh run list --workflow ci.yml --status success` returns nothing; the last 200 runs failed.
   - The first job (`format-check`) fails, so the other seven jobs (unit, conformance, parity,
     fuzz, e2e, compiler matrix, embedded) have never run in CI.
   - Locally `clang-format` is not installed and `make format-check` prints `SKIP`. Every agent
     therefore believed the gate passed.
   - The cause is 6 clang-format violations in 2 files (`src/runtime/runtime_internal.h`,
     `tests/unit/net/test_http_wire.c`, clang-format 18.1.8).

4. **Coverage of the current Rust surface is about 13% (7-20%) overall and about 24% of the kernel-like
   subset.** Rust production code grew about 3.4x since the port's baseline (311K to 1.05M lines).
   The port's own inventory documents are pinned to March. ATP (about 173K lines), `agent_swarm`,
   OTLP, `quic_native`, `lab::{dual_run,swarm_replay}`, `trace::tla_export`, `asupersync-wasm` and
   `asupersync-tokio-compat` are mentioned in no C doc and no bead.

5. **The bead graph claims more completion than the code delivers.**
   - All 583 beads are closed, including `bd-1eqo.16` (the browser runtime), although there is no
     wasm build.
   - The most recent 33K lines (wake-driven scheduler, task groups, HTTP/1.1, WebSocket, RaptorQ,
     native I/O, actors) landed on 2026-10-06/07 with **no beads at all**.
   - None of that new work has a Rust oracle.

6. **Many README claims are wrong.**
   - Counts: 1,364 ASX_API is actually 1,916; 204 tests is 219; 125 headers is 127; 123 sources
     is 131; arena sizes are listed as 64/256/256/32 but are really 8/64/128/16.
   - Eight code samples do not compile.
   - Twelve passages contradict each other: CLI present vs absent, epoll shipped vs "not shipped",
     "walking skeleton" vs threads.
   - Claims that do not hold:
     - R1/R2/R3 class limits are validated but never enforced;
     - the embedded matrix skips all three targets in CI (toolchain-name mismatch plus
       `FAIL_ON_MISSING=0`) and QEMU is off;
     - there is no wasm32 build and no MSVC/Win32 CI build;
     - TLS is a plaintext passthrough;
     - QUIC, gRPC, database, messaging and distributed are in-memory models.

**What genuinely works:**
- Build, unit, invariant and vignette suites pass (159 unit programs plus invariants and
  vignettes; 6 C-level conformance programs).
- The e2e suite passes 22 scenarios in the deterministic CORE profile. The native I/O scenarios
  skip there.
- Things that are real:
  - the wake-driven single-thread scheduler and its park/wake machinery;
  - the HTTP/1.1 parser and connection engine (RFC 9112);
  - the WebSocket codec (RFC 6455);
  - RaptorQ (bit-exact against cberner/raptorq golden vectors);
  - SHA-1/256/512, HMAC and HKDF;
  - the POSIX epoll/poll reactor and native sockets, files, processes and signals.
- The HFT and automotive instrumentation (C-only) is real.
- `parallel-parity` is a real C-vs-C gate (1/2/8/64 logical workers).
- The kernel state-machine tables (region and task transitions, 11 cancel kinds and their
  severities, 6 obligation kinds) match Rust.

The project has a good foundation. It is not the proven, Rust-faithful port its README describes,
and its quality machinery cannot currently detect drift from the Rust base.

---

## 2. Vision Checklist

Status vocabulary: `WORKING` (code + tests + e2e verified), `PARTIAL`, `STUB`, `UNPROVEN` (code exists,
proof missing or vacuous), `NOT_STARTED`, `REGRESSED`, `WRONG_APPROACH`, `DRIFTED` (implemented but
semantically diverges from Rust 0.6.0). Bead coverage at check time: **every row is NO_BEAD**, because
there are 0 open beads.

| # | Vision goal | Source | Status | Evidence |
|---|---|---|---|---|
| V1 | Outcome severity lattice + join | Plan §6.1; README L1225 | PARTIAL | Order/left-bias MATCH (`src/core/outcome.c:15`); outcomes carry no cancel-reason payload, so Rust's Cancelled+Cancelled reason strengthening (`outcome.rs:568`) is lost |
| V2 | Budget algebra (meet = tighter wins, priority per source) | Plan §6.2; README L724 | DRIFTED | priority min vs Rust max; identity/absorbing swapped; no default 128; deadline 0 overloaded as "none" |
| V3 | Cancellation protocol: 11 kinds, severities, cleanup budgets, attribution chains | Plan §6.3; README L747 | DRIFTED | Kinds/severities/quotas MATCH; ordinals differ (LinkedExit/Shutdown); no cause chains; timestamps 0; origin = root region; epoch bumps on every request; cleanup budget hard-kill counted from request |
| V4 | Region lifecycle + close/drain/finalize semantics | Plan §6.4 | DRIFTED | Transition table MATCH; admission in Finalizing wrong; close ordering/outcome missing; drain reason wrong |
| V5 | Task lifecycle + checkpoints + masking | Plan §6.5; README L772 | PARTIAL | Table MATCH; cancel of Created emits spurious Running; `asx_cx_checkpoint` gated/ignores mask/spends poll; value-after-ack policy missing |
| V6 | Obligation linearity + leak handling | Plan §6.6; README L774 | DRIFTED | States/kinds MATCH; default LOG vs Panic; escalation off-by-one; per-obligation vs per-batch; channel permits not obligations |
| V7 | Quiescence invariant | Plan §6.7; README L1063 | PARTIAL | Q1-Q4 implemented; close ordering differs from Rust; unowned obligations allowed |
| V8 | Deterministic wake-driven scheduler | README L709 | PARTIAL / DRIFTED | Park/wake real; ordering is not Rust's (no lanes/priority/seed, sweep-per-round, pending-without-wake re-polled) |
| V9 | Deterministic replay + stable digests | Plan §6.8.3H; README L831 | **WRONG_APPROACH** | Digest covers only the first 1024 events (`trace.c:167`), so divergence after that is invisible and replay verification is vacuous at scale. Fingerprint is an order-sensitive FNV, not Rust's Foata-canonical one. Self-determinism holds only below capacity. |
| V10 | **Rust parity conformance with pinned provenance** | Plan §9; README L1593 | **UNPROVEN (vacuous)** | 0 parity records compared; no op interpreter; fixtures synthesized; provenance stale |
| V11 | Differential fuzzing Rust vs C with minimized counterexamples | Plan §10.6 | **UNPROVEN (skipped)** | Rust binary absent by default; divergences informational; status vocabularies incompatible |
| V12 | JSON/BIN codec semantic equivalence | Plan §7.7; README L49 | UNPROVEN | Compares stored digest strings; `*.codec_bin.json` files are JSON |
| V13 | Cross-profile semantic parity (CORE/FREESTANDING/ROUTER/HFT/AUTO) | Plan §10.6 | UNPROVEN | Compares stored digests; capture tool ignores profile; no per-profile runtime execution |
| V14 | Strict OOM/resource-exhaustion failure-atomic | Plan §10.6 | PARTIAL | `resource-pressure-gate` real for CORE/R3 vs ROUTER/R1, but R-class limits never enforced (only range-validated) |
| V15 | Resource classes R1/R2/R3 with concrete limits | README L483 | STUB | `hooks.c:91` validates range only; arenas fixed at compile-time macros (8/64/128/16) |
| V16 | Channels: MPSC two-phase, oneshot, broadcast, watch, session | Plan §6; README L989 | DRIFTED | MPSC core OK; no cancel-safety, permits not obligations, close vs drop wrong, watch version bug, broadcast drift, waiter caps |
| V17 | Sync primitives cancel-safe | README L1282 | DRIFTED (bugs) | Barrier reuse bug; notify lost wakeup; rwlock reader starvation; waiter caps → RESOURCE_EXHAUSTED |
| V18 | Timers: deterministic ordering, O(1) cancel | Plan §6; README L800 | DRIFTED | Same-tick order differs; timeout boundary; interval first tick/burst; 24h/128 limits |
| V19 | 11 combinators with bounded loser drain | README L852 | DRIFTED | Poll-based race marks losers cancelled without drain; empty race; first_ok cancel |
| V20 | Task groups (race/join/first_ok/quorum) with loser drain | README L863 | DRIFTED | FIRST_OK concurrent vs sequential; tie-break; owner reason; quorum validation timing; sealed branch regions |
| V21 | Actors + supervision | README L1021; Plan Wave D | PARTIAL / DRIFTED | Strategies MATCH; GenServer synchronous; no restart window/backoff; transient-cancelled restarted; reply-loss bug; monitor/link absent |
| V22 | Native I/O: POSIX reactor, sockets, DNS, HTTP/1.1, WS, fs, process, signal | README L1343 | WORKING (POSIX/Linux) / UNPROVEN cross-platform | Real code; e2e native lanes skip in default profile; no kqueue/IOCP; Win32 has no native I/O |
| V23 | Win32 profile | Plan §8.3; README L503 | STUB | Clock/entropy/log real; reactor is SleepEx; no sockets/IOCP; never built in CI |
| V24 | Browser/WASM profile | README L508,L1638 | STUB | Host-compiled only; no wasm32 target anywhere; `bd-1eqo.16` closed anyway |
| V25 | Embedded matrix: cross-build + QEMU + real device | Plan §10.6; README L1859 | STUB (gate silently skips) | CI installs `*-linux-gnu-gcc`, Makefile wants `*-openwrt-linux-musl-gcc`, `FAIL_ON_MISSING=0` → all skipped; QEMU off; no RISC-V in CI |
| V26 | Portability matrix GCC/Clang/MSVC × 32/64 | Plan §10.6 | PARTIAL | compiler-matrix job exists but never ran (CI blocked); no MSVC lane |
| V27 | HFT tail-latency/jitter gate | Plan §10.6 | PARTIAL | Instrumentation real (C-only); `slo-gate` not in CI |
| V28 | Automotive deadline/watchdog gate | Plan §10.6 | PARTIAL | Instrumentation real; e2e lanes pass locally; never ran in CI |
| V29 | Crash/restart replay continuity | Plan §10.6 | PARTIAL | `continuity_restart.sh` passes locally; no Rust oracle |
| V30 | Formal verification (CBMC, algebraic, litmus) | README L1546 | PARTIAL / WRONG-SPEC | Harnesses exist; algebraic budget suite proves the *old* priority semiring; `formal-check` not in CI |
| V31 | Footprint: CORE kernel <64KB (hard <128KB), ROUTER <128KB | Plan §11 | UNPROVEN | Debug archive text 712KB/bss 1.46MB; release not measured in CI; `size-gate` not in CI |
| V32 | Zero dynamic allocation in core / static arenas | README L926 | PARTIAL | Static arrays in bss; DEC-004 says static-arena backend deferred; claim needs precise wording |
| V33 | Performance evidence vs reference | Gauntlet perf pillar; README L600 | NOT_STARTED | No C-vs-Rust benchmark; Rust ships `benches/runtime_vs_tokio.rs` with per-op rows |
| V34 | Baseline freeze + rebase protocol | Plan §1.1 | NOT_STARTED (for 0.6.0) | Inventory pinned to `a9e737d8`; rebase blocked by provenance check + destructive capture script |
| V35 | Release artifacts + install smoke | README L660 | PARTIAL | Local gates exist; never exercised by green CI |
| V36 | Docs truthfulness | AGENTS/README | REGRESSED | Counts, samples, contradictions (see §6) |
| V37 | CI gates block merges | README L1772 | **REGRESSED** | 200/200 CI runs failed; nothing blocks |
| V38 | Systems surfaces beyond kernel (TLS, HTTP/2, gRPC, DB, messaging, QUIC, distributed) | Plan Wave C/D | STUB / EXCLUDED | In-memory models; TLS named but plaintext |

**Delivery:** 0 of 38 goals are fully `WORKING` with Rust-parity evidence. V22 works on Linux
without an oracle. About 10 are PARTIAL. 11 are DRIFTED. 6 are UNPROVEN or vacuous. 6 are STUB or
NOT_STARTED. 2 have regressed.

---

## 3. Answers to the Five Reality-Check Questions

### 3.1 What IS working right now?
- **Strict-flag builds** with `-Werror -Wconversion -Wsign-conversion`, across 131 C sources and
  127 headers.
- **Local suites:** 159 unit programs, 3 invariant suites, 12 vignettes, 6 C conformance
  programs, and 22 e2e scenarios in CORE. All pass.
- **Kernel state-machine tables:** region and task transitions, 11 cancel kinds with Rust
  severities, cleanup quotas and priorities, 6 obligation kinds, mask depth 64.
- **Runtime machinery:** wake-driven park/wake, task timers on a deadline heap, join and watch
  waiters, structured drain, slot reclamation with generations, cross-thread wakes through the
  waker lock and the reactor `notify_fn`.
- **Real protocol engines:**
  - HTTP/1.1 (RFC 9112) parser, serializer and keep-alive engine;
  - WebSocket (RFC 6455) codec, handshake and connection engine;
  - RaptorQ RFC 6330 encoder/decoder, bit-exact against cberner/raptorq;
  - SHA-1/256/512, HMAC, HKDF and base64.
- **Native POSIX backends:** epoll (Linux) and poll(2) reactor, non-blocking sockets, DNS via
  getaddrinfo on the blocking pool, files, fork/exec processes with pidfd, signals through a
  self-pipe.
- **C-only instrumentation:** the HFT histogram, jitter tracker and overload policy; the
  automotive deadline tracker, watchdog, degraded-mode ring and compliance gate.
- **`parallel-parity`:** a real C-vs-C digest comparison across 1/2/8/64 logical workers.
- **C self-determinism:** the fuzz-smoke same-seed comparison.

### 3.2 What is NOT working or not yet implemented?
- **Rust parity:** there is no oracle (§1.1), no fixture interpreter, and the provenance is
  19,722 commits stale.
- **Kernel drift:** see §5 (K1-K14).
- **Scheduler parity:** no lanes, no priority, no seeded tie-break, sweep semantics, and lab clock
  semantics differ (§5 S1-S6).
- **Channel, sync, time, actor drift and bugs:** see §5 (C*, Y*, T*, A*).
- **CI:** red for its entire history.
- **Resource classes:** not enforced.
- **Embedded and portability:** the CI matrix silently skips; no MSVC, wasm32 or QEMU.
- **Platforms:** Win32 native I/O, kqueue and IOCP are absent; the browser profile is host-only.
- **Placeholder surfaces:** TLS, QUIC, gRPC, DB, messaging and distributed are placeholders.
- **Performance:** no comparative benchmark against the reference.
- **Documentation:** wrong in many places (§6).

### 3.3 What is blocking us?
1. **No honest oracle.** Without a harness that runs the real Rust 0.6.0 runtime and the C
   runtime on the same scenario, every "parity" claim is unfalsifiable. Every drift fix risks
   being wrong too. This is the critical-path blocker.
2. **Red CI.** Nothing has ever been enforced in CI, so regressions merge silently. Local
   `SKIP`-on-missing-tool behaviour hides it.
3. **A stale spec.** `EXISTING_ASUPERSYNC_STRUCTURE.md` and the transition and channel docs
   describe Rust as of February/March. Upstream has made 1,077 kernel-path commits since.
4. **Scope and bookkeeping drift.** Work lands without beads, and beads close without evidence
   (`bd-1eqo.16`). The graph shows 100% done.
5. **Architecture choices that block exact trace parity.** The scheduler sweeps instead of
   stepping; outcomes have no reason payload; cancel kinds are ordinal-encoded in digests; waiter
   arrays are fixed-size.

### 3.4 If all open beads were implemented, would the gap close?
**No.** There are zero open beads, so implementing "all open beads" changes nothing. Every gap in
this document is a NO_BEAD gap.

### 3.5 Vision goals not covered by ANY bead
All of V2-V5, V8-V13, V15-V21, V23-V27, V30, V31, V33, V34, V36 and V37. Also all of the
upstream-only surfaces listed in §1.4.

---

## 4. Strategy

**Order of operations:**
1. Make the gates honest.
2. Build the oracle.
3. Fix drift one semantic unit at a time. Each fix lands only with Rust-captured fixtures that
   fail before it and pass after it.
4. Widen coverage.

The kernel comes before surfaces. Proof comes before claims.

Principles carried from the plan and the gauntlet methodology:
- **Honesty lives in the harness.** A gate that can pass with zero comparisons, with sabotaged
  input, or with a missing tool is a lie. Every gate must fail closed, carry an engine identity,
  and run a negative control.
- **Pin the reference.** Rust baseline = `5e60b1c4c` (v0.6.0), pinned by git rev. Every artifact
  embeds the baseline commit, toolchain and lock hash. Upstream drift is handled by the documented
  dual-baseline rebase protocol (plan §1.1), run nightly.
- **Semantic delta budget stays 0.** Resource-plane differences (worker count, arena sizes,
  profile instrumentation) must not change canonical semantics. Semantic differences are either
  fixed or recorded as explicit owner-approved exceptions.
- **Three comparison strengths:**
  - (a) outcome/snapshot equivalence (schedule-independent);
  - (b) Mazurkiewicz trace equivalence via Foata normal form, which is independent of how
    independent events interleave and matches Rust's `trace/canonicalize.rs`;
  - (c) exact trace equivalence under a shared schedule: either Rust
    `LabRuntime::run_forced_schedule` replays C's schedule, or C's lab dispatcher reproduces
    Rust's seeded picker.

  Every fixture must pass (a) and (b). Scheduler-semantics fixtures must also pass (c).
- **Fix the spec before the code.** Re-extract each semantic unit from 0.6.0 with file:line
  citations before changing C.

### 4.1 The spec has a single source: the v4 formal semantics rule index

Rust ships a normative small-step semantics, `/dp/asupersync/asupersync_v4_formal_semantics.md`,
whose **Canonical Rule Index** names every rule, for example:
- ENQUEUE, SCHEDULE-STEP, SPAWN, SCHEDULE, COMPLETE-OK/ERR;
- CANCEL-REQUEST, strengthen, CANCEL-ACKNOWLEDGE, CHECKPOINT-MASKED, CANCEL-DRAIN, CANCEL-FINALIZE;
- CLOSE-BEGIN, CLOSE-CANCEL-CHILDREN, CLOSE-CHILDREN-DONE, CLOSE-RUN-FINALIZER, CLOSE-COMPLETE;
- RESERVE, COMMIT, ABORT, LEAK.

It also defines the independence relation (§1.8) and the scheduler lanes (§1.11). Rust maintains
`formal/impl_refinement_map.md`, which maps every rule to Rust code and to the lab oracle that
witnesses it. Its formal assets include 189 Lean theorems/lemmas in `formal/lean` and a TLA+
model in `formal/tla`.

The C port therefore stops maintaining a parallel, hand-extracted spec as its authority. Instead:

- **`docs/C_REFINEMENT_MAP.md`.** It mirrors Rust's refinement map rule by rule: rule name → C
  function (file:line) → C ghost-monitor check → C unit/invariant test → Rust-captured fixture
  IDs → status (`Implemented | Variant | Partial | Missing`). Every rule must reach `Implemented`
  with fixture evidence, or carry an owner-approved exception.
- **Rule names become spec tags.** Every fixture declares the rules it exercises
  (`"rules": ["CANCEL-REQUEST","strengthen"]`). The comparator reports coverage per rule, so a rule
  with zero fixtures is a visible hole.
- **Lean/TLA crosswalk.** Each Lean theorem that states a kernel invariant (for example
  idempotence of strengthen, bounded cleanup, obligation conservation) gets a C counterpart:
  - a CBMC harness over the C transition functions, when the state is small and bounded;
  - otherwise a property test with the same statement.

  The crosswalk records `theorem → C witness → status`.

  The TLA+ model's invariants become C invariant-suite assertions.
- **The extractor reads real Rust source.** `tools/zero_drift_extractor.py` currently parses
  *embedded sample Rust snippets*, not the real source (another self-referential artifact).
  - Replace it with a real extractor built on `syn`, under `tools/` (tooling may use
    dependencies). It reads `/dp/asupersync` at the pinned rev and emits
    `schemas/rust_kernel_constants.json`: enum variants and ordinals, cancel severity/quota/priority
    tables, budget constants, transition tables, mask depth, cancel-streak limit, attribution
    bounds, restart-intensity defaults.
  - C tables are generated or checked against it. A nightly diff against origin/main HEAD turns
    upstream constant drift into an automatic bead.

### 4.2 Oracle architecture: in-process twin-run

Fixture files alone make a slow, brittle oracle. The stronger design runs **both engines in one
process**:

- **Twin-run harness.** `tools/twin_run/` is a Rust crate that depends on asupersync at the pinned
  rev and builds `libasx.a` through the `cc` crate. It calls one C entry point:
  `asx_conformance_run(const char *scenario_json, asx_conformance_sink *sink)`. That entry point
  is the C fixture interpreter (W1.5), exposed as a library function with a small hand-written
  `extern "C"` binding.
- **Each scenario runs on both runtimes.** The harness projects both traces into the canonical
  vocabulary (W1.2) and compares them at strengths (a)/(b)/(c). The two sides must report
  different `EngineIdentity` values (Rust commit vs C build hash plus git SHA); if they match,
  the run is rejected, so no engine can be compared against itself.
- **Differential fuzzing with coverage guidance.** `cargo fuzz` (libFuzzer) or `proptest` over
  the DSL v2 grammar drives both engines at native speed. Coverage of *both* sides guides the
  mutator (compile `libasx.a` with `-fsanitize-coverage=trace-pc-guard`).
- **The fixture corpus is an output, not hand-maintained.** `twin_run capture` writes the
  checked-in fixtures (JSON plus genuine BIN) with real provenance. CI on machines without Rust
  still has an offline oracle: the C runner replays the fixtures and compares its own execution
  against them.
- **Forced-schedule replay closes the loop for exact parity.** The C lab dispatcher records its
  dispatch sequence, Rust `LabRuntime::run_forced_schedule` replays it, and the harness compares
  event by event. The reverse direction also runs: Rust's certificate schedule is replayed in C
  via L3.
- **Comparing reachable-outcome sets across engines** (alien lever).
  - For small scenarios (≤ 4 tasks, ≤ 12 steps), enumerate the reachable *outcome set* under all
    schedules on both sides:
    - Rust: `lab::explorer` / DPOR;
    - C: L4 bounded DPOR over the same independence relation.
  - Assert set equality.
  - This catches drift that no single seed exposes, such as a C-only reachable leak or a
    Rust-only reachable Cancelled.

### 4.2.1 Scheduling-model contract (the hardest part of exact parity)

Exact trace parity needs more than "add lanes". It needs a written contract, extracted from Rust
code with file:line citations, that pins every degree of freedom:

1. **Step definition.** What one `LabRuntime` step does, in order:
   - fire due timers (or not, under `auto_advance=false`);
   - drain wakes;
   - pick a lane;
   - pick a task;
   - poll it once;
   - apply wakes produced *during* the poll (enqueue position).
2. **RNG.**
   - `util/det_rng.rs` is xorshift64. Pin the seed derivation from `LabConfig.seed` and any
     degenerate-seed remapping (`det_rng.rs:126-167`).
   - Pin exactly when a value is drawn: every step, only on ties, or also for steal/worker choice.
   - Pin how the draw maps to an index (`rng % len`).
3. **Lane membership and promotion.**
   - When a task enters the cancel lane: on request or on acknowledgement.
   - Lane priorities: cleanup priority vs budget priority.
   - The cancel-streak limit (16, widened to 2× during DrainObligations/DrainRegions) and its
     fallback rule.
   - Timed-lane EDF tie-breaks.
4. **Wake ordering.**
   - Where a woken task goes: tail of the lane, or a LIFO slot (the October 2026 Rust change adds
     a LIFO slot for tasks woken by the running task; check whether the lab mirrors it).
   - Wakes for an already-queued task are deduplicated.
5. **Spawn enqueue.** Whether a spawned task is runnable immediately or after the parent's poll
   returns, and in which position.
6. **Time.**
   - When virtual time advances: `advance_to_next_timer` versus `run_with_auto_advance`.
   - How timer coalescing in the 1 ms wheel interacts with steps.
7. **Worker count > 1 in the lab.** How the virtual worker is chosen per step. This is needed
   only for the optional multi-worker parity tier.

The C lab dispatcher (S1) implements this contract exactly. The proof is the **dispatch
certificate**:
- Rust emits a `ScheduleCertificate` (`lab/runtime.rs:2576`).
- C emits the same canonical certificate bytes for the same scenario and seed.
- Certificate equality is the strongest parity statement possible, and the release target for
  every scheduler-unit fixture.

### 4.2.2 Projection completeness (guarding against a too-coarse vocabulary)

A canonical vocabulary that leaves out a state-changing event makes two diverging engines look
equal. Two guards:
- **Projection completeness test (Rust side).**
  - For each Rust `TraceEvent` kind that changes task, region, obligation, channel, timer or
    cancel state, a mutation test alters the event and asserts that the projection changes.
  - Kinds that are deliberately unprojected are listed with a rationale
    (`docs/VOCABULARY_EXCLUSIONS.md`).
  - A new upstream event kind that is neither projected nor excluded fails the nightly drift lane.
- **State-snapshot cross-check.** At the end of every scenario, both engines dump a canonical
  snapshot and the comparator compares it independently of the event stream. The snapshot holds:
  - per task: state, outcome with reason, cancel phase, cleanup polls used;
  - per region: state, close outcome;
  - per obligation: state, abort reason, holder;
  - per channel: queue length, reserved count, closed flags;
  - pending timers.

### 4.3 Statistical honesty for "parity" claims

- **Per-unit Bayesian lower bound.** For each semantic unit, model agreement on N random
  differential scenarios as Beta(1+agree, 1+disagree). The release gate and README claim use the
  **lower 95% credible bound**, never the point estimate. Example: "unit CANCEL: ≥ 99.95% of
  random scenarios agree (95% LB, N=60k)". Any confirmed divergence must be fixed or classified;
  the bound only describes residual uncertainty.
- **Ratchet.** `reports/parity_ratchet.json` stores each unit's lower bound. A change that lowers
  any unit's bound is blocked unless a dated, owner-signed waiver exists.
- **Anytime-valid monitoring.** Long fuzz campaigns monitor the divergence rate with an e-process
  (p₀ = 1e-6, λ = 0.9, α = 1e-3 for software invariants). They stop on evidence by Ville's
  inequality, with no fixed-horizon multiple-testing problem.
- **Negative-evidence ledgers.** Keep `docs/progress/conformance-negative-results.md`,
  `perf-negative-results.md` and `surface-deferrals.md`. Every rejected hypothesis, reverted fix
  or waived divergence gets an entry with a concrete retry-condition predicate. AGENTS.md requires
  grepping them before starting parity or perf work.

---

## 5. Bridge Plan (workstreams and gaps)

Each item gives current state, target, success criteria and dependencies. IDs (W0.x, K*, S*, C*,
Y*, T*, A*, L*, R*, P*, E*, X*) are referenced by the beads.

### W0 — Truth and gate honesty (P0, first)

- **W0.1 Make CI green on main.**
  - *Current:* `check` job fails at format-check.
    - The 6 violations are in `src/runtime/runtime_internal.h` and
      `tests/unit/net/test_http_wire.c`.
    - Every later step (cppcheck lint, API docs, checkpoint coverage, anti-butchering, static
      analysis, strict build, browser builds, model-check) has never been seen to pass in CI.
  - *Target:* a fully green `ci.yml` run on `main`, then keep it green.
  - *Plan:* fix formatting; push; iterate on each newly exposed failing step until every job
    passes. Document each failure class in the bead.
  - *Success:* `gh run list --workflow ci.yml --status success --limit 1` returns a run for HEAD.
    All 8 jobs execute (none skipped).

- **W0.2 Local gates fail closed.**
  - *Current:* `format-check` and `lint` print SKIP when the tool is missing; AGENTS.md tells
    agents to run them.
  - *Target:* a single `STRICT_GATES=1` mode, used by AGENTS.md instructions, under which a
    missing formatter or analyzer is a failure.
  - *Plan:* make clang-format reproducible without system installs via
    `uvx --from clang-format==18.1.8 clang-format`; set the same version in CI.
  - *Success:* on a machine without clang-format, `make format-check STRICT_GATES=1` fails, and
    the documented uvx path passes.

- **W0.3 Make the conformance gates fail closed now, before the oracle exists.**
  - *Target:* `run_conformance.sh conformance` fails when `comparable_parity_records == 0`.
  - *Plan:*
    - rename the current checks to what they are: `fixture-integrity` and `codec-roundtrip`;
    - add a check that every fixture's `semantic_digest` recomputes from its own events (this
      catches the 4 hand-written combinator fixtures);
    - require each `capture_run_id` to be a UUID;
    - make unknown ops a hard error.
  - The gate will go red until the oracle (W1) lands. That is intended and must be visible.
    Record it in the Semantic Delta register as `harness_defect` with an expiry, not as a waiver.
  - *Success:* the sabotage fixture (`launch_missiles`) fails the gate, and the current corpus
    reports `0 comparable` as a failure.

- **W0.4 README truth pass.**
  - Fix every numeric claim and generate the counts with `tools/count_inventory.sh`, checked by
    `lint-docs`.
  - Fix the 8 broken code samples (L1052-1056, 1193, 1209, 1266, 1384-1389, 1414-1424, 1642,
    1644), and compile all samples in CI.
  - Resolve the 12 contradictions (CLI, epoll, walking skeleton, multi-thread dispatch, wasm,
    win32, parity claims, resource classes, `make check` vs `check-ci`, e2e counts, the
    `include/asx/channel` path, stale register rows).
  - Rewrite the "full semantic model" and Rust comparison table claims honestly.
  - Add a generated "Parity status vs asupersync 0.6.0" table, sourced from the oracle report.
  - *Success:* a doc lint compiles the samples and checks the counts. No contradiction remains.

- **W0.5 Docs truth pass.**
  - `FEATURE_PARITY.md`: retire "133/133"; mark statuses as `impl-complete (unverified vs 0.6.0)`
    until oracle evidence exists.
  - `SOURCE_TO_FIXTURE_PROVENANCE_MAP.md`: remove or flag the 66 nonexistent fixture IDs.
  - `RUST_FIXTURE_CAPTURE_TOOLING.md`: it documents a CLI that does not exist.
  - `CHANNEL_TIMER_SEMANTICS.md`: it documents features that are not implemented; the Rust line
    citations are stale.
  - `DEFERRED_STUBS_REGISTER.md`: Win32 and io_driver rows are stale; QUIC handshake and DB no-op
    are missing.
  - `rust-toolchain.toml` vs AGENTS.md: the comment says generic nightly, the file pins
    `nightly-2026-08-31`.
  - `SCENARIO_DSL.md`: opcodes don't match the fixtures.

- **W0.6 Bead truth pass.**
  - Reopen or annotate beads whose closure claims are false (`bd-1eqo.16` browser runtime, the
    conformance beads that claim parity).
  - Create retroactive verification beads for the 2026-10-06/07 work, which had no beads.

- **W0.7 Tracked build artifacts.**
  - Five ELF binaries are tracked under `tests/fuzz/` and `tests/unit/fuzz/`.
  - Removing them requires explicit owner approval (AGENTS RULE 1). Until then, add them to
    `.gitignore` going forward.

- **W0.8 The e2e suite is extremely slow under rch.**
  - `tests/e2e/openwrt_package.sh` calls `tools/packaging/openwrt/build_package.sh`, whose
    `ASX_USE_RCH=auto` runs `rch exec -- make ...`.
  - When `make test-e2e` is itself already on an rch worker, `rch` is on the worker's PATH, so it
    offloads a second time from inside the worker.
  - Observed on 2026-10-08: the 22-family suite took 17.5 minutes. The packaging step alone took
    about 12 minutes; the other 21 families took about 5.
  - Two more problems in the same area:
    - The nested build writes `build/lib/libasx.a` for the EMBEDDED_ROUTER/BIN profile into the
      shared `build/` dir. It clobbers the CORE library that later steps may use, a hidden
      cross-step coupling.
    - There is no per-scenario timeout, so a real hang would stall until rch's 1800 s kill.
  - *Fix:*
    - auto mode detects that it is already inside an rch job (the worker environment exports
      `RCH_CARGO_WRAPPER_BYPASS=1`; prefer an explicit marker such as `ASX_IN_RCH=1` exported by
      the Makefile) and builds locally;
    - the packaging build uses its own build dir;
    - the harness enforces a per-scenario timeout, so a hang becomes a FAIL with a log rather
      than a silent stall.
  - *Success:* `rch exec -- make test-e2e` finishes in under 7 minutes; `build/lib/libasx.a` is
    unchanged by the suite; a deliberately hung scenario fails within its timeout.

- **W0.9 Evidence-required closure.**
  - A bead can't close without an evidence link: test name plus a passing gate run ID or
    artifact path.
  - `lint-evidence` exists but is skipped in CI. Wire it in, and extend it so that a bead whose
    description claims parity must link oracle evidence (W1), not codec round-trips.

- **W0.10 Generated docs to stop drift permanently.**
  - Every count, table and status in the README that can be computed is generated from
    machine-readable sources and checked by `lint-docs`. That covers API counts, test counts,
    arena sizes, profile tables, the parity table and coverage numbers.
  - Hand-edited numbers become a lint failure.

### W1 — Honest differential oracle against Rust 0.6.0 (P0, critical path)

- **W1.1 Rebaseline.**
  - Pin `5e60b1c4c` in `docs/rust_baseline_inventory.json` with the dual-baseline rebase record
    (`a9e737d8` to `5e60b1c4c`).
  - Make `tools/ci/capture_rust_fixtures.sh` non-destructive: write to a staging dir, diff, then
    promote.
  - Pin the capture and fuzz tools by `git = "https://github.com/Dicklesworthstone/asupersync",
    rev = "5e60b1c4c..."`, because 0.6.0 is not on crates.io.

- **W1.2 Canonical semantic vocabulary v2.**
  - A single normative spec plus JSON schema covering:
    - event kinds;
    - the status mapping (Rust `Error` kinds to `asx_status`);
    - cancel kinds **by name**;
    - outcome encoding including the cancel reason;
    - obligation resolution reasons;
    - region close outcomes;
    - timer and channel events.
  - Both the Rust capture tool and the C interpreter emit only this vocabulary.

- **W1.3 Scenario DSL v2.**
  - Versioned JSON schema. Scripted task bodies are small step programs:
    - `yield`, `park_until_woken`, `checkpoint`, `sleep_ns`, `consume_cost`;
    - `reserve`/`commit`/`abort` obligation;
    - `mask`/`unmask`;
    - `spawn_child`;
    - `complete ok|err|panic`.
  - Region ops: `open`, `open_child(budget)`, `close`, `cancel(kind)`, `drain`.
  - Task-group ops, channel/sync waits, timers, actor spawn/cast/call/stop, supervisor policies.
  - Fault injection: clock skew, allocation failure.
  - Forbidden-behavior fixtures declare their expected failure.

- **W1.4 Rust capture tool v2.**
  - Drives the real `LabRuntime` with async bodies that interpret the DSL step programs.
  - Projects real `TraceEvent`s into the vocabulary.
  - Records the dispatch schedule and RNG draws.
  - An unknown op is a hard error. An invariant violation refuses capture unless the fixture is
    declared forbidden.
  - Provenance: real UUID, commit, toolchain, lock hash.
  - Emits Foata fingerprints computed by Rust's own `trace::canonicalize`.

- **W1.5 C fixture interpreter.**
  - Executes DSL v2 through the C runtime in lab mode and emits vocabulary events.
  - Computes the digest from its own execution and never trusts the fixture's digest.
  - The runner records engine identity (lib build hash, git SHA, profile, codec).
  - A negative-control fixture that corrupts one expected event must fail.

- **W1.6 Comparator.**
  - Modes (a) outcome/snapshot, (b) Foata canonical trace and (c) forced/exact schedule.
  - Report fields: first-divergence JSON pointer, mismatch signature, and `delta_classification`
    per plan §9.3.
  - Delta-debugging minimizer for divergent scenarios.
  - `make conformance` uses this comparator.

- **W1.7 Fixture corpus regeneration.**
  - At least 6 scenarios per semantic unit, including must-fail scenarios.
  - Units: budget, cancel, task lifecycle, region lifecycle, obligations, leak policy, masking,
    checkpoint, quiescence, scheduler, timers, MPSC, oneshot, broadcast, watch, sync primitives,
    combinators, task groups, actors, supervision.
  - Fixtures are captured, not authored.

- **W1.8 Real codec/profile parity.**
  - Run the interpreter in every profile build and both codecs.
  - Compare runtime-produced digests.
  - Store real binary fixture files (`.bin`), not JSON relabelled as BIN.

- **W1.9 Real Rust differential fuzz.**
  - A DSL v2 grammar fuzzer drives both engines through the shared vocabulary.
  - Any divergence fails the build.
  - Failures are minimized into `fixtures/fuzz_counterexamples/`.
  - Nightly runs go through rch.
  - Divergence rate is monitored with an anytime-valid e-process, so long campaigns can stop on
    evidence without a multiple-testing penalty.

- **W1.10 Nightly drift lane.**
  - Recapture against `/dp/asupersync` origin/main HEAD and diff against the pinned corpus.
  - Classify each delta (`intentional_upstream | c_regression | spec_defect | harness_defect`).
  - File a bead automatically per new upstream delta.

- **W1.11 Spec re-extraction.**
  - Refresh `EXISTING_ASUPERSYNC_STRUCTURE.md`, `LIFECYCLE_TRANSITION_TABLES.md`,
    `QUIESCENCE_FINALIZATION_INVARIANTS.md`, `CHANNEL_TIMER_*` and
    `RUST_EXPORTED_SURFACE_INVENTORY.md` to 0.6.0 with file:line citations.
  - Tag each rule `[SPEC-NNN]` and map each tag to fixtures (a verification contract).

### Kernel parity (K) — each gated by W1 fixtures

- **K1. Budget priority.**
  - Meet with `max`; `infinite()` priority 0, `zero()` 255, default/new 128.
  - Make deadline presence explicit, so "deadline at time 0" is expressible.
  - Fix the algebraic/CBMC suites and the README.
- **K2. Cleanup budget.**
  - Start the cleanup counter at acknowledgement (Cancelling).
  - Production: advisory. Lab: strengthen to PollQuota.
  - Remove CANCEL_FORCED from the parity surface. An opt-in hard bound becomes a documented
    resource-plane option.
- **K3. Completion policy.**
  - Ordinary spawn keeps an Ok/Err value when the cancel was acknowledged after the first poll.
  - Combinators and JoinSet report Cancelled.
  - Match the Rust classification exactly.
- **K4. Region close.**
  - Close is body-outcome driven.
  - Drain carries a reason: the root region's tasks get that kind, descendants get PARENT.
  - Order: finalizers, then leak audit, then close.
  - `close_outcome` is a join with Cancelled as default when the region was cancelled.
  - Obligations require a holder or are audited.
- **K5. Finalizing admission.** Normal spawn is rejected in Finalizing; add a cleanup-spawn
  entry point.
- **K6. Leak policy.**
  - Default PANIC, following the Rust default. Owner decision D2 covers the embedded
    implications.
  - Count before check (escalation fires on the first leak when threshold is 1).
  - Decide per batch.
  - Recover maps to the Error abort reason.
- **K7. Cancel attribution.**
  - Timestamps on every cancel.
  - Cause chains bounded at depth 16 / 4096 bytes, with truncation metadata.
  - Origin is the immediate parent.
  - Epoch bumps on the first request only.
  - Message tie-break.
  - Outcome carries the reason.
  - Cancel kinds are encoded by name in digests. Ordinal alignment is decision D1.
- **K8. Checkpoint/Cx.**
  - Checkpoint is always allowed and never spends a poll.
  - It detects deadline, poll and cost exhaustion and turns them into the corresponding cancel
    kinds.
  - It honors masking.
  - The capability set aligns to SPAWN/TIME/RANDOM/IO/REMOTE; C-only bits are documented as such.
  - Add CapabilityBudget.
- **K9.** Cancelling a Created task emits no spurious Running transition.
- **K10.** Cost-exhaustion semantics match Rust (cancel when the quota reaches exactly 0 at the
  checkpoint).
- **K11. Poll combinators.**
  - True loser drain: a cancel callback, then poll each loser to completion.
  - Empty race waits for the owner to be cancelled.
  - first_ok stops on a cancel.
- **K12. Task groups.**
  - FIRST_OK becomes sequential, or is renamed with a new sequential variant.
  - Tie-break: 2-way alternation, N-way via the RNG.
  - Owner reason passes through; scope timeout reason is Timeout.
  - Quorum is validated before spawning.
  - Each race branch runs in its own sealed child region.
- **K13. Links/monitors.**
  - LinkedExit and FailFast cancels, trap_exit, monitors (DOWN).
  - Resolve the name clash with the unrelated C `monitor/` and `link/` modules.
- **K14. Outcome payload.** Outcomes carry the cancel reason and a panic payload descriptor, and
  Cancelled+Cancelled joins strengthen the reason.

### Scheduler parity (S)

- **S1. Lab dispatch mode.**
  - One task per step.
  - Lanes: cancel (streak limit 16, widened to 32 while draining), then due timed tasks (EDF by
    deadline and sequence), then ready.
  - Within the top lane: priority, then wake sequence, then a seeded tie-break.
  - The PRNG and draw points replicate Rust's lab exactly; every draw is recorded.
  - Verified with W1 mode (c).
- **S2.** A Pending task that was not woken is not runnable. Yield is an explicit self-wake.
- **S3.** A cancelled task is promoted to the cancel lane with its cleanup priority.
- **S4.** Lab clock is frozen, has an `auto_advance` flag (default off), and
  `clock_is_virtual()` returns true for it.
- **S5.** Production scheduler honors lanes and priority. Budget priority must mean something
  outside the lab.
- **S6.** `asx_parallel_run`: lane order cancel > timed > ready; deadline-ordered timed lane;
  honors parked tasks.
- **S7.** Update the README "ascending arena index" claims.

### Channel parity (C)

- **C1.** Cancel-safety for every channel wait: check cancellation, return `ASX_E_CANCELLED`,
  withdraw from the queue, hand the wake on. No manual `wait_cancel` needed.
- **C2.** Send permits are obligations (commit on send, abort on abort or drop). Outstanding
  permits are auto-aborted at task/region end, and leaks are detected.
- **C3. Close semantics.**
  - Separate close (queue stays drainable) from receiver drop (discard).
  - Sender clone/drop reference counting replaces `close_sender`.
  - Reserve on a closed channel returns DISCONNECTED.
- **C4. Watch.** Versions start at 0; subscribe marks the current version seen; 64-bit versions.
- **C5. Broadcast.**
  - With no receivers, send returns CLOSED.
  - Send returns the receiver count.
  - LAGGED carries the missed count.
  - 64-bit sequence; multiple senders; reserve.
- **C6.** Intrusive unbounded FIFO waiter queues, linked through task slots and shared with sync.
  This removes waiter caps and the eviction/RESOURCE_EXHAUSTED paths.
- **C7. MPSC extras.** `send_evict_oldest`, `recv_many`, `closed()`, and WeakSender equivalents.
  Unbounded is decision D3.
- **C8. Oneshot.** `reserve`/permit with obligation, cancel check, `poll_closed`.
- **C9. Session.** Rust's session means tracked permits; C's means bidirectional request/response.
  Rename the C concept and add the Rust one.

### Sync parity (Y)

- **Y1.** Barrier reusable across rounds (bug fix): round counter, released-then-cancelled
  returns OK.
- **Y2.** Notify keeps a stored permit (lost-wakeup fix).
- **Y3.** RwLock bounds reader starvation: one reader turn per 16 writer hand-offs.
- **Y4.** Semaphore gets `acquire_many`, `add_permits` and `forget`; double release is rejected;
  semaphore permits become obligations.
- **Y5.** Mutex gets poisoning and `lock_until`.
- **Y6.** Waiter caps removed (via C6).

### Time parity (T)

- **T1.** Timers that fall in the same 1 ms tick fire in insertion order, matching Rust's
  4x256-slot wheel.
- **T2.** Timeout expires only when `now > deadline`, and the inner task is polled first at
  equality.
- **T3.** Interval: the first tick is immediate; add Burst (default), Delay and Skip.
- **T4.** Maximum duration is 7 days (clamp); no fixed timer count. Make `timer_wheel.c`
  authoritative or delete it in favor of the scheduler heap (owner approval needed to delete).
- **T5.** `asx_timer_update` refuses a stale handle.

### Actors / GenServer / supervision (A)

- **A1. Bug fix.** In `actor.c:404`, check `replied` before `alive`.
- **A2.** Stop seals the mailbox instead of queuing a message.
- **A3. GenServer.**
  - Runs on its own actor task.
  - `call` returns a token backed by a reply obligation.
  - System-message queue.
  - Cast overflow policy: REJECT by default, DROP_OLDEST optional.
  - Name registry.
- **A4. Supervision.**
  - Sliding restart window (3 per 60 s, virtual time).
  - Exponential backoff 100 ms to 10 s, doubling.
  - Escalation: Stop, Escalate, ResetCounter.
  - Transient children: a cancelled exit is normal.
  - Stop children in reverse start order.
- **A5.** Mailbox capacity 64; send waits for space.
- **A6.** Monitors and links (with K13).

### Lab / trace parity (L)

- **L1. Foata canonical trace class.** An independence relation shared with Rust (spec'd in W1.2)
  plus a canonical fingerprint in C, cross-checked against Rust `canonicalize.rs` fingerprints.
- **L2. Oracle catalog.**
  - Port the Rust lab oracles that apply to the C kernel: quiescence, losers drained, no
    obligation leaks, futurelock, cancel monotonicity, region tree integrity, deterministic
    replay, and others.
  - Map all 24 Rust oracles to C as present, missing or n/a.
- **L3.** Schedule recording and forced-schedule replay in C, the counterpart to Rust
  `run_forced_schedule`.
- **L4. Optional.** Bounded DPOR exploration over small scenarios using the independence relation
  (a C port of the Rust explorer's core). Used to prove scheduler-independence of outcomes for
  fixtures.

### Resource contract (R)

- **R1. Enforce R1/R2/R3 class limits at runtime**, or derive the compile-time arena sizes from
  the class. Exhaustion must be deterministic and failure-atomic at the *class* limit.
- **R2. Arena truth.** Publish actual arena sizes per profile and class, generated into the
  README.
- **R3. Footprint gate.**
  - Release builds per profile, measured in CI.
  - Reconcile the plan targets (CORE kernel < 64 KB) with the current tree. Either a
    `KERNEL_ONLY` build configuration meets them, or the targets are restated with owner sign-off.

### Performance pillar (P)

- **P1. C-vs-Rust comparative bench.**
  - Workloads mirror `benches/runtime_vs_tokio.rs`:
    - spawn + join;
    - yield;
    - mpsc ping-pong;
    - cancel latency;
    - timer churn;
    - TCP request/response;
    - HTTP/1.1 keep-alive.
  - Same host and same minute via rch; report `cv%`.
  - History ratchet in `.bench-history/`.
- **P2.** Put `slo-gate` and `size-gate` in CI, observe-only first, then blocking.
- **P3.** Publish honest perf numbers in the README.

### Embedded / portability (E)

- **E1. Embedded matrix actually runs.**
  - Toolchain triplets must match what CI installs.
  - `FAIL_ON_MISSING=1` in CI.
  - QEMU enabled.
  - RISC-V added.
- **E2.** MSVC/Win32 build lane on `windows-latest`.
- **E3.** 32-bit lane (`-m32`).
- **E4.** wasm32 build lane for CORE and BROWSER (clang `--target=wasm32-wasi` or emcc),
  running the conformance interpreter under wasmtime, as the plan's optional Wasm determinism
  oracle.
- **E5.** ASan/UBSan/TSan lanes in CI. TSan covers the blocking pool and cross-thread waker.
- **E6. Cross-architecture semantic identity.**
  - The checked-in Rust-captured fixture corpus is replayed by the C interpreter under QEMU
    user-mode on aarch64, armv7, mipsel, **mips (big-endian)**, riscv64 and i386.
  - It also runs on wasm32 under wasmtime.
  - Every canonical digest must match x86_64.
  - Big-endian MIPS matters: ath79 and many router SoCs are big-endian, and the BIN codec and
    digest code are exactly where endianness bugs hide.
- **E7. Bare-metal proof.**
  - Build CORE `-ffreestanding` for Cortex-M4 and RISC-V 32. The targets already exist:
    `cross-baremetal-arm-m4-free`, `cross-baremetal-riscv32-router`.
  - Run a fixture subset under `qemu-system-arm -M mps2-an386` / `qemu-system-riscv32 -M virt`
    with semihosting output, and compare digests.
  - Record RAM and flash footprint per resource class. This turns "runs on a $5 router" from a
    claim into an artifact.

### Memory safety and parser hardening (SEC)

A C runtime that parses network input must be held to a higher memory-safety bar than its Rust
reference:
- **SEC1. libFuzzer/AFL++ harness per parser.**
  - Parsers: HTTP/1.1 request and response, chunked decoding, WebSocket frames and handshake,
    DNS name and literal parsing (IPv4/IPv6), the JSON codec decoder, the BIN codec decoder,
    RaptorQ OTI and packet decoding, base64.
  - Built with ASan + UBSan + `-fsanitize=fuzzer`.
  - Seed corpora come from RFC examples and real traffic captures.
- **SEC2. Differential parser fuzzing.**
  - HTTP/1.1 and WebSocket go through the twin-run harness against the Rust parsers
    (`http::h1`, `net::websocket`): accept/reject decisions and parsed fields must agree.
  - RaptorQ goes against both cberner/raptorq and asupersync's own RaptorQ.
- **SEC3. Soak.** 24-hour rch-offloaded campaigns per harness. Crashes are minimized into a
  regression corpus replayed in CI.
- **SEC4. Static analysis.** GCC `-fanalyzer` and clang-tidy (`cert-*`, `bugprone-*`) lanes
  alongside cppcheck. Findings are triaged as must-fix, waived or backlog (plan §10.7.2).
- **SEC5. Secret handling.** HMAC/HKDF keys are wiped, comparisons are constant-time (already
  claimed: verify with `dudect`-style timing tests), and entropy is taken from the OS
  (`getrandom`), never from the deterministic PRNG in non-lab builds.

### Runtime robustness and ergonomics (RB) — found by end-to-end usage, 2026-10-08

These came from building real programs against the library: the quickstart, 14 examples, the
CLI, an HTTP server, a TCP echo server, a 1000-task swarm and cancellation storms, under
ASan/UBSan.

- **RB1 (critical): the trace digest stops tracking execution after 1024 events.**
  - `asx_trace_emit` stores only the first `ASX_TRACE_CAPACITY` events.
  - `asx_trace_digest` hashes only the stored prefix (`src/runtime/trace.c:97-180`).
  - So two runs that diverge after event 1024 produce the **same digest**. A 1000-task swarm
    gave `17ba9890164d5fd7` in every run, including a deliberately perturbed one.
  - The "ring buffer" is a prefix buffer, `asx_trace_event_count` clamps and hides the overflow,
    and the "FNV-1a offset basis" `0x517cc1b727220a95` is not FNV's (`0xcbf29ce484222325`).
  - *Fix:*
    - **rolling digest** updated at emit time, independent of storage;
    - a true ring that keeps the last N events for forensics;
    - an emitted-total and dropped counter;
    - the digest covers the total count;
    - documented constants.
  - *Success:* perturbing event #5000 changes the digest; replay verification detects divergence
    at any depth.
- **RB2: the default build fakes networking and busy-spins.**
  - Plain `make build` gives CORE with `DETERMINISTIC=1` and MEMORY sockets.
  - `asx_server_listen` "succeeds" on port 0 with no OS socket, and the accept loop spins 999,999
    polls until the budget runs out.
  - *Fix:* the MEMORY backend parks on in-memory readiness and never spins; listening on a
    non-loopback or real port in MEMORY mode returns an explicit `ASX_E_PERMISSION_DENIED` (or a
    documented status); README build lines say which flags give real I/O.
- **RB3: HTTP server robustness.**
  - It accepts at most 4 connections (`ASX_HTTP_SERVER_MAX_CONNS=4`); 4 idle connections starve
    every other client.
  - It has no header-read or idle timeout.
  - `drain_timeout_ms` is never read (`src/net/server.c:24`); a stuck client held shutdown for
    12 s with a 5 s drain timeout.
  - It binds only loopback (`server.c:53`, no address field).
  - Inline limits are 4096-byte body, 16 headers, 256-byte URI, all compile-time.
  - *Fix:*
    - configurable connection cap with overload policy;
    - header/idle/request timeouts as task timers;
    - enforced drain timeout (cancel stragglers with SHUTDOWN after the timeout);
    - a bind-address field;
    - per-config limits;
    - a slowloris e2e test.
- **RB4: compile-time caps can't be overridden.** `ASX_TRACE_CAPACITY`, `ASX_MAX_CHANNELS`,
  `ASX_MAX_TIMERS` and `ASX_MAX_WAKERS` lack `#ifndef` guards. Give every `ASX_MAX_*` and
  capacity macro a guard, and add a test that builds with overrides.
- **RB5: the POSIX-live unit suite fails 13/159 suites.**
  - The failures are tests that inject a virtual clock, ghost reactor or entropy (`test_sleep.c:108`,
    `test_deadline.c:79`, `test_io_driver.c:336`, `test_parallel.c:966`, `test_hooks.c:201`, …).
  - `test_process_native` fails when PATH contains unsearchable directories (EACCES).
  - CI runs the unit suite only in deterministic mode (`ci.yml:756`).
  - *Fix:* make each test mode-aware or mode-agnostic, add a CI lane running the full unit suite
    with `PROFILE=POSIX DETERMINISTIC=0`, and make exec PATH search skip EACCES directories the
    way `execvp` does.
- **RB6: sanitizer findings.**
  - UBSan: `src/actor/supervisor.c:367` calls `memcpy(s->specs, children, 0)` with
    `children == NULL`, reachable via `asx_supervisor_start(..., NULL, 0)`.
  - ASan stack-use-after-return in `tests/unit/runtime/test_rt.c:59/449-457`: the test assumes
    `asx_spawn_blocking` completes synchronously, which is true only on CORE.
- **RB7: the run budget's deadline is ignored while blocked in the reactor.**
  - `asx_scheduler_run(rid, &budget)` with a +200 ms deadline hung in `epoll_wait` (`sched_block`
    at `scheduler.c:548`, called from `sched_idle` at `:595`).
  - *Fix:* bound the reactor wait by the run budget's deadline and return
    `ASX_E_POLL_BUDGET_EXHAUSTED`, or a deadline status, on time.
- **RB8: ergonomics.**
  - `ASX_MUST_USE` expands to `warn_unused_result`, and GCC does not honor `(void)` casts against
    it, so `examples/ex_join_set.c:57,72` fails `-Werror`. Provide `ASX_IGNORE_RESULT(x)` and fix
    the example; examples must build under `-Werror` in CI.
  - Library/app config mismatch is undetected: an app compiled with different `ASX_PROFILE_*` or
    `ASX_MAX_*` than the library silently misbehaves. *Fix:* `asx_runtime_init` checks a config
    fingerprint macro compiled into the app against the library's and returns
    `ASX_E_CONFIG_MISMATCH`.
  - `asx doctor` checks only a fresh in-process runtime and always reports HEALTHY. It should
    probe the clock, reactor, sockets, threads and entropy, and reject unknown `--format` values.
  - After `asx_scheduler_run` returns OK the region is still OPEN; document this (or adopt
    Rust's scope semantics via K4).

### Native record/replay: "every failure can be reproduced" (RP)

The README's core promise is "Reproduce production failures exactly". Today determinism is proven
only for lab runs whose inputs are already deterministic. Nothing records a *native* run's
nondeterministic inputs and replays them in the lab.

- **RP1 Recording.** A native-mode recorder captures every nondeterministic boundary into a
  versioned, bounded trace file. That covers:
  - reactor readiness sets;
  - bytes returned by reads/recv, or their hashes plus lengths in "shape-only" mode;
  - accept/connect results;
  - clock reads and entropy draws;
  - signal deliveries;
  - blocking-pool completion order;
  - process exits.

  `hindsight.c` already has the ring and the boundary taxonomy. Extend it into a persisted
  journal with an FNV/SHA-256 chain.
- **RP2 Replay.** A replay backend substitutes every native boundary with the journal and runs
  under the lab clock. The canonical digest must equal the recorded run's digest. If the journal
  is exhausted or diverges, report the first divergent boundary and the trace sequence.
- **RP3 E2E proof.**
  - A real native HTTP/1.1 server under load, with injected client resets and a SIGTERM drain,
    records a journal.
  - The lab replays it bit-exactly 100 times.
  - A deliberately non-deterministic bug (a race the journal captures) is reproduced at the same
    trace sequence every time.
- **RP4 Crash continuity.** The journal survives a crash (fsync policy per profile; RAM-ring on
  ROUTER with flash-wear limits) and replay after restart reproduces the digest. This is the real
  version of the "Crash/Restart Replay Continuity Gate".

### Metamorphic oracles (M): cheap, Rust-independent, always on

These complement the twin-run oracle. They run in plain C CI on every profile.

| ID | Relation | Catches |
|---|---|---|
| M1 | Permuting the spawn order of independent tasks keeps per-task outcomes and the Foata class | order-dependence bugs, accidental arena-index semantics |
| M2 | Adding an independent no-op task changes no other task's outcome or obligation resolution | hidden global coupling |
| M3 | Spawning under `meet(a,b)` is equivalent to nested regions with budgets a then b | budget-inheritance drift (would have caught K1 at runtime) |
| M4 | Issuing a weaker cancel after a stronger one changes nothing | strengthen monotonicity |
| M5 | Profile substitution (CORE ↔ ROUTER ↔ HFT ↔ AUTO ↔ FREESTANDING) keeps the canonical digest when the scenario is within all class limits | real profile parity (replaces the fake gate) |
| M6 | Encoding the *runtime-produced* trace JSON→BIN→JSON keeps the digest | real codec equivalence |
| M7 | Changing worker count keeps the digest (exists: `parallel-parity`) | parallel semantic forks |
| M8 | Scaling all durations by k (no overflow) keeps event order | timer-ordering arithmetic bugs |
| M9 | Record→replay keeps the digest (lab and native via RP) | replay holes |
| M10 | Cancelling before the first poll, and cancelling at the first checkpoint, both yield Cancelled with the same reason kind | completion-policy drift (K3) |

Each relation is a test generator run under the fuzzer's scenario grammar, with ddmin
minimization of violations.

### Governance and process (G)

- **G1.** Mandatory gate run before any commit claiming parity: `make conformance` with the real
  oracle, `make metamorphic`, `make check STRICT_GATES=1`.
- **G2.** Weekly "reality ratchet" report, generated: per-rule refinement status, per-unit parity
  lower bounds, surface coverage (FeatureUniverse), CI health, open drift deltas. Published as an
  artifact and summarized in the README parity table.
- **G3.** FeatureUniverse (`tools/surface/feature_universe.json`).
  - Every Rust 0.6.0 public item in kernel-scope modules is marked
    `present|partial|missing|excluded|n/a`, with weights summing to 1.0 per family.
  - Generated by the syn extractor (§4.1).
  - The coverage dashboard is generated, and excluded items still count as coverage debt.
- **G4.** Upstream-watch: the nightly drift lane (W1.10) files beads tagged `upstream-delta`.
  The weekly triage resolves each delta as port, exclude or defer, with a retry condition.

### Systems surfaces (X) — owner decisions required

- **X1. TLS.** The current API name promises encryption it does not provide. Recommended
  (decision D4): replace it with a hook-based `asx_tls_provider` interface. Users plug in
  BearSSL, mbedTLS or wolfSSL outside the dependency-free core; the core keeps a test-only
  plaintext provider explicitly named as such.
- **X2.** QUIC, gRPC, DB, messaging and distributed in-memory models: either relabel them
  clearly as deterministic in-memory models for lab use, or remove them (D5).
- **X3.** kqueue backend for macOS/BSD (P2).
- **X4.** IOCP or Win32 sockets (P3, after E2).
- **X5.** HTTP/2 and HPACK (P3).
- **X6.** ATP and the other upstream-only surfaces: record them as explicit EXCLUDED rows with
  rationale in `DEFERRED_SURFACE_REGISTER.md`.

---

## 6. README Corrections Inventory (feeds W0.4)

Wrong counts (claimed → actual):

| README location | Claimed | Actual |
|---|---|---|
| L11/19/43/1876 | 1,364 ASX_API | 1,916 |
| L13/52/610/636 | 204 test programs | 219 |
| — | 149 unit tests | 159 |
| — | 20 (or 17) e2e | 22 |
| — | 3 conformance | 6 |
| L590 | 125 headers | 127 |
| L600 | 123 sources | 131 |
| L1696 | 75 docs | 79 |
| L1701 | 23 parity units | 30 |
| L999-1012 arena table | regions 64, tasks 256, obligations 256, MPSC 32 | 8, 64, 128, 16 |
| L1711/1717 CI diagram | "148 unit" / "17 scenarios" | 159 / 22 |

Other corrections:
- **L1527 "no suppression flags".** The build uses `-Wno-unused-parameter`.
- **L1533 "eight lint gates before merge".** CI skips `lint-evidence`, `lint-semantic-delta` and
  `lint-schema-validation`; `formal-check`, `size-gate` and `slo-gate` aren't in CI.
- **L1725 "All 8 jobs must pass".** CI has never passed.
- **L1859 embedded CI.** It skips every target.
- **L1618 "replays each fixture through the C runtime".** False.
- **L1570 "C digest must match" Rust.** Not enforced.
- **L54/L509 profile parity.** Holds only by construction.
- **L1620 "both JSON and BIN".** The BIN files are JSON.
- **Code samples that do not compile:**
  - L1052-1056: `asx_cx_narrow` argument order is wrong and `ASX_CAP_NETWORK` doesn't exist;
  - L1193: `asx_plan_dag_id` should be `asx_plan_id`;
  - L1209: `asx_cleanup_push` takes 4 arguments;
  - L1266: function names are wrong;
  - L1384-1389: the stream API differs;
  - L1414-1424: lab field/function names are wrong;
  - L1642: `asx_abi_feature_flags` doesn't exist;
  - L1644: `ASX_E_NOT_SUPPORTED` doesn't exist.
- **Contradictions:**
  - L297-301/L1882 (no CLI) vs L56-60/L205 (`make cli`);
  - L1844 (no native reactor) vs L1345 (epoll);
  - L1116/L1286 ("walking skeleton") vs cross-thread waker and threads;
  - L1251/L1560 vs L506 (multi-core);
  - L37 ("full semantic model");
  - L140/L495/L557/L951 (WASM and Windows);
  - L131/L491 (resource classes);
  - L631 vs L1546 (`make check` vs `check-ci`);
  - L1493/L1500-1510 (e2e list; `e2e_browser_smoke.c` doesn't exist);
  - L599 (`include/asx/channel/` doesn't exist).
- **L147-160 comparison table.** Rust has a built-in circuit breaker, epochs, a deadline monitor,
  metrics histograms, Lean/TLA artifacts, kqueue/io_uring/Windows reactors, TLS and a wasm crate.

---

## 7. Owner Decisions Needed

| ID | Decision | Recommendation |
|---|---|---|
| D1 | Align cancel-kind ordinals with Rust (ABI break) or encode by name in digests/codecs | Encode by name in canonical vocabulary **and** align ordinals now (no users; AGENTS says no backcompat) |
| D2 | Default leak policy PANIC (Rust parity) vs LOG (embedded-friendly) | PANIC default for parity; profile may override as a documented resource-plane choice only if it does not change canonical semantics (it does — so PANIC everywhere) |
| D3 | Add unbounded MPSC (Rust has it) to a bounded-memory C runtime? | Add, gated by profile capability (ROUTER/AUTOMOTIVE forbid via `ASX_FORBID_UNBOUNDED_QUEUE`) |
| D4 | TLS: provider hook vs in-tree TLS 1.3 | Provider hook; rename current passthrough |
| D5 | QUIC/gRPC/DB/messaging/distributed in-memory models: relabel or remove | Relabel as lab models; exclude from README feature claims |
| D6 | Footprint targets (<64KB CORE kernel) vs current tree size | Introduce `ASX_KERNEL_ONLY` build to meet target; otherwise restate |
| D7 | Delete tracked ELF binaries and orphan `timer_wheel.c` if superseded | Approve deletion explicitly (RULE 1) |

---

## 8. Milestones and Exit Gates

| Milestone | Contents | Exit gate (all must hold) |
|---|---|---|
| **M-α Honest gates** | W0.1-W0.10; bug fixes Y1, Y2, A1, C4, RB1, RB6, RB7 | CI green on main with all 8 jobs executed. Fake gates renamed and fail-closed. README/docs lint passes with generated counts and compiled samples. Sabotage fixture rejected. |
| **M-β Oracle online** | W1.1-W1.6, §4.1 refinement map v1, real extractor, twin-run harness, K1 as the first unit fixed under the oracle | `make conformance` compares more than 0 records using executed C plus Rust-captured fixtures. Budget unit at 100% (a)+(b) with the K1 fix. Negative control fails. EngineIdentity asserted. |
| **M-γ Kernel parity** | K2-K14, S1-S7, L1, L3, W1.7 corpus | Every kernel rule in the refinement map is `Implemented` with fixtures. Dispatch-certificate equality on scheduler fixtures. Per-unit parity LB ≥ 99.9% (95%) on ≥ 20k random scenarios per unit. |
| **M-δ Primitive parity** | C1-C9, Y3-Y6, T1-T5, A2-A6 | Same LB criterion for channel, sync, time, actor and supervision units. Waiter caps gone. Metamorphic M1-M10 green. |
| **M-ε Proof breadth** | W1.8-W1.10, L2, L4, M, RP, SEC, E1-E7, R1-R3 | Cross-arch digests identical, including BE MIPS and wasm32. Native record/replay e2e green. 24h fuzz soak clean. Footprint artifacts per class. Nightly drift lane live. |
| **M-ζ Surface and perf** | P1-P3, X1-X6, G2-G3 | C-vs-Rust bench published with cv%. FeatureUniverse coverage published and ratcheting. Owner decisions D1-D7 recorded. README fully generated from evidence. |

## 9. Execution Order (critical path)

```
W0.1 CI green ─┬─> W0.3 fail-closed gates ─> W1.1 rebaseline ─> W1.2 vocab ─> W1.3 DSL v2
               │                                                     │
               └─> W0.4/W0.5 truth passes                            v
                                                   W1.4 Rust capture v2 ─┐
                                                   W1.5 C interpreter ───┼─> W1.6 comparator ─> W1.7 corpus
                                                                         │
   Bug fixes that need no oracle (Y1, Y2, A1, C4) can land immediately with C tests + later fixtures.
                                                                         v
            K1..K14, S1..S7, C1..C9, Y3..Y6, T1..T5, A2..A6  (each: spec re-extract → fixture → fix → green)
                                                                         v
            W1.8 profile/codec parity, W1.9 fuzz, W1.10 nightly drift, L1..L4, R1..R3, P1..P3, E1..E5, X*
```
