# Fuzz Parity Design — Differential Fuzz Grammar Specification

> Formal specification of the C differential fuzz harness (`fuzz_differential.c`)
> for exact reimplementation parity in the Rust port.
>
> Bead: bd-rkql.1
> SPDX-License-Identifier: MIT

---

## 1. Overview

The differential fuzz harness generates random operation sequences ("scenarios"),
executes them against the runtime, and verifies:

1. **Deterministic self-consistency** — same seed always produces the same semantic digest
2. **Mutation robustness** — mutated scenarios cause no crashes or UB
3. **Cross-implementation parity** — (future) C and Rust produce identical digests for identical seeds

The design is fully deterministic: every random decision flows from a single 64-bit seed
through a xoshiro256** PRNG. No heap allocation occurs during fuzzing.

---

## 2. PRNG: xoshiro256**

### Initialization (splitmix64)

Given a 64-bit `seed`, produce 4×64-bit state `s[0..3]`:

```
z = seed
for i in 0..4:
    z += 0x9e3779b97f4a7c15
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9
    z = (z ^ (z >> 27)) * 0x94d049bb133111eb
    s[i] = z ^ (z >> 31)
```

### Next (xoshiro256**)

```
result = rotl(s[1] * 5, 7) * 9
t = s[1] << 17
s[2] ^= s[0]
s[3] ^= s[1]
s[1] ^= s[2]
s[0] ^= s[3]
s[2] ^= t
s[3] = rotl(s[3], 45)
return result
```

Where `rotl(x, k) = (x << k) | (x >> (64 - k))`.

### Bounded u32

```
fuzz_rng_u32(rng, bound):
    if bound == 0: return 0
    return (uint32_t)(fuzz_rng_next(rng) % (uint64_t)bound)
```

---

## 3. Digest Algorithm: FNV-1a (64-bit)

### Constants

| Name   | Value                  |
|--------|------------------------|
| Offset | `0xcbf29ce484222325`   |
| Prime  | `0x100000001b3`        |

### Byte-level feed

```
hash ^= (uint64_t)byte
hash *= prime
```

### Multi-byte feed (little-endian decomposition)

| Function        | Decomposition                          |
|-----------------|----------------------------------------|
| `hasher_u32(v)` | Feed bytes: v[7:0], v[15:8], v[23:16], v[31:24] |
| `hasher_i32(v)` | Cast to u32, then `hasher_u32`         |
| `hasher_u64(v)` | `hasher_u32(v & 0xFFFFFFFF)`, then `hasher_u32(v >> 32)` |

### Digest construction order

For each scenario execution, the hasher is fed in this exact order:

```
1. hasher_u64(scenario.seed)
2. hasher_u32(scenario.op_count)
3. For each op i in 0..op_count:
   a. hasher_u32(op.kind)           # before execution
   b. [execute op]
   c. hasher_i32(result_status)     # after execution
4. hasher_u32(scheduler_event_count)
5. For each scheduler event ei in 0..event_count:
   a. hasher_u32(event.kind)
   b. hasher_u64(event.task_id)
   c. hasher_u32(event.sequence)
6. digest = hasher.hash
```

---

## 4. Operation Grammar

### 4.1 Op Structure

```c
typedef struct {
    fuzz_op_kind kind;      // enum 0..20
    uint32_t     idx_a;     // primary handle index
    uint32_t     idx_b;     // secondary handle index
    uint32_t     arg_u32;   // capacity, cancel_kind, poll_quota, etc
    uint64_t     arg_u64;   // time, deadline, value
} fuzz_op;
```

### 4.2 Op Kinds (21 total)

