/*
 * posix/hooks.c — POSIX platform adapter
 *
 * Provides clock, entropy, reactor, log, and bounded blocking-submit hooks
 * for live-mode POSIX targets.
 *
 * Blocking-submit uses a fixed pthread pool with bounded queueing; runtime
 * code owns task metadata and drains this pool before reset.
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
#include <errno.h>
#include <sys/epoll.h>

#define ASX_POSIX_REACTOR_MAX_EVENTS 64

typedef struct {
    int epoll_fd;
} asx_posix_reactor_ctx;

static asx_posix_reactor_ctx g_reactor_ctx = {-1};

static asx_status posix_reactor_ensure(asx_posix_reactor_ctx *rc) {
    if (rc == NULL || rc->epoll_fd < 0) {
        /* Lazy-initialize epoll instance */
        if (rc == NULL) return ASX_E_INVALID_STATE;
        rc->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
        if (rc->epoll_fd < 0) return ASX_E_RESOURCE_EXHAUSTED;
    }
    return ASX_OK;
}

static int posix_interest_to_epoll(uint32_t interest, uint32_t *out_events) {
    uint32_t events = 0u;
    uint32_t valid =
        ASX_POSIX_REACTOR_READABLE | ASX_POSIX_REACTOR_WRITABLE | ASX_POSIX_REACTOR_ERROR;

    if (out_events == NULL || interest == 0u || (interest & ~valid) != 0u) { return 0; }
    if ((interest & ASX_POSIX_REACTOR_READABLE) != 0u) { events |= (uint32_t)EPOLLIN; }
    if ((interest & ASX_POSIX_REACTOR_WRITABLE) != 0u) { events |= (uint32_t)EPOLLOUT; }
    if ((interest & ASX_POSIX_REACTOR_ERROR) != 0u) {
        events |= (uint32_t)EPOLLERR | (uint32_t)EPOLLHUP;
    }
    *out_events = events;
    return events != 0u;
}

static asx_status posix_reactor_wait(void *ctx, uint32_t timeout_ms, uint32_t *ready_count) {
    asx_posix_reactor_ctx *rc = (asx_posix_reactor_ctx *)ctx;
    struct epoll_event events[ASX_POSIX_REACTOR_MAX_EVENTS];
    asx_status st;
    int n;

    if (ready_count == NULL) return ASX_E_INVALID_ARGUMENT;
    *ready_count = 0;

    st = posix_reactor_ensure(rc);
    if (st != ASX_OK) return st;

    n = epoll_wait(rc->epoll_fd, events, ASX_POSIX_REACTOR_MAX_EVENTS, (int)timeout_ms);
    if (n < 0) {
        /* EINTR is not an error — just means we were interrupted */
        if (errno != EINTR) return ASX_E_INVALID_STATE;
        *ready_count = 0;
        return ASX_OK;
    }
    *ready_count = (uint32_t)n;
    return ASX_OK;
}

asx_status asx_posix_reactor_register_fd(void *reactor_ctx, int fd, uint32_t interest) {
    asx_posix_reactor_ctx *rc = (asx_posix_reactor_ctx *)reactor_ctx;
    struct epoll_event ev;
    uint32_t events = 0u;
    asx_status st;
    int ret;

    if (fd < 0 || !posix_interest_to_epoll(interest, &events)) return ASX_E_INVALID_ARGUMENT;
    st = posix_reactor_ensure(rc);
    if (st != ASX_OK) return st;

    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.fd = fd;

    ret = epoll_ctl(rc->epoll_fd, EPOLL_CTL_ADD, fd, &ev);
    if (ret == 0) return ASX_OK;
    if (errno == EEXIST && epoll_ctl(rc->epoll_fd, EPOLL_CTL_MOD, fd, &ev) == 0) { return ASX_OK; }
    if (errno == EBADF || errno == EINVAL || errno == EPERM) return ASX_E_INVALID_ARGUMENT;
    return ASX_E_INVALID_STATE;
}

