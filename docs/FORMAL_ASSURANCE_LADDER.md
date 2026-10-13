# Formal Assurance Ladder

> **Bead:** bd-3vt.1
> **Status:** Canonical design artifact
> **Last updated:** 2026-03-01 by NobleCanyon

This document defines a staged formal assurance ladder for the asupersync
ANSI C runtime. Each level builds on the previous, with explicit entry/exit
criteria, cost/benefit analysis, and integration paths.

## 1. Ladder Overview

| Level | Name | Technique | Status | Cost | Value |
|-------|------|-----------|--------|------|-------|
| L0 | Golden Output Matching | Fixture-based parity against Rust reference | **Deployed** | Low | High — catches behavioral drift |
| L1 | Exhaustive Bounded Enumeration | Compile-and-enumerate all state pairs | **Deployed** | Low | High — proves small FSM properties |
| L2 | Bounded Model Checking | CBMC/KLEE harnesses for C code paths | **Pilot** | Medium | High — finds bugs in C code directly |
| L3 | Algebraic Property Proofs | Exhaustive testing of algebraic laws | **Pilot** | Low | Medium — proves lattice/monoid laws |
| L4 | Translation Validation | Schema-to-code consistency checking | **Pilot** | Low | High — prevents spec-implementation drift |
| L5 | Symbolic Model Checking | TLA+/Spin for protocol-level properties | **Deferred** | High | Medium — overkill for current FSM sizes |

### Decision: Why not jump to TLA+/Spin (L5)?

The state machines in asx are small (5, 6, and 4 states). Exhaustive
enumeration (L1) already covers the full state space. The cost of maintaining
TLA+ models alongside C code exceeds the marginal verification value.

L5 becomes worthwhile only if:
- State spaces grow beyond ~20 states (making enumeration expensive)
- Protocol-level properties span multiple interacting FSMs
- Safety-critical certification requires formal model artifacts

**Recommendation:** Focus on L2-L4 for immediate value. Revisit L5 only if
state spaces expand or certification demands it.

---

## 2. Level 0: Golden Output Matching

### Technique
Compare C runtime outputs against golden reference outputs from the Rust
implementation. Detect behavioral drift through semantic digest comparison.

### Entry Criteria
- Rust reference fixtures available in `fixtures/rust_reference/`
- Conformance runner (`tools/ci/run_conformance.sh`) operational
- Semantic digest computation matches Rust reference algorithm

### Exit Criteria
- All fixture scenarios pass with matching semantic digests
- Semantic delta budget = 0 (no unexcused drift)
- All three conformance modes pass: conformance, codec-equivalence, profile-parity

### Artifacts
- `tools/ci/run_conformance.sh` (896 lines, 3 modes)
- `tools/ci/run_profile_parity.sh` (adapter isomorphism)
- `fixtures/rust_reference/smoke/` (golden outputs)
- `docs/SEMANTIC_DELTA_BUDGET.md` (exception workflow)

### Cost/Benefit
- **Cost:** Fixture maintenance when Rust spec evolves; conformance runner complexity
- **Benefit:** Catches any behavioral drift between Rust spec and C implementation
- **Verdict:** Essential. Already deployed. Keep.

### Limitations
- Relies on fixture coverage — untested paths remain unverified
- Does not prove absence of bugs, only consistency with Rust reference
- Fixture staleness if Rust reference evolves without C update

---

## 3. Level 1: Exhaustive Bounded Enumeration

### Technique
Enumerate all (from, to) state pairs for each FSM and verify transition
legality, terminal state properties, monotonicity, reachability, and
predicate consistency. Feasible because state spaces are small (5x5, 6x6,
4x4 = 77 total pairs).

### Entry Criteria
- Transition tables defined in `src/core/transition_tables.c`
- State enums defined in `include/asx/asx_ids.h`
- Predicate functions (terminal, can_spawn, etc.) implemented

### Exit Criteria
- All 10 bounded model invariants pass (22 test functions)
- Full reachability: all non-initial states reachable via BFS
- Terminal state consistency: no outgoing edges from terminal states
- Forward progress: region transitions always increase ordinal

### Artifacts
- `tests/invariant/model_check/test_bounded_model.c` (482 lines, 22 tests)
- `tests/invariant/lifecycle/test_lifecycle_legality.c` (316 lines, 18 tests)

### Cost/Benefit
- **Cost:** Minimal — tests are fast (< 1ms), easy to maintain
- **Benefit:** Complete coverage of all state pairs for small FSMs
- **Verdict:** Essential. Already deployed. Keep.

### Limitations
- Only checks transition tables, not the code that uses them
- Does not verify temporal properties or multi-step sequences
- Cannot find bugs in the runtime code that correctly queries the tables

