/*
 * browser_boundary.h — browser profile capability boundary (fail-closed)
 *
 * The browser profile operates in a sandboxed WASM environment where
 * native host surfaces (filesystem, process spawning, OS signals) are
 * unavailable. This module provides:
 *
 *   1. Capability classification: native vs browser-safe surfaces
 *   2. Fail-closed gate: rejects native surface access with
 *      ASX_E_PERMISSION_DENIED when the browser profile is active
 *   3. Compile-time and runtime queryable capability flags
 *
 * The boundary is fail-closed: any surface not explicitly granted
 * returns ASX_E_PERMISSION_DENIED. This prevents silent capability
 * leaks when new surfaces are added.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_BROWSER_BOUNDARY_H
#define ASX_BROWSER_BOUNDARY_H

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <asx/runtime/profile_compat.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Host surface classification
 *
 * Each surface the runtime can interact with is enumerated here.
 * Surfaces are classified as either browser-safe (available in WASM)
 * or native-only (blocked by the browser boundary).
 * ------------------------------------------------------------------- */

typedef enum {
    /* Browser-safe surfaces */
    ASX_SURFACE_REGION = 0,     /* region lifecycle */
    ASX_SURFACE_TASK = 1,       /* task spawn/poll/drain */
    ASX_SURFACE_CHANNEL = 2,    /* MPSC channel */
    ASX_SURFACE_TIMER = 3,      /* timer wheel */
    ASX_SURFACE_OBLIGATION = 4, /* obligation protocol */
    ASX_SURFACE_TRACE = 5,      /* trace/telemetry */
    ASX_SURFACE_GHOST = 6,      /* ghost monitors (debug) */

    /* Native-only surfaces (blocked in browser) */
    ASX_SURFACE_FILESYSTEM = 7, /* fs read/write/stat */
    ASX_SURFACE_PROCESS = 8,    /* child process spawn */
    ASX_SURFACE_SIGNAL = 9,     /* OS signal handling */
    ASX_SURFACE_IO_DRIVER = 10, /* native I/O reactor */
    ASX_SURFACE_BLOCKING = 11,  /* blocking thread pool */
    ASX_SURFACE_SERVER = 12,    /* native server/listener substrate */
    ASX_SURFACE_GRPC = 13,      /* gRPC transport/server family */
    ASX_SURFACE_MESSAGING = 14, /* broker/messaging family */

    ASX_SURFACE_COUNT = 15
} asx_host_surface;

/* -------------------------------------------------------------------
 * API: Capability queries
 * ------------------------------------------------------------------- */

/* Check if a surface is available for the given profile.
 * Returns 1 if the surface is available, 0 if blocked. */
ASX_API int asx_surface_available(asx_profile_id profile, asx_host_surface surface);

/* Check if a surface is available for the currently active profile. */
ASX_API int asx_surface_available_active(asx_host_surface surface);

/* Return the human-readable name of a host surface. Never returns NULL. */
ASX_API const char *asx_surface_name(asx_host_surface surface);

/* -------------------------------------------------------------------
 * API: Fail-closed gate
 * ------------------------------------------------------------------- */

/* Gate a surface access: returns ASX_OK if the surface is available
 * for the active profile, ASX_E_PERMISSION_DENIED if blocked.
 * This is the primary entry point for fail-closed boundary enforcement. */
ASX_API asx_status asx_surface_gate(asx_host_surface surface);

/* Return the number of surfaces blocked for the given profile. */
ASX_API uint32_t asx_surface_blocked_count(asx_profile_id profile);

/* Return the number of surfaces available for the given profile. */
ASX_API uint32_t asx_surface_available_count(asx_profile_id profile);

#ifdef __cplusplus
}
#endif

#endif /* ASX_BROWSER_BOUNDARY_H */
