/*
 * browser_diagnostic.h — browser-specific diagnostics and DX error taxonomy
 *
 * Extends the shared diagnostic/evidence infrastructure with
 * browser-specific capabilities:
 *
 *   1. DX error taxonomy: human-readable, actionable error messages
 *      for common browser profile failures (capability denial,
 *      sandbox escape attempts, WASM compat issues)
 *   2. Browser evidence collector: inspects the browser boundary
 *      and records findings to the shared evidence sink
 *   3. Flake governance: captures deterministic replay artifacts
 *      for browser failure forensics
 *
 * Composes with diagnostic.h — not a parallel silo.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_BROWSER_DIAGNOSTIC_H
#define ASX_BROWSER_DIAGNOSTIC_H

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <asx/runtime/browser_boundary.h>
#include <asx/runtime/diagnostic.h>
#include <asx/runtime/profile_compat.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * DX error taxonomy — browser-specific error classes
 *
 * Each error class maps to a human-readable explanation and
 * remediation hint. These are designed for developer experience:
 * when a browser profile user hits a boundary, the error message
 * tells them *why* and *what to do instead*.
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_BROWSER_DX_NONE = 0,

    /* Capability boundary violations */
    ASX_BROWSER_DX_NATIVE_FS = 1,       /* filesystem access in browser */
    ASX_BROWSER_DX_NATIVE_PROCESS = 2,  /* process spawn in browser */
    ASX_BROWSER_DX_NATIVE_SIGNAL = 3,   /* signal handling in browser */
    ASX_BROWSER_DX_NATIVE_IO = 4,       /* native I/O driver in browser */
    ASX_BROWSER_DX_NATIVE_BLOCKING = 5, /* blocking thread pool in browser */

    /* Profile mismatch */
    ASX_BROWSER_DX_WRONG_PROFILE = 6, /* non-browser code in browser build */

    /* Sandbox integrity */
    ASX_BROWSER_DX_ALLOCATOR_UNSEAL = 7, /* unsealed allocator in browser */

    ASX_BROWSER_DX_COUNT = 8
} asx_browser_dx_class;

/* DX error entry: class, explanation, and remediation */
typedef struct {
    asx_browser_dx_class dx_class;
    const char *title;       /* short title (e.g., "filesystem unavailable") */
    const char *explanation; /* why this fails in browser */
    const char *remediation; /* what to do instead */
} asx_browser_dx_entry;

/* Look up a DX error entry by class.
 * Returns NULL if class is out of range or ASX_BROWSER_DX_NONE. */
ASX_API const asx_browser_dx_entry *asx_browser_dx_lookup(asx_browser_dx_class dx_class);

/* Map a surface denial to the corresponding DX class.
 * Returns ASX_BROWSER_DX_NONE if the surface is not a browser-denied surface. */
ASX_API asx_browser_dx_class asx_browser_dx_for_surface(asx_host_surface surface);

/* Return the total number of DX error classes. */
ASX_API uint32_t asx_browser_dx_count(void);

/* -------------------------------------------------------------------
 * Browser evidence collector
 *
 * Inspects the browser boundary state and records findings to a
 * shared evidence sink. Composes with asx_inspect_to_evidence().
 * ------------------------------------------------------------------- */

/* Browser-specific evidence report */
typedef struct {
    asx_profile_id active_profile;
    int is_browser;                /* 1 if active profile is BROWSER */
    uint32_t surfaces_available;   /* count of available surfaces */
    uint32_t surfaces_blocked;     /* count of blocked surfaces */
    int allocator_sealable;        /* 1 if allocator can be sealed */
    int all_semantic_rules_hold;   /* 1 if all 8 semantic rules enforced */
} asx_browser_evidence_report;

/* Capture browser boundary evidence into a report struct.
 * Always succeeds (no failure modes). */
ASX_API void asx_browser_evidence_capture(asx_browser_evidence_report *report);

/* Record browser evidence into a shared evidence sink.
 * Records one entry per check (boundary, parity, allocator).
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if either arg is NULL,
 * ASX_E_RESOURCE_EXHAUSTED if sink is full. */
ASX_API asx_status asx_browser_evidence_to_sink(const asx_browser_evidence_report *report,
                                                 asx_evidence_sink *sink);

/* -------------------------------------------------------------------
 * Flake governance — deterministic artifact retention
 *
 * Captures a replay-forensics snapshot that makes browser failures
 * reproducible. The snapshot includes trace digest, profile ID,
 * boundary state, and ghost violations — enough to replay the
 * exact failure scenario.
 * ------------------------------------------------------------------- */

typedef struct {
    uint64_t trace_digest;           /* deterministic trace fingerprint */
    asx_profile_id profile;          /* profile under which failure occurred */
    uint32_t ghost_violations;       /* ghost violation count at capture */
    uint32_t surfaces_blocked;       /* surfaces blocked at capture */
    uint32_t evidence_fail_count;    /* FAIL evidence entries at capture */
    uint32_t evidence_warn_count;    /* WARN evidence entries at capture */
    int is_deterministic;            /* 1 if replay should be exact */
} asx_browser_flake_snapshot;

/* Capture a flake-governance snapshot for failure forensics.
 * Reads from trace, ghost, and browser boundary subsystems.
 * sink may be NULL (in which case evidence counts are zero). */
ASX_API void asx_browser_flake_capture(asx_browser_flake_snapshot *snapshot,
                                        const asx_evidence_sink *sink);

/* Check if two flake snapshots represent the same failure.
 * Returns 1 if trace digests match and boundary state is identical. */
ASX_API int asx_browser_flake_match(const asx_browser_flake_snapshot *a,
                                     const asx_browser_flake_snapshot *b);

#ifdef __cplusplus
}
#endif

#endif /* ASX_BROWSER_DIAGNOSTIC_H */
