/*
 * asx_abi.h — ABI stability contract and versioned compatibility (bd-56t.4)
 *
 * This header defines the ABI stability contract for the asx library.
 * Include it to access ABI version checks, deprecation macros, and
 * compile-time compatibility verification.
 *
 * ## Stability contract
 *
 * - MAJOR: incremented on ABI-breaking changes (enum reorder, handle
 *   layout change, struct field removal, hook signature change)
 * - MINOR: incremented on backward-compatible API additions (new error
 *   codes, new profiles, new functions)
 * - PATCH: incremented on bug fixes with no API/ABI change
 *
 * ## ABI-critical surfaces (changes require MAJOR bump)
 *
 * 1. Opaque handle layout (asx_region_id, asx_task_id, etc.)
 *    - 64-bit format: [type_tag:16][state_mask:16][gen:16][slot:16]
 *    - state_mask: bits 0-5 lifecycle state (1 << state), bits 6-9 the
 *      issuing runtime's tag (0 for the first runtime), bits 10-15 zero
 *
 * 2. Enumeration values — numeric assignments are frozen:
 *    - asx_region_state, asx_task_state, asx_obligation_state
 *    - asx_outcome_severity, asx_cancel_kind, asx_cancel_phase
 *    - asx_cancel_severity, asx_resource_class, asx_profile_id
 *
 * 3. Status code families — error code numbers are permanent:
 *    - 1xx general, 2xx transition, 3xx region, ..., 15xx replay
 *    - New codes may be added; existing codes never change meaning
 *
 * 4. Runtime hooks struct (asx_runtime_hooks) — field order and
 *    function pointer signatures are frozen
 *
 * 5. Binary wire formats — trace and codec binary protocols are
 *    versioned independently (ASX_TRACE_BINARY_VERSION, etc.)
 *
 * ## Forward-compatible evolution
 *
 * - Config structs use size-field pattern: set cfg.size = sizeof(cfg)
 *   before passing to library (the *_init functions do). A size other
 *   than the library's own is refused with ASX_E_INVALID_ARGUMENT
 *   (rt.c, hooks.c); older layouts are not adapted.
 * - New enum values may be appended (never inserted or reordered)
 * - New error codes added within existing families
 * - New functions may be added without breaking ABI
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_ABI_H
#define ASX_ABI_H

#include <asx/asx_export.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------
 * ABI version — tracks binary compatibility independently of API version
 *
 * ABI version is bumped when compiled library layout changes in ways
 * that break callers linked against a previous version.
 * ----------------------------------------------------------------------- */

/* Major 3: cancel-kind ordinals follow Rust's CancelKind (DEC-006) and the
 * entropy hooks gain a fallible draw (try_u64_fn). Major 2: checked creation
 * entry points that take the application fingerprint. */
#define ASX_ABI_VERSION_MAJOR 3
#define ASX_ABI_VERSION_MINOR 0
#define ASX_ABI_VERSION_PATCH 0

/* Composite ABI version for single-value comparisons.
 * Format: (MAJOR * 1000000) + (MINOR * 1000) + PATCH */
#define ASX_ABI_VERSION                                                                            \
    ((ASX_ABI_VERSION_MAJOR * 1000000) + (ASX_ABI_VERSION_MINOR * 1000) + ASX_ABI_VERSION_PATCH)

/* -----------------------------------------------------------------------
 * Deprecation macro — marks functions/types scheduled for removal
 *
 * Usage:
 *   ASX_DEPRECATED("use asx_foo_v2() instead")
 *   ASX_API asx_status asx_foo(int arg);
 * ----------------------------------------------------------------------- */

#ifndef ASX_DEPRECATED
#if defined(__cplusplus) && __cplusplus >= 201402L
#define ASX_DEPRECATED(msg) [[deprecated(msg)]]
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
#define ASX_DEPRECATED(msg) [[deprecated(msg)]]
#elif defined(__GNUC__) || defined(__clang__)
#define ASX_DEPRECATED(msg) __attribute__((deprecated(msg)))
#elif defined(_MSC_VER)
#define ASX_DEPRECATED(msg) __declspec(deprecated(msg))
#else
#define ASX_DEPRECATED(msg)
#endif
#endif

