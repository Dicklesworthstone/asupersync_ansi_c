# ASX Test Suite

## Overview

The ASX test suite verifies the ANSI C async runtime across multiple layers:
unit tests, invariant tests, API ergonomics vignette tests, end-to-end scenario
tests, benchmarks, and fuzz tests.

**Current totals**: 204 C test programs (149 unit, 20 e2e, 3 invariant,
12 vignette, 3 conformance, 4 fuzz, 13 formal) with 800+ individual test
cases. Canonical counts are maintained by `tools/count_inventory.sh`.

## Running Tests

```bash
make test              # Unit + invariant + vignette tests
make test-unit         # Unit tests only
make test-invariants   # Lifecycle invariant tests
make test-vignettes    # API ergonomics usage vignettes (public headers)
make test-e2e          # Core e2e scenario lanes
make test-e2e-suite    # All e2e families with unified manifest
make bench             # Performance benchmarks (JSON output)
make fuzz-smoke        # Differential fuzz smoke test
make check             # Full gate: format + lint + build + test
```

Use `rch exec -- make <target>` to offload compilation to remote workers.

## Test Categories

### Unit Tests (`tests/unit/`)

| Suite | Module | Tests | Focus |
|-------|--------|-------|-------|
| test_status | core | 4 | Status code string mapping |
| status_basic_test | core | 6 | Error family classification |
| test_budget | core | 15 | Poll/time budget mechanics |
| test_cancel | core | 12 | Cancel kind severity/strengthen |
| test_transition | core | 22 | State machine transition legality |
| test_outcome | core | 14 | Outcome severity lattice |
| test_ids | core | 9 | Handle pack/unpack round-trip |
| test_cleanup | core | 16 | Cleanup stack LIFO/capacity |
| test_ghost | core | 21 | Ghost safety monitors |
| test_ghost_borrow | core | 22 | Borrow ledger linearity |
| test_obligation | core | 12 | Obligation lifecycle |
| test_error_ledger | core | 5 | Error attribution |
| test_fault_injection | core | 15 | Fault injection hooks |
| test_safety_profiles | core | 17 | Profile semantic rules |
| test_safety_posture | core | 35 | Crafted/stale/null handle rejection |
| test_resource | core | 28 | Resource tracking |
| test_affinity | core | 27 | Core affinity masks |
| test_adaptive | core | 19 | Adaptive parameter tuning |
| test_endian | core | 25 | Endian/unaligned access hardening |
| test_protothread | core | 4 | Stackless protothread macros |
| test_handle_generation | core | 10 | Generational handle safety |
| handle_safety_test | core | 13 | Handle misuse detection |
| test_hooks | core | 13 | Hook vtable contract |
| runtime_hooks_test | core | 9 | Hook init/dispatch |
| test_scheduler | runtime | 10 | Round-robin deterministic scheduling |
| test_scheduler_checker | runtime | 8 | Scheduler invariant checker |
| test_cancellation | runtime | 22 | Cancel propagation protocol |
| test_trace | runtime | 18 | Deterministic event trace/replay |
| test_continuity | runtime | 22 | Trace continuity verification |
| test_fault_containment | runtime | 18 | Region-scoped fault containment |
| test_hindsight | runtime | 33 | Nondeterminism ring logging |
| test_telemetry | runtime | 17 | Telemetry counters |
| test_api_misuse | runtime | 35 | API misuse detection |
| test_automotive_fixtures | runtime | 20 | Automotive profile fixtures |
| test_hft_microburst | runtime | 13 | HFT microburst scenarios |
| test_codec_json | runtime | 12 | JSON codec round-trip |
| test_codec_equivalence | runtime | 18 | Cross-codec parity |
| test_profile_compat | runtime | 33 | Cross-profile compatibility |
| test_coroutine | runtime | 13 | Stackless coroutine scheduling |
| test_ghost (runtime) | runtime | 20 | Runtime ghost integration |
| test_stress | runtime | 17 | Arena exhaustion, cancel storms, timer churn |
| test_hft_instrument | runtime | 34 | HFT instrumentation |
| test_mpsc | channel | 39 | MPSC bounded two-phase channel |
| test_link | link | 3 | Public link request/response coordination |
| test_record | record | 3 | Public record snapshot/event summary surface |
| test_timer_wheel | time | 22 | Hierarchical timer wheel |

### Invariant Tests (`tests/invariant/`)

Invariant tests verify structural properties that must hold across all
valid inputs. They use exhaustive enumeration of state machine transitions.

- **test_lifecycle_legality** (18 tests): Region/task/obligation transition matrix

### API Ergonomics Vignettes (`tests/vignettes/`)

These are compilable usage programs that exercise real public-header flows
before API/ABI freeze (`make test-vignettes`):

- `vignette_lifecycle.c` — region/task lifecycle and drain/close patterns
- `vignette_obligations.c` — reserve/commit/abort obligation protocol
- `vignette_budgets.c` — budget construction and exhaustion handling
- `vignette_replay.c` — deterministic trace/replay and snapshot flows
- `vignette_hooks.c` — freestanding runtime hook integration
- `vignette_security.c` — authenticated symbol, derived-context rejection, and audit/evidence flows
- `vignette_link.c` — public link/session round-trip with obligation and record capture

### Canonical Examples (`examples/`)

These are runnable walkthrough programs validated by the `examples_smoke`
e2e family so public onboarding examples fail fast when runtime surfaces move:

- `ex_quickstart.c` — region/task lifecycle plus current-thread runtime builder, local scope, blocking offload, and deadline flow
- `ex_cancel_drain.c` — cancellation propagation and bounded drain patterns
- `ex_channel_flow.c` — bounded channel handoff and producer/consumer flow
- `ex_network_surface.c` — deterministic resolver, TCP, and UDP walkthrough
- `ex_timeout_deadline.c` — timeout/deadline contracts and timer-wheel behavior
- `ex_lab_replay.c` — deterministic replay/lab artifacts and rerun hints
- `ex_browser_boundary.c` / `ex_browser_replay.c` — browser-profile boundary and replay walkthroughs, validated by the dedicated `browser_smoke` lane

### End-to-End Tests (`tests/e2e/`)

E2e tests are shell scripts that build and run multi-step scenarios.
Each family maps to a hard gate:

| Family | Gate | Script |
|--------|------|--------|
| foundational_contracts | GATE-E2E-FOUNDATIONAL | `foundational_contracts.sh` |
| examples_smoke | GATE-E2E-EXAMPLES | `examples_smoke.sh` |
| browser_smoke | GATE-E2E-BROWSER | `browser_smoke.sh` |
| core_lifecycle | GATE-E2E-LIFECYCLE | `core_lifecycle.sh` |
| codec_parity | GATE-E2E-CODEC | `codec_parity.sh` |
| native_host | GATE-E2E-NATIVE-HOST | `native_host.sh` |
| server_shutdown | GATE-E2E-SERVER | `server_shutdown.sh` |
| robustness | GATE-E2E-ROBUSTNESS | `robustness.sh` |
| robustness_endian | GATE-E2E-ROBUSTNESS | `robustness_endian.sh` |
| robustness_exhaustion | GATE-E2E-ROBUSTNESS | `robustness_exhaustion.sh` |
| robustness_fault | GATE-E2E-ROBUSTNESS | `robustness_fault.sh` |
| hft_microburst | GATE-E2E-VERTICAL-HFT | `hft_microburst.sh` |
| automotive_watchdog | GATE-E2E-VERTICAL-AUTO | `automotive_watchdog.sh` |
| continuity | GATE-E2E-CONTINUITY | `continuity.sh` |
| continuity_restart | GATE-E2E-CONTINUITY | `continuity_restart.sh` |

Run all families: `make test-e2e-suite` (emits `run_manifest.json`).

### Benchmarks (`tests/bench/`)

- **bench_runtime.c**: Scheduler throughput, cancel propagation latency,
  trace emission rates. Outputs JSON for CI trend tracking.

### Fuzz Tests (`tests/fuzz/`)

- **fuzz_differential.c**: Differential fuzzer comparing C runtime against
  reference trace digests
- **fuzz_minimize.c**: Counterexample minimizer for shrinking fuzz failures

## Test Harness

All C tests use `tests/test_harness.h` which provides:

```c
TEST(name) { ... }       // Define a test function
RUN_TEST(name);           // Run a test and track pass/fail
ASSERT_TRUE(expr);        // Boolean assertion
ASSERT_EQ(a, b);          // Equality assertion (with diagnostics)
ASSERT_NEQ(a, b);         // Inequality assertion
TEST_REPORT();            // Print summary and set exit code
```

E2e tests use `tests/e2e/harness.sh` which provides:

```bash
e2e_init "family_name"            # Initialize scenario
e2e_scenario "name" "command"     # Run a scenario step
e2e_finish                        # Emit manifest, report results
```

## Writing New Tests

1. Create `tests/unit/<module>/test_<name>.c`
2. Include `"test_harness.h"` and relevant headers
3. Define tests with `TEST(name)` and assertions
4. Add `RUN_TEST(name)` calls in `main()`
5. End `main()` with `TEST_REPORT(); return test_failures;`
6. Add the test binary to the Makefile's test target list

Every `ASX_MUST_USE` function call must have its return value checked
(typically via `ASSERT_EQ`). The `-Werror=unused-result` flag enforces this.

## CI Integration

The `make check` target runs the full gate sequence:
`format-check` -> `lint` -> `lint-docs` -> `lint-checkpoint` -> `build` -> `test`

Test logs can optionally emit structured JSONL via `test_log.h` for
automated analysis. Schema: `schemas/test_log.schema.json`.

For public/operator-facing verification helpers, the same JSONL pattern is now
available via `include/asx/testing/log.h` with the `asx_test_log_*` API. The
CLI/operator slice also adds:
- `tests/unit/app/test_app.c`
- `tests/unit/app/test_report.c`
- `tests/unit/console/test_console.c`
- `tests/unit/tracing_compat/test_tracing_compat.c`
- `tests/vignettes/vignette_console.c`

The observability/evidence slice adds:
- `tests/unit/evidence/test_evidence.c`
- `tests/unit/monitor/test_monitor.c`
- `tests/vignettes/vignette_observability.c`

The platform adapter slice adds:
- `tests/unit/platform/test_posix_hooks.c` — 12 tests: hook install, validation,
  clock monotonicity/plausibility, entropy distribution/uniqueness, reactor lifecycle
  (epoll pipe fd register/write/wait/deregister on Linux), ghost reactor
- `tests/e2e/posix_adapter_smoke.sh` + `e2e_posix_adapter_smoke.c` — 7 scenarios
- `tests/e2e/install_smoke.sh` — 8 scenarios: install script, header install, quickstart compile/run
- `tests/e2e/cli_smoke.sh` — 10 scenarios: CLI version/info/doctor/help/error handling