| ID | Kind                  | Weight | Category        |
|----|-----------------------|--------|-----------------|
|  0 | `SpawnRegion`         |   12   | Region          |
|  1 | `CloseRegion`         |    8   | Region          |
|  2 | `PoisonRegion`        |    3   | Region          |
|  3 | `SpawnTask`           |   15   | Task            |
|  4 | `CancelTask`          |   10   | Task            |
|  5 | `ReserveObligation`   |    8   | Obligation      |
|  6 | `CommitObligation`    |    7   | Obligation      |
|  7 | `AbortObligation`     |    5   | Obligation      |
|  8 | `ChannelCreate`       |    6   | Channel         |
|  9 | `ChannelReserve`      |    5   | Channel         |
| 10 | `ChannelSend`         |    5   | Channel         |
| 11 | `ChannelAbort`        |    3   | Channel         |
| 12 | `ChannelRecv`         |    5   | Channel         |
| 13 | `ChannelCloseTx`      |    3   | Channel         |
| 14 | `ChannelCloseRx`      |    3   | Channel         |
| 15 | `TimerRegister`       |    6   | Timer           |
| 16 | `TimerCancel`         |    4   | Timer           |
| 17 | `AdvanceTime`         |    5   | Timer           |
| 18 | `SchedulerRun`        |   12   | Scheduler       |
| 19 | `RegionDrain`         |    5   | Scheduler       |
| 20 | `QuiescenceCheck`     |    4   | Scheduler       |

**Total weight: 134**

Selection: accumulate weights, pick uniformly in `[0, total)`, scan for threshold.

### 4.3 Subsystem Coverage

| Subsystem      | Ops                                                         | Count |
|----------------|-------------------------------------------------------------|-------|
| Region         | SpawnRegion, CloseRegion, PoisonRegion, RegionDrain         | 4     |
| Task           | SpawnTask, CancelTask                                       | 2     |
| Obligation     | ReserveObligation, CommitObligation, AbortObligation        | 3     |
| Channel        | Create, Reserve, Send, Abort, Recv, CloseTx, CloseRx       | 7     |
| Timer          | TimerRegister, TimerCancel, AdvanceTime                     | 3     |
| Scheduler      | SchedulerRun, QuiescenceCheck                               | 2     |

---

## 5. Scenario Generation

```
generate_scenario(rng, max_ops):
    scenario.seed = rng_next(rng)
    n = 4 + rng_u32(rng, max_ops > 4 ? max_ops - 4 : 1)
    n = min(n, 128)
    scenario.op_count = n

    # First op is always SpawnRegion (zeroed args)
    ops[0] = { kind: SpawnRegion, idx_a: 0, idx_b: 0, arg_u32: 0, arg_u64: 0 }

    for i in 1..n:
        ops[i].kind    = pick_op(rng)         # weighted selection
        ops[i].idx_a   = rng_u32(rng, MAX_REGIONS)   # MAX_REGIONS from ASX config
        ops[i].idx_b   = rng_u32(rng, MAX_TASKS)     # MAX_TASKS from ASX config
        ops[i].arg_u32 = rng_u32(rng, 32)
        ops[i].arg_u64 = rng_next(rng) % 10000
```

### Constraints

| Name            | Value |
|-----------------|-------|
| `FUZZ_MAX_OPS`  | 128   |
| `MAX_REGIONS`   | `ASX_MAX_REGIONS` (compile-time) |
| `MAX_TASKS`     | `ASX_MAX_TASKS` (compile-time)   |
| `MAX_OBLIGATIONS` | 64  |
| `MAX_CHANNELS`  | `ASX_MAX_CHANNELS` (compile-time) |
| `MAX_TIMERS`    | 32    |

---

## 6. Mutation Engine

### 6.1 Mutation Kinds (7 total)

| ID | Kind           | Description                                              |
|----|----------------|----------------------------------------------------------|
|  0 | `remove_op`    | Remove random op (not op[0]). Shifts remaining ops left. |
|  1 | `duplicate_op` | Copy random op to end (if room).                         |
|  2 | `swap_ops`     | Swap two random ops (neither is op[0]).                  |
|  3 | `change_kind`  | Replace random op's kind via weighted selection.         |
|  4 | `tweak_arg`    | Mutate one op's arg_u32 or arg_u64 (3 sub-variants).    |
|  5 | `insert_op`    | Insert freshly-generated op at random position (if room).|
|  6 | `change_idx`   | Randomize idx_a or idx_b of one op.                      |

