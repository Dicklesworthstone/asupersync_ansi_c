# Fuzz Harness Audit Report

> **Bead:** bd-rkql.2
> **Auditor:** AmberCrane (claude-code/opus-4.6)
> **Date:** 2026-03-12
> **Files reviewed:**
>   - `tests/fuzz/fuzz_differential.c` (1149 lines)
>   - `tests/fuzz/fuzz_minimize.c` (1080 lines)
>   - `docs/FUZZ_PARITY_DESIGN.md` (grammar spec)

---

## 1. Grammar Completeness Audit

### 1.1 Op Coverage Checklist (21/21 present)

| ID | Op Kind | Exercises Meaningful Behavior | Notes |
|----|---------|-------------------------------|-------|
| 0 | SpawnRegion | Yes | Allocates real region slot |
| 1 | CloseRegion | Yes | Transitions region state |
| 2 | PoisonRegion | Yes | Marks region poisoned |
| 3 | SpawnTask | Yes | Creates task with poll fn + allocates from static pool |
| 4 | CancelTask | Yes | Exercises all 11 cancel kinds (0-10) |
| 5 | ReserveObligation | Yes | Creates obligation in region |
| 6 | CommitObligation | Yes | Resolves obligation |
| 7 | AbortObligation | Yes | Aborts obligation |
| 8 | ChannelCreate | Yes | Creates channel with bounded capacity |
| 9 | ChannelReserve | Yes | Acquires send permit |
| 10 | ChannelSend | Yes | Sends value through permit |
| 11 | ChannelAbort | Yes | Aborts reserved permit |
| 12 | ChannelRecv | Yes | Receives from channel |
| 13 | ChannelCloseTx | Yes | Closes sender side |
| 14 | ChannelCloseRx | Yes | Closes receiver side |
| 15 | TimerRegister | Yes | Registers timer with future deadline |
| 16 | TimerCancel | Yes | Generation-safe cancel |
| 17 | AdvanceTime | Yes | Advances sim_time and collects expired timers |
| 18 | SchedulerRun | Yes | Runs scheduler with budget+poll_quota |
| 19 | RegionDrain | Yes | Drains region with fixed 256-poll budget |
| 20 | QuiescenceCheck | Yes | Checks region quiescence |

### 1.2 Cancel Kind Coverage

`op->arg_u32 % 11u` generates values 0-10, matching exactly:
- ASX_CANCEL_USER (0) through ASX_CANCEL_SHUTDOWN (10)

**Verdict: Correct.** All 11 cancel kinds are reachable.

### 1.3 Weight Bias Assessment

Total weight: 134. Distribution:

| Category | Total Weight | % of Total | Assessment |
|----------|-------------|-----------|------------|
| Region (4 ops) | 28 | 20.9% | Good — lifecycle is core |
| Task (2 ops) | 25 | 18.7% | Good — heavily exercised |
| Obligation (3 ops) | 20 | 14.9% | Adequate |
| Channel (7 ops) | 30 | 22.4% | Good — most complex subsystem |
| Timer (3 ops) | 15 | 11.2% | Adequate |
| Scheduler (2 ops) | 16 | 11.9% | Adequate |

**Verdict: Reasonable.** No subsystem is severely under-represented. SpawnTask (15) and SpawnRegion (12) have the highest weights, which is appropriate since they create the entities other ops consume.

### 1.4 Channel Backpressure Scenarios

The grammar CAN create full-channel situations:
- ChannelCreate allocates with capacity `1 + (arg_u32 % 63)` (range 1-63)
- ChannelReserve acquires permits; enough reserves will exhaust capacity
- ChannelSend through permits fills the queue

However, the fuzzer does NOT explicitly create backpressure-specific scenarios. With the current weight distribution, the probability of naturally creating a full channel is moderate — capacity ranges up to 63 but permits and sends need to accumulate. This is a **coverage gap** but not a bug.