---

## 4. Level 2: Bounded Model Checking (CBMC)

### Technique
Use CBMC (C Bounded Model Checker) to verify properties of actual C code
paths. Unlike L1 which checks tables in isolation, L2 verifies that the
runtime code correctly enforces transition legality, handle validation,
and resource bounds.

### Entry Criteria
- CBMC installed (`apt install cbmc` or build from source)
- Harness files created per target function
- Properties expressed as C assertions or `__CPROVER_assert()`

### Exit Criteria
- All harness properties verified within configured unwind bound
- No assertion violations found
- Verification covers: transition enforcement, handle validation, budget
  exhaustion, obligation linearity

### Pilot Targets

The following components are selected for L2 pilot based on criticality
and verification tractability:

#### 4.1 Region Transition Enforcement

**What:** Verify that `asx_region_transition_check()` correctly implements
the 5x5 transition matrix for all inputs including out-of-range values.

**Property:** For all `from` in [0..4] and `to` in [0..4]:
- `transition_check(from, to) == ASX_OK` iff `region_transitions[from][to] == 1`
- Out-of-range inputs return `ASX_E_INVALID_ARGUMENT`

**Harness:** `tests/formal/cbmc/region_transition_harness.c`

**CBMC command:**
```
cbmc tests/formal/cbmc/region_transition_harness.c \
  src/core/transition_tables.c \
  -I include --unwind 1 --property "region_*"
```

#### 4.2 Task Transition Enforcement

**What:** Verify `asx_task_transition_check()` for the 6x6 matrix.

**Property:** Same as 4.1 for task states.

**Harness:** `tests/formal/cbmc/task_transition_harness.c`

#### 4.3 Obligation Linearity

**What:** Verify that obligation state machine prevents double-resolution.

**Property:** Once an obligation reaches any terminal state (COMMITTED,
ABORTED, LEAKED), all subsequent transition attempts return
`ASX_E_INVALID_TRANSITION`.

**Harness:** `tests/formal/cbmc/obligation_linearity_harness.c`

#### 4.4 Handle Validation

**What:** Verify that `asx_handle_is_valid()` correctly rejects
`ASX_INVALID_ID` and accepts non-zero handles.

**Property:** `asx_handle_is_valid(0) == 0` and
`asx_handle_is_valid(x) == 1` for all `x != 0`.

**Harness:** `tests/formal/cbmc/handle_validation_harness.c`

### Cost/Benefit
- **Cost:** CBMC installation (~5 min), harness authoring (~30 min each),
  CI integration (~1 hour). Total: ~3 hours for pilot.
- **Benefit:** Finds bugs that table-checking misses (e.g., bounds errors,
  off-by-one in array indexing, unsigned/signed confusion)
- **Verdict:** Recommended for critical paths. Low incremental cost given
  existing test infrastructure.

### Fallback Plan
If CBMC installation is impractical (e.g., embedded CI runners without
package access), the harnesses double as standalone assertion-based tests
compilable with any C99 compiler. The `__CPROVER_assert` calls degrade
to standard `assert()` via a compatibility header.

---

## 5. Level 3: Algebraic Property Proofs

### Technique
Exhaustively verify algebraic laws (commutativity, associativity,
idempotence, identity, absorption) for lattice and monoid operations.
Feasible because the domains are small (4 outcome values, 11 cancel
kinds, bounded budget components).

### Entry Criteria
- Outcome join, cancel strengthen, and budget meet operations implemented
- Test infrastructure can enumerate all input combinations

### Exit Criteria
- All algebraic laws verified for all input combinations
- Identity and absorbing elements confirmed
- Monotonicity properties validated

### Pilot Targets

#### 5.1 Outcome Lattice Laws

**Domain:** 4 severity values (OK=0, ERR=1, CANCELLED=2, PANICKED=3)
**Total combinations:** 4 x 4 = 16 pairs, 4 x 4 x 4 = 64 triples

**Properties:**
- Severity commutativity: `severity(join(a,b)) == severity(join(b,a))`
- Associativity: `severity(join(join(a,b),c)) == severity(join(a,join(b,c)))`
- Idempotence: `join(a,a) == a`
- Identity: `join(OK, x) == x` for all x
- Absorption: `join(PANICKED, x).severity == PANICKED` for all x

**Harness:** `tests/formal/algebraic/test_outcome_lattice.c`

#### 5.2 Cancel Strengthen Monotonicity

**Domain:** 11 cancel kinds x 11 cancel kinds = 121 pairs
**Severity range:** 0..5

**Properties:**
- Monotone: `severity(strengthen(a,b)) >= max(severity(a), severity(b))`
- Idempotent: `strengthen(x,x) == x`
- Deterministic tie-break: equal severity uses earlier timestamp

