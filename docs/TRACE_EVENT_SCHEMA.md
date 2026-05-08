# Trace Event Schema

> **Bead:** `bd-v12u.13`
> **Status:** Versioned trace-event contract for incident evidence and Rust/C correlation
> **Last updated:** 2026-05-08 by SilverFrog

This document defines the versioned event schema used when trace events leave
the in-process C runtime and become conformance fixtures, incident bundle
evidence, or replay minimizer inputs. It does not change runtime trace emission
or semantic digest behavior.

## 1. Schema Identity

| Field | Value |
|-------|-------|
| Schema name | `asx.trace_event` |
| Schema version | `asx.trace_event.v1` |
| Version tuple | `1.0.0` |
| JSON schema | `schemas/trace_event.schema.json` |
| Public C descriptor | `asx_trace_schema_current()` |

Version `1.x` requires the same digest-visible event fields as the in-process
`asx_trace_event` record:

- `sequence`: monotonic zero-based event index.
- `kind` / `kind_id`: stable event kind string and numeric wire value.
- `entity_id`: primary runtime handle or zero when no handle exists.
- `aux`: kind-specific payload.

The digest-visible fields are exactly `sequence`, `kind_id`, `entity_id`, and
`aux`. `src/runtime/trace.c` hashes these fields in little-endian order with
the existing FNV-1a 64-bit trace digest. JSON examples carry hex strings for
64-bit integer fields so incident tools do not lose precision.

## 2. Optional Correlation Fields

The schema permits optional metadata that does not participate in semantic
digest calculation:

- `producer`: runtime, language, profile, and source path.
- `domain`: scheduler, region, task, obligation, channel, or timer.
- `semantic_unit`: provenance-map unit such as `REGION-001`.
- `rust_correlation`: Rust baseline commit and source fixture path.
- `rust_event` and `source_fixture_event`: per-event Rust fixture anchors.

Readers may ignore optional fields. They must not infer semantic equivalence
from optional metadata alone; the digest-visible fields remain authoritative.

## 3. Compatibility Rules

The C compatibility API returns:

| Result | Meaning |
|--------|---------|
| `exact` | Schema name, version tuple, required fields, optional mask, and digest fields match. |
| `additive_optional` | Same schema name, same major version, same required fields, and same digest fields, but optional metadata or minor/patch version differs. |
| `incompatible` | Descriptor is unversioned, schema name differs, major version differs, required fields drift, or digest fields drift. |

Upgrade and downgrade behavior:

1. A `1.x` reader may consume newer `1.y` producers only when changes are
   optional and digest-invisible.
2. A newer `1.y` reader may consume older `1.x` producers by treating absent
   optional fields as unknown.
3. Any required-field or digest-field change requires a new major version and
   must fail compatibility against `asx.trace_event.v1`.
4. Version major `0`, missing schema name, or missing schema version is always
   incompatible.

## 4. Golden Examples and Gates

Golden examples:

- `fixtures/trace_events/trace-schema-v1-c-region-task.json`
- `fixtures/trace_events/trace-schema-v1-rust-correlated.json`

Validation gates:

- `build/tests/unit/runtime/test_trace` checks the C descriptor and
  compatibility behavior.
- `make lint-schema-validation` validates the golden examples against
  `schemas/trace_event.schema.json`.
- `make conformance`, `make codec-equivalence`, and `make profile-parity`
  continue to enforce zero semantic drift for runtime behavior.

Incident bundles and minimizers must consume the schema version and digest-field
contract before comparing traces. Unversioned event-field changes are treated as
evidence-format failures, not runtime semantic proof.
