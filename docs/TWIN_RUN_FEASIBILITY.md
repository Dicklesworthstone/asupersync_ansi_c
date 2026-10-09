# Twin-run feasibility (W1.4s spike, bd-9kll.2.16)

**Decision: GO.** A crate outside asupersync can drive its lab runtime at the
pinned revision with every capability the W1.4 twin-run design relies on, using
only public API and no cargo features beyond the defaults. No upstream
visibility change is needed.

- Reference: asupersync 0.6.0 at `5e60b1c4c53d62aaddae68de3ee7de4732f1755b`,
  pinned by git rev (0.6.0 is not on crates.io).
- Spike crate: `tools/twin_run_spike/` (standalone workspace, `Cargo.lock`
  committed). It is the seed of `tools/twin_run`.
- Toolchain: the repo pin `nightly-2026-08-31` (asupersync's default
  `nightly-outcome-try` feature needs nightly; the pin matches asupersync's own).
- Run: `cd tools/twin_run_spike && rch exec -- cargo run`. Each capability prints
  `CAP <id> <ok|FAIL> <detail>`; the last line is `SPIKE GO` or `SPIKE NO-GO`.

## Capability table

Evidence is the spike's output from 2026-10-09 (rch worker vmi1227854, debug
build), quoted in the last column.

| # | Capability | API used (all `pub`, no feature flags) | Result |
|---|---|---|---|
| 1 | Construct `LabRuntime` from `LabConfig {seed, worker_count, ...}` | `asupersync::lab::{LabConfig, LabRuntime}`; `LabConfig::new(seed).worker_count(1).trace_capacity(..).max_steps(..)` | ok: `seed=42 workers=1` |
| 2 | Real async task bodies in a real region with budgets | `lab.state.create_root_region(Budget)`, `lab.state.create_task(region, Budget, async {..})`, `lab.scheduler.lock().schedule(task, 0)`, `lab.run_until_quiescent()`, `lab.is_quiescent()`; `Cx::current()` inside bodies | ok: `steps=7 quiescent=true` |
| 3 | Full trace buffer as `TraceEvent` values with readable fields | `lab.trace().snapshot() -> Vec<TraceEvent>`; `TraceEvent { seq, time, logical_time, kind, .. }` are `pub` fields | ok: `events=11 seq_monotonic=true` |
| 4 | Forced schedules: record, serialize canonically, decode, replay exactly | `lab.start_forced_schedule_recording(max)`, `finish_forced_schedule_recording()`, `ForcedSchedule::to_canonical_bytes()` / `try_from_canonical_bytes(bytes, ForcedScheduleDecodeLimits)`, `run_forced_schedule(&s, ForcedScheduleLimits)` from `asupersync::lab::runtime` | ok: `dispatches=7 canonical_bytes=301 decode_roundtrip=true`; replay `schedule_hash_match=true trace_fingerprint_match=true` |
| 5 | Schedule certificate | `lab.certificate().hash()`, `.decisions()` | ok: `hash=0xd9bf65e7968c6bae decisions=7` |
| 6 | Foata canonical form, fingerprint, independence relation | `asupersync::trace::canonicalize::{canonicalize, trace_fingerprint}`, `FoataTrace::{depth, len, fingerprint}`, `asupersync::trace::independence::independent` | ok: `foata_depth=5 foata_len=11 independent_adjacent_pairs=7` |
| 7 | Primitives and combinators from inside lab tasks | `asupersync::channel::{mpsc, oneshot}` (`send`/`recv`/`reserve` + permit `send`), `Cx::spawn`, `cx.scope().join_all` / `race_all`, `lab.state.obligations_iter()` | ok: `join_all_sum=3 race_winner=3@0 reserve_commit=7 pending_obligations=0` |
| — | Seed determinism (unforced rerun, same seed) | as above | ok: `certificate_match=true trace_fingerprint_match=true` |

Not yet exercised by the spike: broadcast/watch channels, the sync primitives,
`Scope::first_ok` / `quorum`, actors and supervisors. They are `pub` at the
pinned rev (`src/channel/mod.rs`, `src/sync/mod.rs`, `src/cx/scope.rs:1811`,
`:2023`), and nothing in capabilities 1–7 suggests they behave differently;
W1.4 covers them with scenarios rather than another spike.

## Findings the oracle design must account for

