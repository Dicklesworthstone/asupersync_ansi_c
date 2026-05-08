/*
 * asx/platform/posix.h — POSIX platform adapter public API
 *
 * Provides asx_posix_hooks_install() which populates asx_runtime_hooks
 * with POSIX-native implementations: monotonic clock (clock_gettime),
 * entropy (getrandom/urandom), and stderr logging.
 *
 * Usage:
 *   asx_runtime_hooks hooks;
 *   asx_posix_hooks_install(&hooks);
 *   asx_runtime_set_hooks(&hooks);
 *
 * Requires: -DASX_PROFILE_POSIX at compile time.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_PLATFORM_POSIX_H
#define ASX_PLATFORM_POSIX_H

#include <asx/asx_config.h>
#include <asx/asx_export.h>
#include <asx/asx_status.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Install POSIX-native hooks into the provided hook table.
 *
 * Calls asx_runtime_hooks_init() for safe defaults, then overlays:
 *   - clock: clock_gettime(CLOCK_MONOTONIC) for wall time
 *   - entropy: getrandom(2) with /dev/urandom fallback
 *   - reactor: epoll (Linux) or poll(2) fallback
 *   - log: stderr fprintf sink
 *   - blocking: bounded pthread worker queue for live-mode blocking submit
 *
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if hooks is NULL. */
ASX_API ASX_MUST_USE asx_status asx_posix_hooks_install(asx_runtime_hooks *hooks);

#ifdef __cplusplus
}
#endif

#endif /* ASX_PLATFORM_POSIX_H */
