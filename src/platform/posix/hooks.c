/*
 * posix/hooks.c — POSIX platform adapter
 *
 * Provides clock and entropy hooks for live-mode POSIX targets.
 * Reactor and blocking pool hooks are added by subsequent beads.
 *
 * Walking skeleton (Wave B/C): clock + entropy implemented.
 * Graduation: reactor (epoll/kqueue), blocking pool (pthreads).
 *
 * SPDX-License-Identifier: MIT
 */

#ifdef ASX_PROFILE_POSIX

/* Required for clock_gettime, struct timespec, syscall, O_CLOEXEC */
#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include <asx/asx_config.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Feature detection                                                   */
/* ------------------------------------------------------------------ */

#if defined(__linux__)
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#if defined(SYS_getrandom)
#define ASX_HAS_GETRANDOM 1
#endif
#elif defined(__APPLE__)
#include <sys/random.h>
#include <time.h>
#define ASX_HAS_GETENTROPY 1
#else
#include <time.h>
#endif

/* Fallback: /dev/urandom */
#include <fcntl.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Clock hooks                                                         */
/* ------------------------------------------------------------------ */

static asx_time posix_wall_clock(void *ctx) {
    struct timespec ts;
    (void)ctx;
#if defined(CLOCK_MONOTONIC)
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (asx_time)((uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec);
    }
#endif
    /* Fallback to CLOCK_REALTIME if MONOTONIC unavailable */
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
        return (asx_time)((uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec);
    }
    return 0;
}

static asx_time posix_logical_clock(void *ctx) {
    /* Logical clock for deterministic mode: counter-based, advanced by runtime.
     * POSIX adapter delegates to the default logical clock behavior.
     * In practice this is only used when ASX_DETERMINISTIC=1. */
    static uint64_t g_logical_counter = 0;
    (void)ctx;
    return (asx_time)(++g_logical_counter);
}

/* ------------------------------------------------------------------ */
/* Entropy hooks                                                       */
/* ------------------------------------------------------------------ */

static uint64_t posix_entropy_u64(void *ctx) {
    uint64_t value = 0;
    (void)ctx;

#if defined(ASX_HAS_GETRANDOM)
    {
        long ret = syscall(SYS_getrandom, &value, sizeof(value), 0);
        if (ret == (long)sizeof(value)) return value;
    }
#elif defined(ASX_HAS_GETENTROPY)
    {
        if (getentropy(&value, sizeof(value)) == 0) return value;
    }
#endif

    /* Fallback: /dev/urandom */
    {
        int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            ssize_t n = read(fd, &value, sizeof(value));
            close(fd);
            if (n == (ssize_t)sizeof(value)) return value;
        }
    }

    /* Last resort: hash of pid + timestamp (not cryptographic) */
    {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        {
            uint64_t pid_val = (uint64_t)(unsigned int)getpid();
            uint64_t ns_val = (uint64_t)(unsigned long)ts.tv_nsec;
            value = pid_val * 6364136223846793005ULL + ns_val * 1442695040888963407ULL;
        }
    }
    return value;
}

/* ------------------------------------------------------------------ */
/* Log hook                                                            */
/* ------------------------------------------------------------------ */

static void posix_log_stderr(void *ctx, int level, const char *message) {
    static const char *level_names[] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL"};
    const char *name;
    (void)ctx;
    if (message == NULL) return;
    name = (level >= 0 && level <= 5) ? level_names[level] : "???";
    (void)fprintf(stderr, "[asx:%s] %s\n", name, message);
}

/* ------------------------------------------------------------------ */
/* Install function                                                    */
/* ------------------------------------------------------------------ */

asx_status asx_posix_hooks_install(asx_runtime_hooks *hooks) {
    if (hooks == NULL) return ASX_E_INVALID_ARGUMENT;

    /* Start from safe defaults */
    asx_runtime_hooks_init(hooks);

    /* Override with POSIX implementations */
    hooks->clock.now_ns_fn = posix_wall_clock;
    hooks->clock.logical_now_ns_fn = posix_logical_clock;
    hooks->entropy.random_u64_fn = posix_entropy_u64;
    hooks->log.write_fn = posix_log_stderr;

    /* Reactor and blocking pool are future work (bd-vhzw, bd-8575) */

    return ASX_OK;
}

#else
typedef int asx_no_empty_tu_warning;
#endif /* ASX_PROFILE_POSIX */