Selection: uniform random in `[0, 7)`.

### 6.2 Tweak Arg Sub-variants

Selected by `rng_u32(rng, 3)`:

| Sub | Effect                                  |
|-----|-----------------------------------------|
|  0  | `arg_u32 = rng_u32(rng, 128)`          |
|  1  | `arg_u64 = rng_next(rng) % 100000`     |
|  2  | `arg_u32 ^= (1 << rng_u32(rng, 32))`  |

### 6.3 Trivial Scenario Guard

If `op_count < 2`, mutation degenerates to `tweak_arg` on op[0] only:
`ops[0].arg_u32 = rng_u32(rng, 64)`.

---

## 7. Execution Model

### 7.1 Handle Tracking State

```
regions[MAX_REGIONS]          + region_count
tasks[MAX_TASKS]              + task_count
obligations[64]               + obligation_count
channels[MAX_CHANNELS]        + channel_count
permits[MAX_CHANNELS]         + permit_count
timers[32]                    + timer_count
sim_time: uint64              (monotonically increasing)
```

### 7.2 Per-Op Execution Semantics

Each op indexes into existing handles using modular arithmetic:
`idx = op.idx_a % handle_count` (or produces `ASX_E_NOT_FOUND` if count == 0).

| Op                | Handle Source             | Arg Interpretation                        |
|-------------------|---------------------------|-------------------------------------------|
| SpawnRegion       | —                         | Creates new region_id                     |
| CloseRegion       | regions[idx_a % count]    | —                                         |
| PoisonRegion      | regions[idx_a % count]    | —                                         |
| SpawnTask         | regions[idx_a % count]    | polls = 1 + (arg_u32 % 16)               |
| CancelTask        | tasks[idx_b % count]      | kind = arg_u32 % 11                       |
| ReserveObligation | regions[idx_a % count]    | Creates new obligation_id                 |
| CommitObligation  | obligations[idx_a % count]| —                                         |
| AbortObligation   | obligations[idx_a % count]| —                                         |
| ChannelCreate     | regions[idx_a % count]    | cap = 1 + (arg_u32 % (MAX_CAP - 1))      |
| ChannelReserve    | channels[idx_a % count]   | Creates new send_permit                   |
| ChannelSend       | permits[idx_a % count]    | value = arg_u64                           |
| ChannelAbort      | permits[idx_a % count]    | —                                         |
| ChannelRecv       | channels[idx_a % count]   | Receives into local var                   |
| ChannelCloseTx    | channels[idx_a % count]   | —                                         |
| ChannelCloseRx    | channels[idx_a % count]   | —                                         |
| TimerRegister     | —                         | deadline = sim_time + 1 + (arg_u64 % 5000)|
| TimerCancel       | timers[idx_a % count]     | —                                         |
| AdvanceTime       | —                         | advance = 1 + (arg_u64 % 2000)           |
| SchedulerRun      | regions[idx_a % count]    | quota = 1 + (arg_u32 % 64)               |
| RegionDrain       | regions[idx_a % count]    | quota = 256 (fixed)                       |
| QuiescenceCheck   | regions[idx_a % count]    | —                                         |

### 7.3 Task Poll Function

Tasks use a static pool of `fuzz_task_state` structs (max `MAX_TASKS`).
Each has `polls_remaining` set at spawn. Poll function:

```
poll(state, self_id):
    if state.polls_remaining > 0:
        state.polls_remaining -= 1
        return ASX_E_PENDING
    return ASX_OK
```

### 7.4 Runtime Reset

Before each execution:
```
asx_runtime_reset()
asx_channel_reset()
asx_timer_wheel_reset(global_wheel)
fuzz_reset_task_states()
```

### 7.5 Digest Finalization

After all ops, the scheduler event log is hashed:
```
for each event in scheduler_event_log:
    hash(event.kind, event.task_id, event.sequence)
```

This captures side effects beyond return codes (scheduling order, task state transitions).