### 1.5 Profile-Specific Code Paths

**Not exercised.** The harness compiles against whatever profile the Makefile builds. It does not switch profiles or exercise FREESTANDING/EMBEDDED_ROUTER-specific paths within a single run. This is expected — profile switching requires separate compilation, not runtime switching.

---

## 2. Handle Tracking Correctness

### 2.1 Slot Exhaustion Handling

| Handle Type | Max Slots | Exhaustion Check | Verdict |
|-------------|-----------|------------------|---------|
| Regions | FUZZ_MAX_REGIONS (= ASX_MAX_REGIONS = 8) | `hs.region_count < FUZZ_MAX_REGIONS` | Correct |
| Tasks | FUZZ_MAX_TASKS (= ASX_MAX_TASKS = 64) | `hs.task_count < FUZZ_MAX_TASKS` | Correct |
| Obligations | FUZZ_MAX_OBLIGATIONS (= 64) | `hs.obligation_count < FUZZ_MAX_OBLIGATIONS` | Correct |
| Channels | FUZZ_MAX_CHANNELS (= ASX_MAX_CHANNELS = 16) | `hs.channel_count < FUZZ_MAX_CHANNELS` | Correct |
| Permits | FUZZ_MAX_CHANNELS (= 16) | `hs.permit_count < FUZZ_MAX_CHANNELS` | See note |
| Timers | FUZZ_MAX_TIMERS (= 32) | `hs.timer_count < FUZZ_MAX_TIMERS` | Correct |

**Permit pool sizing note:** Permits share the `FUZZ_MAX_CHANNELS` (16) limit. Since each channel can have capacity up to 63, and multiple reserves per channel are possible, 16 permit slots could be a bottleneck. However, the constraint is that you need a channel first (max 16), and the fuzzer creates at most one permit per ChannelReserve op. This is adequate for fuzz coverage.

### 2.2 Handle Reuse / Stale Handle Races

**No stale-handle bugs found.** The harness appends handles to arrays (regions[], tasks[], etc.) and never removes them. This means:
- CloseRegion on `regions[idx]` may close an already-closed region (runtime returns error — safe)
- CancelTask on `tasks[idx]` may cancel an already-cancelled task (runtime returns error — safe)
- CommitObligation/AbortObligation may operate on already-resolved obligations (runtime returns error — safe)

The handle arrays grow monotonically and are never compacted. This is intentional and correct — it allows stale-handle operations to be tested naturally.

### 2.3 sim_time Advancement

`sim_time` is monotonically increasing: `hs.sim_time += 1 + (arg_u64 % 2000)`. Timer deadlines are set relative to current sim_time: `deadline = sim_time + 1 + (arg_u64 % 5000)`. This is correct — timers always fire in the future relative to their registration time.

**Observation:** The sim_time is local to the handle state and only used for timer deadlines. The timer wheel's internal time is advanced via `asx_timer_collect_expired(wheel, sim_time, ...)`. This means sim_time and the timer wheel are properly synchronized.

### 2.4 Channel Permit Lifecycle

- **Reserve→Send path:** ChannelReserve acquires permit, ChannelSend sends through it. Both use `idx_a % count` to select. A permit can be sent multiple times if the same index is selected again (runtime should return error for already-sent permit — safe).
- **Reserve→Abort path:** ChannelReserve then ChannelAbort. ChannelAbort calls `asx_send_permit_abort()` which releases the reserved slot. A double-abort on the same permit is possible via modular indexing (runtime handles gracefully).
- **No permit removal from array:** After send or abort, the permit stays in the permits[] array. Subsequent ChannelSend/ChannelAbort on the same index will operate on a consumed/aborted permit. This tests the runtime's error handling for invalid-state permits.

**Verdict: Correct.** All paths are reachable and safe.

---

## 3. Mutation Engine Review

### 3.1 Mutation Operator Assessment

