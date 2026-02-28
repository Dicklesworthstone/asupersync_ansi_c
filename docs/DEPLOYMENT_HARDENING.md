# Deployment Hardening Playbooks

Operator runbooks and golden domain scenario packs for verifying ASX
deployment readiness across embedded router, HFT, and automotive profiles.

## Scenario Pack Overview

Three deployment hardening scenario packs exercise domain-specific
stress patterns. Each pack produces deterministic trace digests and
structured JSONL manifests for CI consumption.

| Pack | Profile | Gate ID | Scenarios | Focus |
|------|---------|---------|-----------|-------|
| Router Storm | EMBEDDED_ROUTER | GATE-E2E-DEPLOY-ROUTER | 7 | Region churn, exhaustion, poison isolation |
| Market Open Burst | HFT | GATE-E2E-DEPLOY-HFT | 7 | Admission spike, overload, mass cancel |
| Automotive Fault Burst | AUTOMOTIVE | GATE-E2E-DEPLOY-AUTO | 7 | Fault injection cascade, containment |

## Running Scenario Packs

### Prerequisites

Build the library for the target profile:

```bash
make build PROFILE=EMBEDDED_ROUTER   # for router-storm
make build PROFILE=HFT               # for market-open-burst
make build PROFILE=AUTOMOTIVE         # for automotive-fault-burst
```

### Single Pack Execution

```bash
# Router storm (embedded router high-churn scenario)
ASX_E2E_PROFILE=EMBEDDED_ROUTER ./tests/e2e/router_storm.sh

# Market open burst (HFT extreme admission spike)
ASX_E2E_PROFILE=HFT ./tests/e2e/market_open_burst.sh

# Automotive fault burst (fault injection cascade)
ASX_E2E_PROFILE=AUTOMOTIVE ./tests/e2e/automotive_fault_burst.sh
```

### Full Suite Execution

```bash
./tests/e2e/run_all.sh
```

### Environment Variables

| Variable | Default | Purpose |
|----------|---------|---------|
| `ASX_E2E_SEED` | 42 | Deterministic seed for replay |
| `ASX_E2E_PROFILE` | CORE | Profile under test |
| `ASX_E2E_CODEC` | json | Codec under test |
| `ASX_E2E_RESOURCE_CLASS` | R3 | Resource class (R1/R2/R3) |
| `ASX_E2E_SCENARIO_PACK` | all | Filter scenarios by prefix |
| `ASX_E2E_VERBOSE` | 0 | Verbose output |

### Reproducing Failures

Every failure includes a rerun command in the manifest:

```bash
ASX_E2E_SEED=42 ASX_E2E_PROFILE=EMBEDDED_ROUTER ASX_E2E_RESOURCE_CLASS=R2 \
  ./tests/e2e/router_storm.sh
```

## Router Storm Pack

**Profile**: EMBEDDED_ROUTER (R1/R2/R3 resource classes)
**Policy ID**: DEPLOY-ROUTER-STORM

### Scenarios

| ID | Exercise | Acceptance |
|----|----------|------------|
| router-storm-001.region_churn | Rapid region open/close cycles | >= 8 churn cycles complete |
| router-storm-002.multi_region_saturation | Concurrent task load across regions | All regions drain cleanly |
| router-storm-003.exhaustion_handling | Task arena saturation | Region stays OPEN, drain + close succeeds |
| router-storm-004.obligation_churn | Reserve/commit/abort under pressure | All obligations resolve |
| router-storm-005.poison_isolation | Fault containment via region poison | Healthy region unaffected |
| router-storm-006.cancel_under_load | Cancel propagation during saturation | All tasks reach completion |
| router-storm-007.trace_deterministic | Replay digest stability | Identical digests across runs |

### Operator Guidance

**Watchdog/Restart**: If a router-storm scenario fails in production,
check the region state via `asx_region_get_state()`. If the region is
OPEN, the runtime is still healthy and can accept new work after
draining. If poisoned, the region must be abandoned and a new region
opened.

**Safe Upgrade**: Before upgrading, run the router-storm pack with the
current resource class. Compare the trace digest against the known-good
baseline. A digest mismatch indicates semantic drift.

**Rollback**: If the post-upgrade digest diverges, revert to the
previous binary and re-run the pack. The deterministic seed ensures
identical execution paths for comparison.

## Market Open Burst Pack

**Profile**: HFT (busy-spin wait policy)
**Policy ID**: DEPLOY-MARKET-BURST

### Scenarios

| ID | Exercise | Acceptance |
|----|----------|------------|
| market-open-burst-001.admission_spike | Extreme task spawn rate | Capacity limit hit, clean rejection |
| market-open-burst-002.burst_obligations | Interleaved obligation lifecycle | All obligations committed or aborted |
| market-open-burst-003.partial_drain | Tight budget under spike | Budget exhaustion, then full drain |
| market-open-burst-004.burst_recovery | Saturate-drain-verify cycle | Region healthy after burst |
| market-open-burst-005.mass_cancel_burst | Cancel all during active burst | All tasks reach COMPLETED |
| market-open-burst-006.multi_region_isolation | Cross-region independence under load | Regions operate independently |
| market-open-burst-007.trace_deterministic | Replay digest stability | Identical digests across runs |