---

## 8. Main Fuzz Loop

```
for iter in 0..iterations:
    scenario = generate_scenario(rng, max_ops)

    # Phase 1: Determinism self-check
    exec_a = execute(scenario)
    exec_b = execute(scenario)
    assert exec_a.digest == exec_b.digest

    # Phase 2: Mutation + crash detection
    mutated = copy(scenario)
    for m in 0..mutations_per_scenario:
        mutate(rng, mutated)

    exec_c = execute(mutated)
    exec_d = execute(mutated)
    assert exec_c.digest == exec_d.digest   # mutant determinism
```

### Configuration Defaults

| Parameter               | Default | Smoke  | Nightly |
|-------------------------|---------|--------|---------|
| `iterations`            | 1000    | 100    | 100000  |
| `max_ops`               | 64      | 32     | 96      |
| `mutations_per_scenario`| 4       | 4      | 8       |

---

## 9. JSONL Reporting Contract

All output is JSONL (one JSON object per line).

### Mismatch Record

```json
{
  "kind": "determinism_failure" | "mutant_determinism_failure",
  "iteration": <u64>,
  "seed": <u64>,
  "digest_a": "<16-hex>",
  "digest_b": "<16-hex>",
  "mutations": [{"kind": "<name>", "target": <u32>, "secondary": <u32>}],
  "ops": [{"op": "<name>", "idx_a": <u32>, "idx_b": <u32>,
           "arg_u32": <u32>, "arg_u64": <u64>, "result": <i32>}]
}
```

### Summary Record

```json
{
  "kind": "summary",
  "initial_seed": <u64>,
  "iterations": <u64>,
  "determinism_failures": <u64>,
  "crashes": <u64>,
  "duration_sec": <f64>,
  "iterations_per_sec": <f64>
}
```

---

## 10. Coverage Gap Analysis

### Currently covered

- Region lifecycle: open/close/poison/drain (full lifecycle)
- Task lifecycle: spawn/cancel (11 cancel kinds tested)
- Obligation lifecycle: reserve/commit/abort (full lifecycle)
- Channel lifecycle: create/reserve/send/abort/recv/close_tx/close_rx (full lifecycle)
- Timer lifecycle: register/cancel/advance_time with expiry collection
- Scheduler: run with budget, quiescence check
- Cross-subsystem: tasks in regions, obligations in regions, channels in regions

### Not yet covered (potential additions for parity)

| Gap                        | Description                                           | Priority |
|----------------------------|-------------------------------------------------------|----------|
| Budget exhaustion paths    | Only infinite budget with poll_quota used; no cost or deadline exhaustion | Medium |
| Cancel strengthen chains   | CancelTask uses raw cancel_kind, no multi-level strengthen testing | Low |
| Symbol registry operations | New symbol subsystem has no fuzz coverage             | Low |
| Typed symbol mismatches    | Type mismatch detection not exercised by fuzzer       | Low |
| Ghost/ownership operations | Resource/ownership APIs not in fuzz grammar           | Medium |
| Admission gate             | No admission gate capacity testing under fuzz         | Low |
| Error ledger               | No error reporting/counting exercised                 | Low |

### Recommendations for Rust parity

1. The PRNG, hasher, and digest construction order must be **byte-identical** between C and Rust implementations
2. The weight table values and selection algorithm must match exactly
3. Scenario generation arg ranges (% 10000, % 32, etc.) must be preserved
4. Handle tracking modular indexing semantics must match
5. Runtime reset sequence must be equivalent
6. Scheduler event log hashing must include the same fields in the same order

---

## 11. File Reference

| File | Purpose |
|------|---------|
| `tests/fuzz/fuzz_differential.c` | C reference implementation (1149 lines) |
| `tests/fuzz/fuzz_minimize.c` | Scenario minimizer (companion tool) |
| `tests/fuzz/test_harness_sanity.sh` | Automated sanity checks for fuzz binaries |
| `build_asan.sh` | ASan/UBSan build script for fuzz harnesses |