| ID | Operator | Meaningful? | Produces Valid Scenarios? | Notes |
|----|----------|-------------|--------------------------|-------|
| 0 | remove_op | Yes | Yes | Never removes op[0] (SpawnRegion). Correctly shifts remaining ops. |
| 1 | duplicate_op | Yes | Yes | Copies random op to end. Bounds-checked against FUZZ_MAX_OPS. |
| 2 | swap_ops | Yes | Yes | Swaps two ops (neither is op[0]). Identity swap (idx==idx2) is handled. |
| 3 | change_kind | Yes | Yes | Replaces op kind via weighted selection. |
| 4 | tweak_arg | Yes | Yes | Three sub-variants for arg mutation. |
| 5 | insert_op | Yes | Yes | Inserts fresh op at random position (>= 1). Bounds-checked. |
| 6 | change_idx | Yes | Yes | Randomizes idx_a or idx_b of one op. |

### 3.2 Bounds Checking

- **idx_a/idx_b generation:** `fuzz_rng_u32(rng, FUZZ_MAX_REGIONS)` for idx_a, `fuzz_rng_u32(rng, FUZZ_MAX_TASKS)` for idx_b. At execution time, these are modular-indexed against actual handle counts. No out-of-bounds possible.
- **arg_u32:** Generated as `fuzz_rng_u32(rng, 32)` (range 0-31). At execution time, further bounded per op (e.g., `% 11u` for cancel kind, `% 16u` for poll count). Safe.
- **arg_u64:** Generated as `fuzz_rng_next(rng) % 10000` (range 0-9999). At execution time, further bounded per op. Safe.
- **Mutation change_idx:** `fuzz_rng_u32(rng, FUZZ_MAX_REGIONS)` or `fuzz_rng_u32(rng, FUZZ_MAX_TASKS)` — same range as generation. Safe.
- **Mutation insert_op:** `idx = 1 + fuzz_rng_u32(rng, sc->op_count)` — this can produce `idx = sc->op_count` when the random value equals `sc->op_count - 1`. The subsequent check `if (idx > sc->op_count) idx = sc->op_count` handles the edge case. Safe.

### 3.3 duplicate_op Noop memmove

Line 429-430:
```c
memmove(&sc->ops[sc->op_count + 1u], &sc->ops[sc->op_count], 0u);
```

This memmove with size 0 is a **dead code / noop**. It doesn't actually shift anything — the duplicate is just placed at `sc->ops[sc->op_count]`. This is correct behavior (append to end) but the memmove call is misleading dead code. **Minor code quality issue, not a bug.**

---

## 4. Digest Computation Review

### 4.1 FNV-1a Determinism

The FNV-1a implementation is:
- **Platform-independent:** Uses fixed 64-bit constants, little-endian byte decomposition via explicit shifts. No platform-dependent behavior.
- **Deterministic hash chain order:** Documented in FUZZ_PARITY_DESIGN.md §3. Feed order: seed → op_count → (for each op: kind → result) → event_count → (for each event: kind → task_id → sequence).

**Verdict: Deterministic and portable.**

### 4.2 Semantically Complete Digest

The digest includes:
- Scenario seed and op count (input identification)
- Each op's kind and execution result status code (per-op behavior)
- Scheduler event count, kinds, task IDs, and sequence numbers (side effects)

**Not included (correctly):**
- Handle values (region_id, task_id values — these are allocation-dependent but deterministic)
- Timing values (sim_time — local to fuzzer, not runtime state)
- Pointer addresses (no pointers in digest)

**Observation:** The digest does NOT include the received channel values or timer fire information beyond what's in scheduler events. This means two executions that differ only in channel payload content would produce the same digest. This is acceptable because the fuzzer's goal is behavioral determinism (same status codes + same scheduling order), not content verification.

### 4.3 Missing `asx_scheduler_event_reset()` in fuzz_differential.c