**Harness:** `tests/formal/algebraic/test_cancel_monotone.c`

#### 5.3 Budget Meet Lattice

**Properties (sampled due to large domain):**
- Commutativity: `meet(a,b) == meet(b,a)` (component-wise)
- Associativity: `meet(meet(a,b),c) == meet(a,meet(b,c))`
- Identity: `meet(infinite, x) == x`
- Absorption: `meet(zero, x) == zero`

**Harness:** `tests/formal/algebraic/test_budget_lattice.c`

### Cost/Benefit
- **Cost:** ~2 hours to write exhaustive enumeration tests
- **Benefit:** Proves algebraic laws hold, catches subtle operator bugs
- **Verdict:** Recommended. Small domain sizes make exhaustive checking cheap.

### Fallback Plan
If exhaustive enumeration becomes too slow (unlikely given domain sizes),
sample random inputs with a fixed seed for deterministic reproduction.

---

## 6. Level 4: Translation Validation

### Technique
Automatically verify that C implementation matches the machine-readable
invariant schema (`schemas/invariant_schema.json`). The schema defines
states, legal transitions, forbidden transitions, and invariants. A
validation script extracts the same information from C code and checks
consistency.

### Entry Criteria
- `schemas/invariant_schema.json` exists and is current
- C transition tables match the schema-defined transitions
- C enum values match schema-defined ordinals

### Exit Criteria
- All schema-defined legal transitions are accepted by C code
- All schema-defined forbidden transitions are rejected by C code
- Enum ordinals match between schema and C headers
- Terminal state predicates match schema terminal flags

### Implementation

**Script:** `tools/ci/check_translation_validation.sh`

**Workflow:**
1. Parse `invariant_schema.json` to extract states, transitions, invariants
2. Compile a probe program that tests each transition pair
3. Compare probe output against schema expectations
4. Report mismatches as translation-validation failures

**Properties checked:**
- State enum ordinal agreement (schema vs `asx_ids.h`)
- Legal transition agreement (schema vs `transition_tables.c`)
- Forbidden transition agreement (schema vs `transition_tables.c`)
- Terminal state predicate agreement (schema vs predicate functions)
- Admission gate agreement (schema vs `can_spawn`, `can_accept_work`)

### Cost/Benefit
- **Cost:** ~2 hours to build validation script
- **Benefit:** Prevents spec-implementation drift (highest-value check for
  a ported codebase). Catches cases where schema is updated but C code
  is not, or vice versa.
- **Verdict:** Strongly recommended. This is the most cost-effective
  addition for a Rust-to-C port.

### Fallback Plan
If the validation script becomes unwieldy, reduce scope to checking only
the 3 core FSMs (region, task, obligation) and their transition tables.

---

## 7. Level 5: Symbolic Model Checking (Deferred)

### Technique
Express protocol-level properties in TLA+ or Spin and verify against
formal models of the asx runtime.

### Why Deferred
- Current FSMs are small enough for exhaustive enumeration (L1)
- TLA+ model maintenance cost is significant
- No certification requirement demands formal models
- L2 (CBMC) provides similar bug-finding capability at lower cost

### Trigger Conditions for Activation
- State spaces grow beyond 20 states
- Multi-FSM interaction protocols emerge (e.g., region + task + channel)
- Safety certification (DO-178C, ISO 26262) requires formal models
- A bug is found that L0-L4 missed and L5 would have caught

### Estimated Cost
- Initial model: 2-4 weeks for all 3 FSMs
- Ongoing maintenance: ~2 hours per schema change
- Tooling: TLA+ Toolbox (free), Spin (free)

---

## 8. Integration Path

### Phase 1: Immediate (L0-L1 hardening)
- [x] Conformance runner operational (3 modes)
- [x] Bounded model checker (22 tests, 10 invariants)
- [x] Ghost monitors: protocol, linearity (called by the lifecycle) and determinism (the lab dispatch records every pick; `make conformance-ghost` reruns each scenario against the sealed first run)
- [ ] Ghost borrow ledger: implemented and unit-tested, but nothing in the runtime acquires a borrow, so it records nothing in a run
- [ ] Add schema drift gate to CI (hash check on `invariant_schema.json`)

### Phase 2: Pilot (L2-L4, this bead)
- [ ] CBMC harnesses for 4 critical targets (Section 4)
- [ ] Algebraic property tests for 3 domains (Section 5)
- [ ] Translation validation script (Section 6)
- [ ] Makefile targets: `formal-cbmc`, `formal-algebraic`, `formal-tv`

