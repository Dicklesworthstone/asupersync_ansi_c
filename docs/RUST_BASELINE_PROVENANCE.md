# Rust Baseline Provenance and Rebase Policy

This document defines the canonical provenance contract for Rust-reference fixtures and parity reports in `asx`.

## 1. Current Pinned Baseline (`M0-spec-foundation`)

Machine-readable inventory: `docs/rust_baseline_inventory.json`

Current baseline snapshot:

- `rust_repo_url`: `https://github.com/Dicklesworthstone/asupersync`
- `rust_baseline_commit`: `38c152405bd03e2bd9eecf178bfbbe9472fed861`
- `rust_baseline_commit_ts`: `2026-02-26T21:33:22-05:00`
- `rust_baseline_subject`: `Fix io_uring reactor spurious ERROR events from poll cancellations and stale fds`
- `rust_toolchain_release`: `1.95.0-nightly`
- `rust_toolchain_commit_hash`: `7f99507f57e6c4aa0dce3daf6a13cca8cd4dd312`
- `rust_toolchain_host`: `x86_64-unknown-linux-gnu`
- `cargo_lock_sha256`: `77f4bfaedfe5b8572dfca3181119f9432825ea3d868ee8f5d2793a6c5cdeffdc`
- `cargo_lock_bytes`: `70834`
- `cargo_lock_tracked_by_git`: `false`

## 2. Required Provenance Fields

Every fixture and parity report record must include:

- `rust_baseline_commit`
- `rust_toolchain_commit_hash`
- `rust_toolchain_release`
- `rust_toolchain_host`
- `cargo_lock_sha256`
- `cargo_lock_bytes`
- `cargo_lock_tracked_by_git`
- `fixture_schema_version`
- `scenario_dsl_version`

Parity report records must additionally include:

- `scenario_id`
- `codec`
- `profile`
- `parity`
- `semantic_digest`
- `delta_classification`
- `classification_record_id` when `delta_classification != "none"`

## 3. Delta Classification Taxonomy

Allowed values:

- `none`: no semantic delta observed
- `intentional_upstream`: upstream Rust behavior changed intentionally
- `c_regression`: ANSI C implementation deviated from expected behavior
- `spec_defect`: extraction/specification was incorrect or incomplete
- `harness_defect`: fixture/parity tooling error

## 4. Rebase Protocol (Mandatory)

1. Capture and commit a new baseline inventory entry (old + candidate baseline both available).
2. Regenerate fixtures and parity reports for both baselines.
3. Classify every observed semantic delta using the taxonomy above.
4. Create/attach a classification record for every non-`none` delta with reviewer sign-off.
5. Update the milestone parity target only after all deltas are classified and accepted.

Hard rules:

- One baseline per milestone.
- No mixed-baseline fixtures in a single parity run.
- Provenance mismatch is CI-fail.

## 5. Auditability Requirements

- Baseline inventory must stay machine-readable (`json`) and committed.
- Each parity artifact must be traceable to one inventory record.
- Rebase decisions must be reproducible from committed artifacts without re-opening external chats/notes.

## 6. Capture Commands (Reference)

```bash
git -C /data/projects/asupersync rev-parse HEAD
git -C /data/projects/asupersync show -s --format='%H%n%cI%n%s' HEAD
rustc -Vv
sha256sum /data/projects/asupersync/Cargo.lock
wc -c /data/projects/asupersync/Cargo.lock
```