void asx_posix_reactor_deregister_fd(void *reactor_ctx, int fd) {
    asx_posix_reactor_ctx *rc = (asx_posix_reactor_ctx *)reactor_ctx;
    if (rc == NULL || rc->epoll_fd < 0 || fd < 0) return;
    (void)epoll_ctl(rc->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
}

void asx_posix_reactor_reset(void *reactor_ctx) {
    asx_posix_reactor_ctx *rc = (asx_posix_reactor_ctx *)reactor_ctx;
    if (rc == NULL) return;
    if (rc->epoll_fd >= 0) {
        (void)close(rc->epoll_fd);
        rc->epoll_fd = -1;
    }
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

asx_status asx_posix_reactor_register_fd(void *reactor_ctx, int fd, uint32_t interest) {
    (void)reactor_ctx;
    (void)fd;
    (void)interest;
    return ASX_E_PERMISSION_DENIED;
}

void asx_posix_reactor_deregister_fd(void *reactor_ctx, int fd) {
    (void)reactor_ctx;
    (void)fd;
}

void asx_posix_reactor_reset(void *reactor_ctx) { (void)reactor_ctx; }

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
/* ------------------------------------------------------------------ */

#include <pthread.h>

#define ASX_POSIX_BLOCKING_WORKERS 4u
#define ASX_POSIX_BLOCKING_QUEUE_CAPACITY 8u

typedef struct {
    asx_blocking_job_fn job_fn;
    void *job_ctx;
} posix_blocking_task;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t has_work;
    pthread_cond_t drained;
    pthread_t workers[ASX_POSIX_BLOCKING_WORKERS];
    posix_blocking_task queue[ASX_POSIX_BLOCKING_QUEUE_CAPACITY];
    uint32_t head;
    uint32_t tail;
    uint32_t queued;
    uint32_t active;
    uint32_t worker_count;
    int started;
    int stopping;
} posix_blocking_pool;

static posix_blocking_pool g_blocking_pool = {PTHREAD_MUTEX_INITIALIZER,
                                              PTHREAD_COND_INITIALIZER,
                                              PTHREAD_COND_INITIALIZER,
                                              {0},
                                              {{0}},
                                              0u,
                                              0u,
                                              0u,
                                              0u,
                                              0u,
                                              0,
                                              0};

static void posix_blocking_pool_reset_locked(posix_blocking_pool *pool) {
    pool->head = 0u;
    pool->tail = 0u;
    pool->queued = 0u;
    pool->active = 0u;
    pool->worker_count = 0u;
    pool->started = 0;
    pool->stopping = 0;
}

static void *posix_blocking_worker(void *arg) {
    posix_blocking_pool *pool = (posix_blocking_pool *)arg;

    for (;;) {
        posix_blocking_task task;

        pthread_mutex_lock(&pool->mutex);
        while (pool->queued == 0u && !pool->stopping) {
            pthread_cond_wait(&pool->has_work, &pool->mutex);
        }
        if (pool->queued == 0u && pool->stopping) {
            pthread_mutex_unlock(&pool->mutex);
            break;
        }

        task = pool->queue[pool->head];
        pool->head = (pool->head + 1u) % ASX_POSIX_BLOCKING_QUEUE_CAPACITY;
        pool->queued--;
        pool->active++;
        pthread_mutex_unlock(&pool->mutex);

        if (task.job_fn != NULL) { task.job_fn(task.job_ctx); }

        pthread_mutex_lock(&pool->mutex);
        if (pool->active > 0u) { pool->active--; }
        if (pool->queued == 0u && pool->active == 0u) { pthread_cond_broadcast(&pool->drained); }
        pthread_mutex_unlock(&pool->mutex);
    }

    return NULL;
}

static asx_status posix_blocking_pool_start_locked(posix_blocking_pool *pool) {
    uint32_t i;

    if (pool->started) return ASX_OK;

    pool->head = 0u;
    pool->tail = 0u;
    pool->queued = 0u;
    pool->active = 0u;
    pool->worker_count = 0u;
    pool->stopping = 0;

    for (i = 0; i < ASX_POSIX_BLOCKING_WORKERS; ++i) {
        int ret = pthread_create(&pool->workers[i], NULL, posix_blocking_worker, pool);
        if (ret != 0) {
            uint32_t j;
            uint32_t created = pool->worker_count;

            pool->stopping = 1;
            pthread_cond_broadcast(&pool->has_work);
            pthread_mutex_unlock(&pool->mutex);
            for (j = 0; j < created; ++j) { (void)pthread_join(pool->workers[j], NULL); }
            pthread_mutex_lock(&pool->mutex);
            posix_blocking_pool_reset_locked(pool);
            return ASX_E_RESOURCE_EXHAUSTED;
        }
        pool->worker_count++;
    }

    pool->started = 1;
    return ASX_OK;
}

static asx_status posix_blocking_submit(void *ctx, asx_blocking_job_fn job_fn, void *job_ctx) {
    posix_blocking_pool *pool = (posix_blocking_pool *)ctx;
    asx_status st;

    if (pool == NULL || job_fn == NULL) return ASX_E_INVALID_ARGUMENT;

    pthread_mutex_lock(&pool->mutex);
    st = posix_blocking_pool_start_locked(pool);
    if (st != ASX_OK) {
        pthread_mutex_unlock(&pool->mutex);
        return st;
    }
    if (pool->stopping) {
        pthread_mutex_unlock(&pool->mutex);
        return ASX_E_INVALID_STATE;
    }
    if (pool->queued >= ASX_POSIX_BLOCKING_QUEUE_CAPACITY) {
        pthread_mutex_unlock(&pool->mutex);
        return ASX_E_RESOURCE_EXHAUSTED;
    }

    pool->queue[pool->tail].job_fn = job_fn;
    pool->queue[pool->tail].job_ctx = job_ctx;
    pool->tail = (pool->tail + 1u) % ASX_POSIX_BLOCKING_QUEUE_CAPACITY;
    pool->queued++;
    pthread_cond_signal(&pool->has_work);
    pthread_mutex_unlock(&pool->mutex);

    return ASX_OK;
}

static void posix_blocking_shutdown(void *ctx) {
    posix_blocking_pool *pool = (posix_blocking_pool *)ctx;
    pthread_t workers[ASX_POSIX_BLOCKING_WORKERS];
    uint32_t worker_count;
    uint32_t i;

    if (pool == NULL) return;

    pthread_mutex_lock(&pool->mutex);
    if (!pool->started) {
        posix_blocking_pool_reset_locked(pool);
        pthread_mutex_unlock(&pool->mutex);
        return;
    }

    while (pool->queued > 0u || pool->active > 0u) {
        pthread_cond_wait(&pool->drained, &pool->mutex);
    }

    pool->stopping = 1;
    pthread_cond_broadcast(&pool->has_work);
    worker_count = pool->worker_count;
    for (i = 0; i < worker_count; ++i) { workers[i] = pool->workers[i]; }
    pthread_mutex_unlock(&pool->mutex);

    for (i = 0; i < worker_count; ++i) { (void)pthread_join(workers[i], NULL); }

    pthread_mutex_lock(&pool->mutex);
    posix_blocking_pool_reset_locked(pool);
    pthread_mutex_unlock(&pool->mutex);
}

static uint32_t posix_blocking_capacity(void *ctx) {
    (void)ctx;
    return ASX_POSIX_BLOCKING_QUEUE_CAPACITY;
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

    hooks->blocking.ctx = &g_blocking_pool;
    hooks->blocking.submit_fn = posix_blocking_submit;
    hooks->blocking.shutdown_fn = posix_blocking_shutdown;
    hooks->blocking.capacity_fn = posix_blocking_capacity;

    return ASX_OK;
}

#else
typedef int asx_no_empty_tu_warning;
#endif /* ASX_PROFILE_POSIX */