### Phase 3: CI Integration
- [ ] Add `formal-check` composite target to Makefile
- [ ] Gate PR merges on translation validation
- [ ] Run CBMC harnesses nightly (too slow for per-push CI)
- [ ] Add algebraic tests to standard `make check`

### Phase 4: Deferred (L5)
- [ ] TLA+ models if/when trigger conditions are met
- [ ] Spin models for multi-FSM protocols if needed

---

## 9. Reusable Templates

### CBMC Harness Template

```c
/* tests/formal/cbmc/<domain>_<property>_harness.c */

/* ASX_CHECKPOINT_WAIVER_FILE("CBMC harness — formal verification only") */

#include <asx/asx.h>

#ifdef CBMC
  #define VERIFY(cond) __CPROVER_assert(cond, #cond)
  #define ASSUME(cond) __CPROVER_assume(cond)
  #define NONDET_UINT() __CPROVER_nondet_unsigned()
#else
  #include <assert.h>
  #define VERIFY(cond) assert(cond)
  #define ASSUME(cond) do { if (!(cond)) return 0; } while(0)
  #define NONDET_UINT() 0  /* fallback: test with 0 */
#endif

int main(void)
{
    unsigned from = NONDET_UINT();
    unsigned to   = NONDET_UINT();

    ASSUME(from <= MAX_STATE);
    ASSUME(to   <= MAX_STATE);

    asx_status st = domain_transition_check(from, to);

    /* Property: legal transitions return OK, others return error */
    if (expected_legal(from, to)) {
        VERIFY(st == ASX_OK);
    } else {
        VERIFY(st == ASX_E_INVALID_TRANSITION);
    }

    return 0;
}
```

### Algebraic Property Test Template

```c
/* tests/formal/algebraic/test_<domain>_<property>.c */

/* ASX_CHECKPOINT_WAIVER_FILE("Algebraic property test — exhaustive enumeration") */

#include "test_harness.h"
#include <asx/asx.h>

/* Exhaustive enumeration over domain */
TEST(commutativity)
{
    for (int a = MIN; a <= MAX; a++) {
        for (int b = MIN; b <= MAX; b++) {
            result_t ab = op(a, b);
            result_t ba = op(b, a);
            CHECK(measure(ab) == measure(ba));
        }
    }
}

TEST(associativity)
{
    for (int a = MIN; a <= MAX; a++) {
        for (int b = MIN; b <= MAX; b++) {
            for (int c = MIN; c <= MAX; c++) {
                result_t ab_c = op(op(a, b), c);
                result_t a_bc = op(a, op(b, c));
                CHECK(measure(ab_c) == measure(a_bc));
            }
        }
    }
}
```

### Translation Validation Template

```bash
#!/usr/bin/env bash
# tools/ci/check_translation_validation.sh
#
# Validates C implementation against invariant_schema.json spec.
# See docs/FORMAL_ASSURANCE_LADDER.md Section 6.

# For each domain in schema:
#   1. Extract states and ordinals
#   2. Extract legal/forbidden transitions
#   3. Compile probe that tests each pair
#   4. Compare results against schema expectations
```

---

## 10. Cost/Benefit Summary

| Level | One-Time Cost | Ongoing Cost | Bug-Finding Value | Already Deployed |
|-------|--------------|-------------|-------------------|------------------|
| L0 | 2 weeks | 2 hrs/schema change | High (drift detection) | Yes |
| L1 | 2 days | 30 min/FSM change | High (complete for small FSMs) | Yes |
| L2 | 3 hours | 30 min/harness | High (finds C-specific bugs) | Pilot |
| L3 | 2 hours | 15 min/operator change | Medium (proves algebraic laws) | Pilot |
| L4 | 2 hours | 30 min/schema change | High (prevents spec drift) | Pilot |
| L5 | 2-4 weeks | 2 hrs/schema change | Medium (overkill for current size) | Deferred |

**Total pilot investment:** ~7 hours for L2-L4.
**Recommendation:** Proceed with L2-L4 pilot. Defer L5.

---

## 11. Fallback Plan

If formal tooling proves impractical:

1. **CBMC unavailable:** Harnesses compile as standalone assertion tests
   with any C99 compiler (graceful degradation via compatibility macros)
2. **Translation validation too complex:** Reduce scope to 3 core FSMs only
3. **Algebraic tests too slow:** Switch from exhaustive to seeded-random sampling
4. **Maintenance burden too high:** Freeze at L0-L1 (already deployed) and
   only activate L2-L4 for high-risk changes

The key insight is that L0-L1 already provide strong assurance for the
current codebase. L2-L4 are incremental improvements with diminishing
returns. The ladder should be climbed only as far as the cost/benefit
ratio justifies.
