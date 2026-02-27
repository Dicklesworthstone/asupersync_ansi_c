# C Portability Rules and UB-Elimination Contract

> **Bead:** `bd-296.29`
> **Status:** Canonical portability and undefined-behavior policy
> **Last updated:** 2026-02-27 by CopperSpire

This document defines mandatory rules for portable, deterministic ANSI C/C99 implementation across GCC/Clang/MSVC and embedded cross-targets.

## 1. Portability Objectives

1. eliminate undefined behavior from core/runtime surfaces,
2. make cross-compiler and cross-target behavior explicit and auditable,
3. preserve deterministic semantics under all supported profiles,
4. provide rule-to-test and rule-to-gate mappings.

## 2. Rule Classes

- `P-REQ-*`: mandatory requirements.
- `P-BAN-*`: banned constructs/patterns.
- `P-WRAP-*`: required audited wrappers.
- `P-CI-*`: mandatory CI enforcement hooks.

## 3. Mandatory Requirements (`P-REQ-*`)

| Rule ID | Requirement | Why |
|---|---|---|
| `P-REQ-001` | Use fixed-width integer types for externally serialized/runtime-critical fields | Prevent width drift across compilers/ABIs |
| `P-REQ-002` | All externally visible structs use explicit initialization policy | Prevent uninitialized-field UB and ABI drift |
| `P-REQ-003` | All state transitions pass through authority tables/check helpers | Prevent ad hoc illegal state mutation |
| `P-REQ-004` | All handle dereferences validate type/index/generation | Prevent stale-handle and type confusion misuse |
| `P-REQ-005` | Exhaustion paths are failure-atomic (no partial mutation) | Preserve deterministic safety guarantees |
| `P-REQ-006` | Pointer aliasing assumptions must be explicit and auditable | Avoid strict-aliasing UB |
| `P-REQ-007` | Endianness-sensitive decode/encode uses canonical helpers only | Avoid target-dependent behavior |
| `P-REQ-008` | Unaligned access uses safe load/store helpers only | Avoid architecture traps/UB |

## 4. Banned Constructs (`P-BAN-*`)

| Rule ID | Banned construct/pattern | Rationale |
|---|---|---|
| `P-BAN-001` | Type-punning through incompatible pointer casts | Strict-aliasing UB risk |
| `P-BAN-002` | Out-of-bounds pointer arithmetic | UB + non-portable optimizer behavior |
| `P-BAN-003` | Signed integer overflow reliance | UB in C; breaks determinism |
| `P-BAN-004` | Shift by negative or >= bit-width values | UB/implementation-defined behavior |
| `P-BAN-005` | Reading indeterminate/uninitialized storage | UB + non-reproducible behavior |
| `P-BAN-006` | Assuming packed/unaligned struct layout by default | ABI/compiler variation |
| `P-BAN-007` | Implicit fallthrough in state-transition switch without marker/policy | Transition safety ambiguity |
| `P-BAN-008` | Hidden global mutable runtime state in core semantic paths | Breaks explicit context/authority model |
| `P-BAN-009` | Silent narrowing casts in critical counters/ids | Overflow/wrap ambiguity |
| `P-BAN-010` | Data race-prone unsynchronized shared mutation in runtime adapters | Undefined behavior + nondeterminism |

## 5. Required Audited Wrappers (`P-WRAP-*`)

### 5.1 Arithmetic

- `asx_checked_add_u32`
- `asx_checked_add_u64`
- `asx_checked_mul_u32`
- `asx_checked_mul_u64`

Requirements:

- overflow returns explicit status,
- no implicit wrap in semantic-critical paths.

### 5.2 Bit/Shift Operations

- `asx_safe_lshift_u32(value, shift)`
- `asx_safe_rshift_u32(value, shift)`

Requirements:

- validate shift bounds before operation,
- deterministic error on invalid shift.

### 5.3 Endian and Unaligned Access

- `asx_load_le_u16/u32/u64`
- `asx_store_le_u16/u32/u64`
- `asx_load_be_u16/u32/u64`
- `asx_store_be_u16/u32/u64`

Requirements:

- use `memcpy`-based safe loads/stores for unaligned buffers,
- no direct unaligned cast dereference.

### 5.4 Memory Initialization and Zeroization

- `asx_mem_zero`
- `asx_mem_copy_checked`

Requirements:

- deterministic initialization before first read,
- bounds/size checks on critical buffer operations.

## 6. Determinism-Specific Portability Rules

1. No dependency on unspecified iteration order of hash maps/sets in semantic paths.
2. No dependency on wall-clock nondeterminism in deterministic mode.
3. Comparison/tie-break keys must be explicitly encoded and stable.
4. Error/status selection under ambiguity must use deterministic ordering policy.

## 7. CI and Static-Analysis Enforcement (`P-CI-*`)

| Rule ID | Enforcement mechanism | Expected gate behavior |
|---|---|---|
| `P-CI-001` | warnings-as-errors (`gcc/clang/msvc`) | fail build on warning regression |
| `P-CI-002` | static analysis job (clang-tidy/analog + policy) | fail on must-fix findings; require explicit waiver metadata |
| `P-CI-003` | UB-focused tests (overflow, shift, unaligned, bounds) | fail on first semantic violation |
| `P-CI-004` | endian/unaligned conformance suite | fail on cross-target mismatch |
| `P-CI-005` | deterministic replay equivalence fixtures | fail on digest drift |
| `P-CI-006` | forbidden-behavior fixture family (`bd-296.5`) | fail if forbidden case passes |

## 8. Review Checklist for Contributors

Every core/runtime PR must answer:

1. Which portability rules are touched?
2. Are banned constructs introduced? (must be no)
3. Which wrappers are used for arithmetic/shift/endian/unaligned handling?
4. Which tests/fixtures prove behavior remains portable and deterministic?
5. Are any waivers required? If yes, where is rationale and expiry?

## 9. Waiver Policy

Waivers are exceptional and time-bounded.

Required waiver fields:

- rule id,
- file/line scope,
- technical rationale,
- risk assessment,
- mitigation plan,
- expiry/review date,
- owner.

Expired waivers fail CI.

## 10. Downstream Dependencies

This contract feeds:

- `bd-66l.10` (warning/static-analysis/model-check gates),
- `bd-hwb.1` (core type implementation safety contract),
- `bd-ix8.7` (endian/unaligned hardening suite),
- `bd-1md.14` (robustness fixture families),
- `bd-296.30` (Phase 1 spec-review gate packet).
