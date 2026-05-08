/*
 * asx/platform/posix.h — POSIX platform adapter public API
 *
 * Provides asx_posix_hooks_install() which populates asx_runtime_hooks
 * with POSIX-native implementations: monotonic clock (clock_gettime),
 * entropy (getrandom/urandom), timed reactor wait, stderr logging, and a
 * bounded blocking-submit queue.
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

#define ASX_POSIX_REACTOR_READABLE 0x01u
#define ASX_POSIX_REACTOR_WRITABLE 0x02u
#define ASX_POSIX_REACTOR_ERROR 0x04u

#define ASX_POSIX_HAS_TIMED_REACTOR_WAIT 1u
#if defined(__linux__)
#define ASX_POSIX_HAS_REACTOR_FD_REGISTRATION 1u
#else
#define ASX_POSIX_HAS_REACTOR_FD_REGISTRATION 0u
#endif

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

/* Register a POSIX file descriptor with the reactor context installed in
 * hooks.reactor.ctx. interest is a nonzero mask of ASX_POSIX_REACTOR_* bits.
 *
 * Linux builds use epoll and return ASX_OK on successful ADD or MOD.
 * Non-Linux POSIX builds keep the timed poll fallback fail-closed for fd
 * registration and return ASX_E_PERMISSION_DENIED. */
ASX_API ASX_MUST_USE asx_status asx_posix_reactor_register_fd(void *reactor_ctx, int fd,
                                                              uint32_t interest);

/* Deregister a file descriptor from the POSIX reactor context. Unknown or
 * already-removed descriptors are ignored so cleanup paths stay idempotent. */
ASX_API void asx_posix_reactor_deregister_fd(void *reactor_ctx, int fd);

/* Reset the POSIX reactor context, closing any platform handle it owns. */
ASX_API void asx_posix_reactor_reset(void *reactor_ctx);

#ifdef __cplusplus
}
#endif

#endif /* ASX_PLATFORM_POSIX_H */
