/*
 * win32/hooks.c — Win32 platform adapter
 *
 * Provides clock, entropy, reactor, and log hooks for Win32 targets.
 * Unsupported socket registration, IOCP, and runtime blocking-pool
 * integration remain explicit fail-closed surfaces.
 *
 * Clock:     QueryPerformanceCounter for monotonic ns
 * Entropy:   deterministic PRNG in deterministic builds; BCryptGenRandom in live builds
 * Reactor:   SleepEx timed wait that reports no readiness until registration lands
 * Blocking:  no hook installed until a bounded Win32 pool lands
 * Log:       OutputDebugStringA + stderr
 *
 * SPDX-License-Identifier: MIT
 */

#ifdef ASX_PROFILE_WIN32

#include <asx/asx_config.h>
#include <asx/platform/win32.h>
#include <asx/runtime/blocking.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Include order: windows.h before bcrypt.h for Windows types. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
/* clang-format off */
#include <windows.h>
#include <bcrypt.h>
/* clang-format on */

#define ASX_WIN32_NSEC_PER_SEC 1000000000ULL
#define ASX_WIN32_FILETIME_UNIX_EPOCH_100NS 116444736000000000ULL

#if defined(_MSC_VER)
#pragma comment(lib, "bcrypt.lib")
#endif

/* ------------------------------------------------------------------ */
/* Clock hooks                                                         */
/* ------------------------------------------------------------------ */

static asx_time win32_qpc_now_ns(void *ctx) {
    LARGE_INTEGER counter;
    LARGE_INTEGER frequency;
    uint64_t ticks;
    uint64_t hz;
    uint64_t seconds;
    uint64_t remainder;
    uint64_t nanos;
    (void)ctx;

    if (!QueryPerformanceCounter(&counter)) return 0;
    if (!QueryPerformanceFrequency(&frequency)) return 0;
    if (frequency.QuadPart <= 0) return 0;

    ticks = (uint64_t)counter.QuadPart;
    hz = (uint64_t)frequency.QuadPart;
    seconds = ticks / hz;
    remainder = ticks % hz;
    nanos = seconds * ASX_WIN32_NSEC_PER_SEC;
    nanos += (remainder * ASX_WIN32_NSEC_PER_SEC) / hz;

    return (asx_time)nanos;
}

static asx_time win32_logical_clock(void *ctx) {
    static uint64_t g_logical_counter = 0;
    (void)ctx;
    return (asx_time)(++g_logical_counter);
}

/* ------------------------------------------------------------------ */
/* Entropy hook                                                        */
/* ------------------------------------------------------------------ */

