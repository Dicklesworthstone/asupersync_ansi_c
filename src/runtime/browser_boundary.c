/*
 * browser_boundary.c — browser profile capability boundary (fail-closed)
 *
 * Implements fail-closed surface gating for the browser profile.
 * Native-only surfaces (filesystem, process, signal, I/O driver,
 * blocking thread pool) return ASX_E_PERMISSION_DENIED when the
 * browser profile is active.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/runtime/browser_boundary.h>

/* -------------------------------------------------------------------
 * Surface availability matrix
 *
 * 1 = available, 0 = blocked.
 * Indexed as [surface]. Browser profile blocks native-only surfaces;
 * all other profiles grant all surfaces.
 * ------------------------------------------------------------------- */

static const int g_browser_surface_allowed[ASX_SURFACE_COUNT] = {
    /* REGION */ 1,
    /* TASK */ 1,
    /* CHANNEL */ 1,
    /* TIMER */ 1,
    /* OBLIGATION */ 1,
    /* TRACE */ 1,
    /* GHOST */ 1,
    /* FILESYSTEM */ 0,
    /* PROCESS */ 0,
    /* SIGNAL */ 0,
    /* IO_DRIVER */ 0,
    /* BLOCKING */ 0,
    /* SERVER */ 0,
    /* GRPC */ 0,
    /* MESSAGING */ 0,
    /* TLS */ 0,
    /* DATABASE */ 0};

/* -------------------------------------------------------------------
 * Surface names
 * ------------------------------------------------------------------- */

static const char *g_surface_names[ASX_SURFACE_COUNT] = {"region", "task",       "channel",
                                                         "timer",  "obligation", "trace",
                                                         "ghost",  "filesystem", "process",
                                                         "signal", "io_driver",  "blocking",
                                                         "server", "grpc",       "messaging",
                                                         "tls",    "database"};

/* -------------------------------------------------------------------
 * API implementation
 * ------------------------------------------------------------------- */

int asx_surface_available(asx_profile_id profile, asx_host_surface surface) {
    if ((int)surface < 0 || (int)surface >= ASX_SURFACE_COUNT) { return 0; }

    /* Only the browser profile restricts surfaces */
    if (profile != ASX_PROFILE_ID_BROWSER) { return 1; }

    return g_browser_surface_allowed[(int)surface];
}

int asx_surface_available_active(asx_host_surface surface) {
    return asx_surface_available(asx_profile_active(), surface);
}

const char *asx_surface_name(asx_host_surface surface) {
    if ((int)surface < 0 || (int)surface >= ASX_SURFACE_COUNT) { return "unknown"; }
    return g_surface_names[(int)surface];
}

asx_status asx_surface_gate(asx_host_surface surface) {
    if (asx_surface_available_active(surface)) { return ASX_OK; }
    return ASX_E_PERMISSION_DENIED;
}

uint32_t asx_surface_blocked_count(asx_profile_id profile) {
    uint32_t blocked = 0;
    int i;
    for (i = 0; i < ASX_SURFACE_COUNT; i++) {
        if (!asx_surface_available(profile, (asx_host_surface)i)) { blocked++; }
    }
    return blocked;
}

uint32_t asx_surface_available_count(asx_profile_id profile) {
    return (uint32_t)ASX_SURFACE_COUNT - asx_surface_blocked_count(profile);
}