### Operator Guidance

**Overload Response**: When `ASX_E_RESOURCE_EXHAUSTED` is returned,
the runtime is at capacity. New work is deterministically rejected.
The region remains healthy; no recovery action is needed beyond
draining existing tasks.

**Budget Tuning**: Use `asx_budget_from_polls(N)` to control scheduler
progress per invocation. Under burst conditions, a tight budget
prevents runaway scheduling. Budget exhaustion is reported via
`ASX_E_POLL_BUDGET_EXHAUSTED`.

**Diagnostics**: The trace digest provides a fingerprint of the
execution. In HFT deployments, compare the digest against the
known-good baseline after each firmware update. Any deviation indicates
a behavioral change that requires investigation.

## Automotive Fault Burst Pack

**Profile**: AUTOMOTIVE (sleep wait policy)
**Policy ID**: DEPLOY-AUTO-FAULT

### Scenarios

| ID | Exercise | Acceptance |
|----|----------|------------|
| auto-fault-burst-001.clock_skew | Clock skew injection (5000ns per read) | Operations complete under skew |
| auto-fault-burst-002.clock_reversal | Clock reversal injection | Runtime tolerates time regression |
| auto-fault-burst-003.entropy_const | Constant entropy injection | Deterministic completion |
| auto-fault-burst-004.multi_fault_cascade | Compound clock + entropy faults | Runtime functional under cascade |
| auto-fault-burst-005.fault_containment | Poison isolation after fault detection | Healthy region unaffected |
| auto-fault-burst-006.deadline_under_fault | Deadline cancel under active skew | Task completes via checkpoint |
| auto-fault-burst-007.trace_deterministic | Replay digest stability | Identical digests across runs |

### Operator Guidance

**Fault Detection**: Inject controlled faults during pre-deployment
testing using `asx_fault_inject()`. The fault injection subsystem
supports:
- Clock skew (simulates oscillator drift)
- Clock reversal (simulates NTP correction)
- Constant entropy (forces deterministic randomness)
- Allocation failure (simulates memory pressure)

**Containment**: When a fault is detected in production, poison the
affected region via `asx_region_poison()`. This isolates the fault
while other regions continue operating. Poisoned regions reject
`asx_task_spawn()` with `ASX_E_REGION_POISONED`.

**Deadline Management**: Under fault conditions, cooperative tasks
should checkpoint via `asx_checkpoint()` and respond to cancellation.
Non-cooperative tasks are force-completed after the cleanup budget
expires.

**Post-Incident**: After clearing faults with `asx_fault_clear()`,
verify recovery by running the fault-burst pack and comparing the
trace digest against the known-good baseline.

## Remote Diagnostics (Constrained Links)

For embedded deployments with limited bandwidth:

1. **Trace digest only**: Compare the 64-bit digest remotely without
   transferring the full trace. A match confirms identical behavior.

2. **JSONL manifest**: The structured manifest (`*.summary.json`) is
   compact and contains scenario pass/fail, digests, and rerun
   commands.

3. **Selective retrieval**: Use `ASX_E2E_SCENARIO_PACK=router-storm-001`
   to re-run only the failing scenario and retrieve its artifacts.

4. **Resource class stepping**: Run with R1 (tight) on constrained
   targets, R2 (balanced) on typical hardware, R3 (roomy) on
   server-crossover. The pack validates identical semantics across
   all classes.

## Evidence Linkage

Each playbook lane maps to unit/invariant/e2e obligations:

| Lane | Unit Tests | Invariant | E2E Gate |
|------|-----------|-----------|----------|
| Router Storm | test_profile_compat (55), test_overload_catalog (33) | test_lifecycle_legality (18) | GATE-E2E-DEPLOY-ROUTER |
| Market Burst | test_hft_instrument (34), test_profile_compat (55) | test_lifecycle_legality (18) | GATE-E2E-DEPLOY-HFT |
| Auto Fault | test_automotive_instrument, test_profile_compat (55) | test_lifecycle_legality (18) | GATE-E2E-DEPLOY-AUTO |

Parity evidence: All three packs produce deterministic digests
validated by `GATE-E2E-CONTINUITY` and `profile-parity` CI gates.

## Artifact Structure

```
build/e2e-artifacts/{run_id}/
  router-storm.summary.json
  router-storm.stderr
  market-open-burst.summary.json
  market-open-burst.stderr
  automotive-fault-burst.summary.json
  automotive-fault-burst.stderr
  run_manifest.json

build/test-logs/
  e2e-router-storm.jsonl
  e2e-market-open-burst.jsonl
  e2e-automotive-fault-burst.jsonl
```
