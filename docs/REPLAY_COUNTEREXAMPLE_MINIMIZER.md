# Replay Counterexample Minimizer

> **Schema:** `asx.replay_counterexample.v1`
> **Tracking bead:** `bd-v12u.8`
> **Status:** Class-preserving minimizer packet active

The replay minimizer shrinks failing lab scenarios only when the candidate still
reproduces the original failure class. A candidate that merely fails for a
different reason is rejected.

## Failure-Class Contract

The library minimizer captures two observations:

- `original_failure`: the status/oracle failure from the input scenario.
- `current_failure`: the status/oracle failure from the current minimized
  candidate.

The shrink decision preserves:

- failure class: `oracle`, `resource`, `runtime`, or `conformance`,
- exact terminal status when the failure is status-driven,
- oracle verdict and oracle name when the failure is oracle-driven.

Resource exhaustion is its own class. Removing the resource-failing step and
ending in success, or in a non-resource status, is not a valid minimization.

## Packet Shape

Library-rendered packets use:

- `schema_name`: `asx.replay_counterexample`
- `schema_version`: `asx.replay_counterexample.v1`
- `scenario_name`
- `original_steps`
- `current_steps`
- `attempts`
- `accepted_attempts`
- `rejected_attempts`
- `preserves_failure_class`
- `original_failure`
- `current_failure`

The fuzz CLI emits the same schema version with `kind: minimized_scenario` and
includes the minimized op list, original/final digests, mode, and failure class.

## Fixtures And Validation

Golden fixtures live in `fixtures/replay_counterexamples/` and validate against
`schemas/replay_counterexample.schema.json` through `make lint-schema-validation`.

Focused checks:

```sh
make minimize-selftest
build/tests/unit/runtime/test_replay
make lint-schema-validation
```
