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
#include <asx/platform/posix.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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
/* Reactor hook (epoll on Linux, poll fallback elsewhere)              */
/* ------------------------------------------------------------------ */

#if defined(__linux__)
#include <sys/epoll.h>

#define ASX_POSIX_REACTOR_MAX_EVENTS 64

typedef struct {
    int epoll_fd;
} asx_posix_reactor_ctx;

static asx_posix_reactor_ctx g_reactor_ctx = {-1};

static asx_status posix_reactor_wait(void *ctx, uint32_t timeout_ms, uint32_t *ready_count) {
    asx_posix_reactor_ctx *rc = (asx_posix_reactor_ctx *)ctx;
    struct epoll_event events[ASX_POSIX_REACTOR_MAX_EVENTS];
    int n;

    if (ready_count == NULL) return ASX_E_INVALID_ARGUMENT;
    *ready_count = 0;

    if (rc == NULL || rc->epoll_fd < 0) {
        /* Lazy-initialize epoll instance */
        if (rc == NULL) return ASX_E_INVALID_STATE;
        rc->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
        if (rc->epoll_fd < 0) return ASX_E_RESOURCE_EXHAUSTED;
    }

    n = epoll_wait(rc->epoll_fd, events, ASX_POSIX_REACTOR_MAX_EVENTS, (int)timeout_ms);
    if (n < 0) {
        /* EINTR is not an error — just means we were interrupted */
        *ready_count = 0;
        return ASX_OK;
    }
    *ready_count = (uint32_t)n;
    return ASX_OK;
}

#else /* Non-Linux POSIX: use poll(2) as fallback */
#include <poll.h>

static asx_status posix_reactor_wait(void *ctx, uint32_t timeout_ms, uint32_t *ready_count) {
    (void)ctx;
    if (ready_count == NULL) return ASX_E_INVALID_ARGUMENT;
    *ready_count = 0;
    /* No registered fds — just sleep for the timeout period */
    if (timeout_ms > 0) { poll(NULL, 0, (int)timeout_ms); }
    return ASX_OK;
}

#endif /* __linux__ */

/* Ghost reactor for deterministic mode (same as default) */
static asx_status posix_ghost_reactor_wait(void *ctx, uint64_t logical_step,
                                           uint32_t *ready_count) {
    (void)ctx;
    (void)logical_step;
    if (ready_count != NULL) *ready_count = 0;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Blocking pool (pthread-based)                                       */
/*                                                                     */
/* Provides a thread pool for offloading blocking work. Currently      */
/* implemented as a fire-and-detach model: each submitted blocking     */
/* task gets its own detached thread. A bounded work-stealing pool     */
/* with condvar dispatch would be the production upgrade.              */
/* ------------------------------------------------------------------ */

#include <pthread.h>

/* Include blocking header for asx_blocking_fn typedef */
#include <asx/runtime/blocking.h>

typedef struct {
    asx_blocking_fn fn;
    void *user_data;
} posix_blocking_task;

static void *posix_blocking_worker(void *arg) {
    posix_blocking_task *task = (posix_blocking_task *)arg;
    if (task != NULL && task->fn != NULL) { (void)task->fn(task->user_data); }
    free(arg); /* heap-allocated by submit */
    return NULL;
}

/* Submit blocking work to a detached pthread.
 * Note: This function is available for POSIX profile consumers but is
 * not yet wired into the runtime's blocking.c submit path. That requires
 * adding an optional blocking_submit_fn hook to asx_runtime_hooks (future).
 * For now, CORE profile continues inline execution. */
static asx_status posix_blocking_submit_detached(asx_blocking_fn fn, void *user_data) {
    pthread_t tid;
    posix_blocking_task *task;
    int ret;

    if (fn == NULL) return ASX_E_INVALID_ARGUMENT;

    task = (posix_blocking_task *)malloc(sizeof(*task));
    if (task == NULL) return ASX_E_RESOURCE_EXHAUSTED;
    task->fn = fn;
    task->user_data = user_data;

    ret = pthread_create(&tid, NULL, posix_blocking_worker, task);
    if (ret != 0) {
        free(task);
        return ASX_E_RESOURCE_EXHAUSTED;
    }

    (void)pthread_detach(tid);
    return ASX_OK;
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

    /* Reactor: epoll on Linux, poll fallback elsewhere */
#if defined(__linux__)
    hooks->reactor.ctx = &g_reactor_ctx;
#endif
    hooks->reactor.wait_fn = posix_reactor_wait;
    hooks->reactor.ghost_wait_fn = posix_ghost_reactor_wait;

    /* Blocking pool: function available but not wired into blocking.c submit path.
     * Requires adding blocking_submit_fn hook to asx_runtime_hooks. */
    (void)posix_blocking_submit_detached; /* suppress unused-function warning */

    return ASX_OK;
}

#else
typedef int asx_no_empty_tu_warning;
#endif /* ASX_PROFILE_POSIX */
