/*
 * asx/platform/win32.h — Win32 platform adapter public API
 *
 * Provides asx_win32_hooks_install() which populates asx_runtime_hooks
 * with Win32-native implementations: monotonic clock (QPC), entropy
 * (BCryptGenRandom), reactor (WSAPoll/SleepEx), and log (stderr +
 * OutputDebugString).
 *
 * Usage:
 *   asx_runtime_hooks hooks;
 *   asx_win32_hooks_install(&hooks);
 *   asx_runtime_set_hooks(&hooks);
 *
 * Requires: -DASX_PROFILE_WIN32 at compile time.
 * Links:    -lbcrypt -lws2_32 (or MSVC #pragma comment(lib, ...))
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_PLATFORM_WIN32_H
#define ASX_PLATFORM_WIN32_H

#include <asx/asx_config.h>
#include <asx/asx_export.h>
#include <asx/asx_status.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Install Win32-native hooks into the provided hook table.
 *
 * Calls asx_runtime_hooks_init() for safe defaults, then overlays:
 *   - clock: QueryPerformanceCounter for monotonic nanoseconds
 *   - entropy: BCryptGenRandom with PID+QPC fallback
 *   - reactor: WSAPoll/SleepEx (socket registration is external)
 *   - log: stderr + OutputDebugStringA
 *
 * Blocking pool (CreateThread fire-and-detach) is available internally
 * but not yet wired into the runtime blocking submit path.
 *
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if hooks is NULL. */
ASX_API ASX_MUST_USE asx_status asx_win32_hooks_install(asx_runtime_hooks *hooks);

/* Legacy external-linkage clock/entropy primitives. Prefer using
 * asx_win32_hooks_install() which sets all hooks at once. */
ASX_API asx_time asx_win32_qpc_now_ns(void *ctx);
ASX_API asx_time asx_win32_wall_clock_unix_ns(void *ctx);
ASX_API asx_time asx_win32_logical_clock(void *ctx);
ASX_API uint64_t asx_win32_entropy_u64(void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* ASX_PLATFORM_WIN32_H */