**Not a bug.** `asx_runtime_reset()` (called at line 522) internally calls `asx_scheduler_event_reset()` at `src/runtime/lifecycle.c:101`. The explicit calls in `fuzz_minimize.c` are redundant but harmless.

### 4.4 Redundant Reset Calls

`fuzz_execute()` calls:
1. `asx_runtime_reset()` — which internally resets scheduler events, channels, timers, and more
2. `asx_channel_reset()` — redundant (already called by #1)
3. `asx_timer_wheel_reset(asx_timer_wheel_global())` — redundant (already called by #1)

**Minor code quality issue.** These redundant calls are harmless but could be cleaned up.

---

## 5. Minimizer Review (fuzz_minimize.c)

### 5.1 Code Duplication

`fuzz_minimize.c` duplicates all type definitions, PRNG, hasher, op enum, and execution logic from `fuzz_differential.c` with `min_` prefix. The comment at line 39 acknowledges this. This is a maintainability concern — any bug fix or op addition in one file must be manually propagated to the other.

**Recommendation:** Extract shared types and functions into a `fuzz_common.h` header.

### 5.2 Minimization Correctness

Three strategies are implemented:
1. **Delta debugging (chunk removal):** Correctly tries progressively smaller chunks, doesn't remove op[0].
2. **Single op removal:** Correctly iterates backwards to preserve indices.
3. **Argument simplification:** Tries zeroing indices and reducing args to 1. Correct.

All three verify via `min_preserves_failure()` before accepting changes.

### 5.3 Self-Tests

Three self-tests cover:
1. DIGEST_MATCH mode (op removal limited, arg simplification expected)
2. Argument simplification verification
3. PREDICATE mode with actual op removal

All three have proper assertions. **Well-tested.**

### 5.4 Incomplete CLI

The minimizer's `--scenario` and `--scenario-file` CLI flags are listed in the help text but NOT implemented (line 1075: "scenario input from stdin not yet implemented"). This is a known limitation, not a bug.

---

## 6. Summary of Findings

### Bugs Found: 0

No correctness bugs were identified in either file.

### Code Quality Issues: 3

| ID | File | Line | Issue | Severity |
|----|------|------|-------|----------|
| CQ-1 | fuzz_differential.c | 522-524 | Redundant `asx_channel_reset()` and `asx_timer_wheel_reset()` calls (already done by `asx_runtime_reset()`) | Low |
| CQ-2 | fuzz_differential.c | 429-430 | Noop `memmove(..., 0u)` in duplicate_op — misleading dead code | Low |
| CQ-3 | fuzz_minimize.c | all | Full type/function duplication from fuzz_differential.c | Medium |

### Coverage Gaps: 3

| ID | Gap | Description | Priority |
|----|-----|-------------|----------|
| GAP-1 | Budget exhaustion | Only `poll_quota` exercised; no `deadline` or `cost_quota` exhaustion paths | Medium |
| GAP-2 | Channel backpressure | Full-channel scenarios rely on random accumulation; no deliberate backpressure forcing | Low |
| GAP-3 | Profile-specific paths | No cross-profile coverage within fuzzer (expected — requires separate builds) | Low |

### Design Concerns: 1

| ID | Concern | Description |
|----|---------|-------------|
| DC-1 | `--fixtures-dir` flag unused | fuzz_differential.c accepts `--fixtures-dir` but never uses it for Rust comparison. This is documented as future work (bd-rkql.4). |

### Acceptance Criteria Checklist

- [x] All 21 op types reviewed for semantic coverage — documented in §1.1
- [x] Handle tracking logic reviewed — no out-of-bounds or use-after-free patterns (§2)
- [x] Mutation engine reviewed — all 7 operators produce valid transformations (§3)
- [x] Digest computation reviewed — confirmed deterministic and semantically complete (§4)
- [x] Findings documented: 0 bugs, 3 code quality issues, 3 coverage gaps, 1 design concern
- [x] No P0 bugs to file