/* -----------------------------------------------------------------------
 * ABI compatibility check — compile-time verification
 *
 * Consumer code can use ASX_ABI_CHECK() to verify at compile time that
 * the headers they are building against are ABI-compatible with the
 * library version they expect:
 *
 *   ASX_ABI_CHECK(3);   // require ABI major version 3
 *
 * This prevents silent ABI mismatches when upgrading.
 * ----------------------------------------------------------------------- */

/* Compile-time ABI version assertion (works at file or function scope).
 * Uses sizeof of a negative-size array to produce a compile error on
 * mismatch, wrapped in (void) to suppress unused warnings. */
#define ASX_ABI_CHECK(expected_major)                                                              \
    (void)sizeof(char[(ASX_ABI_VERSION_MAJOR == (expected_major)) ? 1 : -1])

/* -----------------------------------------------------------------------
 * ABI-frozen type size assertions
 *
 * These document the expected sizes of ABI-critical types. A size
 * change in any of these types constitutes an ABI break.
 * ----------------------------------------------------------------------- */

/* Expected sizes (in bytes) for 64-bit targets */
#define ASX_ABI_SIZEOF_REGION_ID 8u
#define ASX_ABI_SIZEOF_TASK_ID 8u
#define ASX_ABI_SIZEOF_OBLIGATION_ID 8u
#define ASX_ABI_SIZEOF_TIMER_ID 8u
#define ASX_ABI_SIZEOF_CHANNEL_ID 8u
#define ASX_ABI_SIZEOF_STATUS 4u
#define ASX_ABI_SIZEOF_TIME 8u

/* Frozen split of a handle's 16-bit mask field: lifecycle state in bits
 * 0-5 (one bit per state), the issuing runtime's tag in bits 6-9, bits 10-15
 * reserved zero. Tag 0 is the first runtime, whose handles are byte-identical
 * to handles issued before the tag existed. The ABI consumer shim checks
 * these against the ASX_HANDLE_* constants of asx_ids.h. */
#define ASX_ABI_HANDLE_STATE_BITS 6u
#define ASX_ABI_HANDLE_RT_TAG_SHIFT 6u
#define ASX_ABI_HANDLE_RT_TAG_BITS 4u

/* -----------------------------------------------------------------------
 * ABI-frozen enum sentinel values
 *
 * The last valid value of each ABI-critical enum. If a new value is
 * appended, the sentinel must be updated. Removal or reordering of
 * existing values is an ABI break.
 * ----------------------------------------------------------------------- */

#define ASX_ABI_REGION_STATE_LAST 4u     /* ASX_REGION_CLOSED */
#define ASX_ABI_TASK_STATE_LAST 5u       /* ASX_TASK_COMPLETED */
#define ASX_ABI_OBLIGATION_STATE_LAST 3u /* ASX_OBLIGATION_LEAKED */
#define ASX_ABI_OUTCOME_SEVERITY_LAST 3u /* ASX_OUTCOME_PANICKED */
#define ASX_ABI_CANCEL_KIND_LAST 10u     /* ASX_CANCEL_LINKED_EXIT */
#define ASX_ABI_CANCEL_PHASE_LAST 3u     /* ASX_CANCEL_PHASE_COMPLETED */

/* -----------------------------------------------------------------------
 * Runtime ABI identity query
 *
 * Allows consumers to verify ABI compatibility at runtime when loading
 * shared libraries:
 *
 *   if (asx_abi_version_major() != ASX_ABI_VERSION_MAJOR) { abort(); }
 * ----------------------------------------------------------------------- */

/* Return the major component of the runtime ABI version. */
ASX_API unsigned int asx_abi_version_major(void);
/* Return the minor component of the runtime ABI version. */
ASX_API unsigned int asx_abi_version_minor(void);
/* Return the patch component of the runtime ABI version. */
ASX_API unsigned int asx_abi_version_patch(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_ABI_H */
