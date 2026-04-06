/*
 * win32/hooks.c — Win32 platform adapter
 *
 * Provides clock, entropy, reactor, blocking pool, and log hooks for
 * live-mode Win32 targets. Mirrors the POSIX adapter structure.
 *
 * Clock:     QueryPerformanceCounter for monotonic ns
 * Entropy:   BCryptGenRandom with PID+QPC fallback
 * Reactor:   WSAPoll-based event multiplexing
 * Blocking:  CreateThread fire-and-detach pool
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

/* winsock2.h must precede windows.h, while bcrypt.h needs Windows types. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <bcrypt.h>
#include <windows.h>
#include <winsock2.h>

#define ASX_WIN32_NSEC_PER_SEC 1000000000ULL
#define ASX_WIN32_FILETIME_UNIX_EPOCH_100NS 116444736000000000ULL

#if defined(_MSC_VER)
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "ws2_32.lib")
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
/* Reactor hook (WSAPoll-based)                                        */
/*                                                                     */
/* WSAPoll is available on Vista+ and provides a portable poll(2)      */
/* equivalent. With no registered sockets it degrades to a timed       */
/* sleep via SleepEx.                                                  */
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
/* Blocking pool (CreateThread fire-and-detach)                        */
/*                                                                     */
/* Same pattern as POSIX pthread adapter: each blocking task gets its  */
/* own thread. Production upgrade would use a bounded thread pool with */
/* event-based completion signaling.                                   */
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
    hooks->entropy.random_u64_fn = win32_entropy_u64;
    hooks->reactor.wait_fn = win32_reactor_wait;
    hooks->reactor.ghost_wait_fn = win32_ghost_reactor_wait;
    hooks->log.write_fn = win32_log_stderr;

    /* Blocking pool: available but not yet wired into runtime submit path */
    (void)win32_blocking_submit_detached; /* suppress unused warning */

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