1. **Channel traffic in the trace.** The 11 events were `RegionCreated=1,
   Spawn=3, Complete=3, UserTrace=2, ObligationReserve=1, ObligationCommit=1`.
   All four non-lifecycle events come from the **oneshot**, not the mpsc
   (source reading, `/dp/asupersync`): `oneshot::Sender::send` reserves a
   SendPermit obligation and traces `"oneshot::reserve creating permit"`
   (`src/channel/oneshot.rs:512`, `:528`), `permit.send` commits it (`:759`), and
   `recv` traces `"oneshot::recv received value"` (`:1041`). `mpsc::Sender::send`
   uses a transient reserve that registers no obligation and emits nothing
   (`src/channel/mpsc.rs:715-727`, `:1190-1201`); only `reserve()` +
   `permit.send()` produce `ObligationReserve` + `ObligationCommit`. Rust has no
   channel event kinds, and successful mpsc sends are invisible in the trace.
   The vocabulary (W1.2, bd-9kll.2.2) must therefore not compare channel traffic
   through trace events alone: channel effects need the end-of-scenario
   snapshot (queue lengths, reserved permits) or explicit scenario
   observations. (An earlier version of this finding attributed the events
   to the mpsc; that was wrong.)
2. **Scheduling decisions are not in the lab trace.** The lab runtime never
   pushes `Schedule`, `Poll`, `Yield`, `Wake` or `CancelAck` into its trace
   buffer; those kinds come only from the production three-lane scheduler's
   opt-in capture (`src/runtime/scheduler/three_lane.rs:8643-8691`). Dispatch
   order lives in the replay recorder, the schedule certificate and the
   forced-schedule receipt, which is why comparator mode (c) consumes the
   receipt rather than the trace.
3. **`race_all` winner.** With children `slow` (one `yield_now` then 3) and
   `fast` (returns 4), `race_all` returned `3` at index 0. The winner is decided
   by the lab's dispatch order and the combinator's polling order, not by "fewest
   polls". The oracle must capture this from the Rust run; C must not assume it.
4. **`Cx::spawn` works for `create_task` tasks.** The spawn gateway is wired in
   the lab, so scenarios can build task trees from inside task bodies; a
   `RuntimeUnavailable` error is not a concern for lab scenarios.
5. **Forced schedules are a portable artifact.** The canonical bytes (301 bytes
   for 7 dispatches) decode under explicit `ForcedScheduleDecodeLimits` and
   replay to the same certificate hash and trace fingerprint. They are the
   natural input for cross-engine forced replay (W1.6 mode (c), bd-9kll.4.8).
6. **Certificates are comparable across runs.** The certificate hash after a
   forced replay equals the original's, and an unforced same-seed rerun
   reproduces it, so certificate equality is a sound cheap check before the
   trace comparison.

## Build and fetch findings

- **Fetch.** rch workers fetch the git dependency from GitHub directly
  (`Updating git repository https://github.com/Dicklesworthstone/asupersync`).
  No vendoring or path patch is needed.
- **Build cost.** A cold debug build took 14m16s at `-j 2` (vmi1227854). A cold
  release build at `-j 3` did not finish within 30 minutes. Warm incremental
  builds are fast. Consequence for W1.4/W1.7: capture fixtures on demand (rch,
  or a scheduled CI job with a cached target dir), commit the captured
  fixtures and forced-schedule receipts, and let per-push CI replay them on the
  C side offline. Per-push CI should not compile asupersync.
- **rch defect.** After successful runs of this subdirectory crate, rch reported
  `Remote completion unconfirmed ... File exists (os error 17)` and retained
  the worker's source ownership; `rch jobs recover` and `rch jobs cancel`
  failed with the same error, and later jobs from this repo were refused on
  that worker (`unfinished overlapping source owner`). Workaround: pin
  `RCH_WORKER` to another worker. This belongs upstream in rch; it does not
  affect the go/no-go.

## Required design changes

None to W1.4's architecture. Refinements to apply to the beads:

- W1.4 (`tools/twin_run`): start from `tools/twin_run_spike`; scenario setup is
  `create_root_region` + `create_task` + `scheduler.lock().schedule`, task trees
  via `Cx::spawn`; capture `trace().snapshot()`, `certificate()`, and the
  forced-schedule receipt for every scenario.
- W1.2 (vocabulary): Rust has no channel event kinds and successful mpsc
  sends are not traced; compare channel effects through the snapshot and
  scenario observations (finding 1). Scheduling order is compared through the
  certificate and forced-schedule receipt, not the trace (finding 2).
- W1.6 (comparator): mode (b) uses `trace::canonicalize`; mode (c) consumes
  `ForcedSchedule` canonical bytes; check certificate equality first.
- W1.7 (corpus) and CI: capture off the per-push path; commit receipts.