static uint64_t win32_entropy_u64(void *ctx) {
    unsigned char bytes[sizeof(uint64_t)];
    uint64_t value = 0;
    NTSTATUS status;
    (void)ctx;

    status = BCryptGenRandom(NULL, bytes, (ULONG)sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (BCRYPT_SUCCESS(status)) {
        memcpy(&value, bytes, sizeof(value));
        return value;
    }

    /* Fallback keeps the hook non-zero even if BCrypt is unavailable. */
    value = (uint64_t)GetCurrentProcessId() << 32;
    value ^= (uint64_t)win32_qpc_now_ns(NULL);
    return value;
}

/* ------------------------------------------------------------------ */
/* Reactor hook                                                        */
/*                                                                     */
/* Win32 socket registration and IOCP are not enabled by this slice.    */
/* The hook is intentionally fail-closed: it can provide a timed wait   */
/* boundary for live profiles, but reports zero readiness until a       */
/* future registration API owns socket/IOCP delivery.                  */
/* ------------------------------------------------------------------ */

static asx_status win32_reactor_wait(void *ctx, uint32_t timeout_ms, uint32_t *ready_count) {
    (void)ctx;
    if (ready_count == NULL) return ASX_E_INVALID_ARGUMENT;
    *ready_count = 0;

    /* No registered fds — sleep for timeout period (like POSIX poll fallback) */
    if (timeout_ms > 0) { SleepEx((DWORD)timeout_ms, TRUE); }
    return ASX_OK;
}

static asx_status win32_ghost_reactor_wait(void *ctx, uint64_t logical_step,
                                           uint32_t *ready_count) {
    (void)ctx;
    (void)logical_step;
    if (ready_count != NULL) *ready_count = 0;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Blocking pool placeholder                                           */
/*                                                                     */
/* Kept private and unwired. Runtime hooks must not claim blocking      */
/* support until a bounded Win32 pool with shutdown/drain semantics      */
/* lands.                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_blocking_fn fn;
    void *user_data;
} win32_blocking_task;

static DWORD WINAPI win32_blocking_worker(LPVOID arg) {
    win32_blocking_task *task = (win32_blocking_task *)arg;
    if (task != NULL && task->fn != NULL) { (void)task->fn(task->user_data); }
    HeapFree(GetProcessHeap(), 0, arg);
    return 0;
}

static asx_status win32_blocking_submit_detached(asx_blocking_fn fn, void *user_data) {
    HANDLE thread;
    win32_blocking_task *task;

    if (fn == NULL) return ASX_E_INVALID_ARGUMENT;

    task = (win32_blocking_task *)HeapAlloc(GetProcessHeap(), 0, sizeof(*task));
    if (task == NULL) return ASX_E_RESOURCE_EXHAUSTED;
    task->fn = fn;
    task->user_data = user_data;

    thread = CreateThread(NULL, 0, win32_blocking_worker, task, 0, NULL);
    if (thread == NULL) {
        HeapFree(GetProcessHeap(), 0, task);
        return ASX_E_RESOURCE_EXHAUSTED;
    }

    CloseHandle(thread); /* detach */
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Log hook                                                            */
/* ------------------------------------------------------------------ */

static void win32_log_stderr(void *ctx, int level, const char *message) {
    static const char *level_names[] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL"};
    const char *name;
    (void)ctx;
    if (message == NULL) return;
    name = (level >= 0 && level <= 5) ? level_names[level] : "???";

    /* Write to both stderr and OutputDebugString for debugger visibility */
    (void)fprintf(stderr, "[asx:%s] %s\n", name, message);
    OutputDebugStringA("[asx:");
    OutputDebugStringA(name);
    OutputDebugStringA("] ");
    OutputDebugStringA(message);
    OutputDebugStringA("\n");
}

/* ------------------------------------------------------------------ */
/* Install function                                                    */
/* ------------------------------------------------------------------ */

asx_status asx_win32_hooks_install(asx_runtime_hooks *hooks) {
    if (hooks == NULL) return ASX_E_INVALID_ARGUMENT;

    /* Start from safe defaults */
    asx_runtime_hooks_init(hooks);

    /* Override with Win32 implementations */
    hooks->clock.now_ns_fn = win32_qpc_now_ns;
    hooks->clock.logical_now_ns_fn = win32_logical_clock;

#if ASX_DETERMINISTIC
    /* Keep the default seeded PRNG in deterministic builds.  The
     * BCrypt-backed primitive remains available through
     * asx_win32_entropy_u64(), but installing it as the runtime entropy
     * hook would mislabel ambient entropy as replay-safe. */
#else
    hooks->entropy.random_u64_fn = win32_entropy_u64;
    hooks->deterministic_seeded_prng = 0u;
#endif

    hooks->reactor.wait_fn = win32_reactor_wait;
    hooks->reactor.ghost_wait_fn = win32_ghost_reactor_wait;
    hooks->log.write_fn = win32_log_stderr;

    /* Blocking pool: deliberately not installed until bounded Win32
     * queueing and deterministic shutdown/drain semantics are present. */
    hooks->blocking.ctx = NULL;
    hooks->blocking.submit_fn = NULL;
    hooks->blocking.shutdown_fn = NULL;
    hooks->blocking.capacity_fn = NULL;
    (void)win32_blocking_submit_detached;

    return ASX_OK;
}

/* Legacy external-linkage wrappers for code that calls the old public
 * names directly.  Delegate to the static implementations above. */
asx_time asx_win32_qpc_now_ns(void *ctx) { return win32_qpc_now_ns(ctx); }

asx_time asx_win32_wall_clock_unix_ns(void *ctx) {
    FILETIME ft;
    ULARGE_INTEGER value;
    uint64_t filetime_100ns;
    (void)ctx;

    GetSystemTimeAsFileTime(&ft);
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    filetime_100ns = (uint64_t)value.QuadPart;

    if (filetime_100ns < ASX_WIN32_FILETIME_UNIX_EPOCH_100NS) return 0;
    return (asx_time)((filetime_100ns - ASX_WIN32_FILETIME_UNIX_EPOCH_100NS) * 100ULL);
}

asx_time asx_win32_logical_clock(void *ctx) { return win32_logical_clock(ctx); }

uint64_t asx_win32_entropy_u64(void *ctx) { return win32_entropy_u64(ctx); }

#else
typedef int asx_no_empty_tu_warning;
#endif
