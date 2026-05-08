# Incident Evidence Bundle

`asx.incident_bundle.v1` is the operator artifact for large-swarm replay and
debug. It is a single JSON object emitted per scenario and suitable for CI
attachment or bug reports. The first producer is
`tests/e2e/parallel_swarm.sh`, which writes
`parallel_swarm.incident_bundle.jsonl` next to the existing detail JSONL stream.

## Contract

Schema:

- `schema_name`: `asx.incident_bundle`
- `schema_version`: `asx.incident_bundle.v1`
- JSON schema: `schemas/incident_bundle.schema.json`
- Golden example:
  `fixtures/incident_bundles/incident-bundle-v1-parallel-swarm.json`

The renderer is `asx_incident_bundle_render_json()`. It renders into
`asx_report_buf` without allocation and rejects missing required fields with
`ASX_E_INVALID_ARGUMENT`. If the fixed buffer would be exhausted, the render
fails with `ASX_E_BUFFER_TOO_SMALL` and leaves the caller output untouched.

## Field Provenance

`run` contains CI and replay identity: run id, scenario id, seed, selected
profile, compiled profile, scale, and codec. These values come from the e2e
harness environment and the compiled ASX profile macros.

`status` contains the scenario verdict, `asx_status` code/string, failure class,
and message. Failure class is derived by
`asx_incident_bundle_failure_class()` unless the caller supplies an override:

- `none`: `ASX_OK`
- `resource`: resource exhaustion and overload statuses
- `conformance`: equivalence or replay mismatch statuses
- `unsupported`: unsupported profile/surface, including
  `ASX_E_PERMISSION_DENIED`
- `runtime`: all other runtime failures

`telemetry` is a compact snapshot of parallel runtime state. Queue depths,
pressure, admission decisions, scheduling counters, locality, and commit
authority are copied from `asx_parallel_telemetry_snapshot`. Scenario emitters
may summarize multiple snapshots by preserving maxima for pressure and queue
fields while keeping the latest locality and commit-authority state.

`trace` contains `asx.trace_event.v1`, event count, semantic digest, and trace
digest. Digest strings use `fnv64:<16 lowercase hex digits>`.

`parity` records whether conformance and profile-parity gates were run for the
artifact. E2e scenarios normally emit `not-run`; CI bundle aggregators may
rewrite this field only when they attach direct gate results.

`artifacts` contains the sibling detail path and exact replay command.

## Stability

Required fields in `asx.incident_bundle.v1` are stable. Additive optional fields
may be added only with a schema update and golden fixture. Removing required
fields, changing digest spelling, or changing failure class meaning requires a
new major schema version.

The bundle is observational. Emitting it must not mutate scheduler state, trace
state, admission policy, or replay identity. Unsupported native behavior is
reported explicitly as `failure_class: "unsupported"` with fail-closed status
text; the bundle must not claim live native execution when the profile only
provides deterministic logical workers.
