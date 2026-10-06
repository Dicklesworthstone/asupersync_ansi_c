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
/* Reactor (epoll on Linux, poll(2) elsewhere)                         */
/*                                                                     */
/* Readiness is one-shot: an armed fd reports once and is disarmed     */
/* until re-armed, so a perpetually writable socket cannot spin the    */
/* scheduler's idle loop. The count-only wait (wait_fn) never consumes */
/* arming: it is used to sleep until "something is ready".            */
/* Define ASX_POSIX_REACTOR_FORCE_POLL to use poll(2) on Linux too.    */
/* ------------------------------------------------------------------ */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#if defined(__linux__) && !defined(ASX_POSIX_REACTOR_FORCE_POLL)
#define ASX_POSIX_REACTOR_EPOLL 1
#include <sys/epoll.h>
#else
#define ASX_POSIX_REACTOR_EPOLL 0
#endif

#define ASX_POSIX_REACTOR_MAX_EVENTS 64

#ifndef ASX_POSIX_REACTOR_MAX_FDS
#define ASX_POSIX_REACTOR_MAX_FDS 256u
#endif

typedef struct {
    int fd;
    uint32_t interest;
    uint64_t token;
    int armed;
} posix_reactor_entry;

typedef struct {
    int epoll_fd;
    /* Registration table: authoritative for poll(2); for epoll it tracks
     * which fds were added so re-arming uses MOD instead of ADD. */
    posix_reactor_entry entries[ASX_POSIX_REACTOR_MAX_FDS];
    uint32_t count;
    /* Self-pipe for cross-thread notify: the read end is part of every
     * wait/poll, the write end is written by posix_reactor_notify(). */
    int wake_rd;
    int wake_wr;
} asx_posix_reactor_ctx;

static asx_posix_reactor_ctx g_reactor_ctx = {-1, {{0, 0u, 0u, 0}}, 0u, -1, -1};

/* Reserved epoll token for the wake pipe (never handed to callers). */
#define ASX_POSIX_REACTOR_WAKE_TOKEN UINT64_MAX

static const uint32_t posix_valid_interest =
    ASX_POSIX_REACTOR_READABLE | ASX_POSIX_REACTOR_WRITABLE | ASX_POSIX_REACTOR_ERROR;

static int posix_set_nonblock_cloexec(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) != 0) return -1;
    fl = fcntl(fd, F_GETFD, 0);
    if (fl < 0 || fcntl(fd, F_SETFD, fl | FD_CLOEXEC) != 0) return -1;
    return 0;
}

/* Create the wake pipe (once). Failure leaves notify a no-op: blocked
 * waits then fall back to their timeout. */
static void posix_reactor_wake_ensure(asx_posix_reactor_ctx *rc) {
    int fds[2];
    if (rc->wake_rd >= 0) return;
    if (pipe(fds) != 0) return;
    if (posix_set_nonblock_cloexec(fds[0]) != 0 || posix_set_nonblock_cloexec(fds[1]) != 0) {
        (void)close(fds[0]);
        (void)close(fds[1]);
        return;
    }
#if ASX_POSIX_REACTOR_EPOLL
    if (rc->epoll_fd >= 0) {
        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = (uint32_t)EPOLLIN; /* level-triggered until drained */
        ev.data.u64 = ASX_POSIX_REACTOR_WAKE_TOKEN;
        if (epoll_ctl(rc->epoll_fd, EPOLL_CTL_ADD, fds[0], &ev) != 0) {
            (void)close(fds[0]);
            (void)close(fds[1]);
            return;
        }
    }
#endif
    rc->wake_rd = fds[0];
    rc->wake_wr = fds[1];
}

static void posix_reactor_wake_drain(asx_posix_reactor_ctx *rc) {
    char buf[64];
    if (rc->wake_rd < 0) return;
    while (read(rc->wake_rd, buf, sizeof(buf)) > 0) {
        ASX_CHECKPOINT_WAIVER("bounded: pipe holds a finite number of wake bytes");
    }
}

static void posix_reactor_notify(void *ctx) {
    asx_posix_reactor_ctx *rc = (asx_posix_reactor_ctx *)ctx;
    char b = 1;
    ssize_t n;
    if (rc == NULL || rc->wake_wr < 0) return;
    /* EAGAIN means the pipe is full: a wake is already pending. */
    n = write(rc->wake_wr, &b, 1);
    (void)n;
}

static asx_status posix_reactor_ensure(asx_posix_reactor_ctx *rc) {
    if (rc == NULL) return ASX_E_INVALID_STATE;
#if ASX_POSIX_REACTOR_EPOLL
    if (rc->epoll_fd < 0) {
        rc->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
        if (rc->epoll_fd < 0) return ASX_E_RESOURCE_EXHAUSTED;
    }
#endif
    posix_reactor_wake_ensure(rc);
    return ASX_OK;
}

static posix_reactor_entry *posix_reactor_find(asx_posix_reactor_ctx *rc, int fd) {
    uint32_t i;
    for (i = 0; i < rc->count; i++) {
        if (rc->entries[i].fd == fd) return &rc->entries[i];
    }
    return NULL;
}

/* Map native readiness to ASX ready bits. Errors and hang-ups wake every
 * armed direction so the waiter observes the failure on its next call. */
static uint32_t posix_ready_bits(int readable, int writable, int failed, uint32_t armed) {
    uint32_t ready = 0u;
    if (readable) ready |= ASX_POSIX_REACTOR_READABLE;
    if (writable) ready |= ASX_POSIX_REACTOR_WRITABLE;
    if (failed) {
        ready |= ASX_POSIX_REACTOR_ERROR;
        ready |= armed & (ASX_POSIX_REACTOR_READABLE | ASX_POSIX_REACTOR_WRITABLE);
    }
    return ready;
}

static asx_status posix_reactor_arm(void *ctx, int fd, uint32_t interest, uint64_t token) {
    asx_posix_reactor_ctx *rc = (asx_posix_reactor_ctx *)ctx;
    posix_reactor_entry *e;
    asx_status st;

    if (rc == NULL) return ASX_E_INVALID_STATE;
    if (fd < 0 || interest == 0u || (interest & ~posix_valid_interest) != 0u) {
        return ASX_E_INVALID_ARGUMENT;
    }
    st = posix_reactor_ensure(rc);
    if (st != ASX_OK) return st;

    e = posix_reactor_find(rc, fd);
#if ASX_POSIX_REACTOR_EPOLL
    {
        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = (uint32_t)EPOLLONESHOT | (uint32_t)EPOLLRDHUP;
        if ((interest & ASX_POSIX_REACTOR_READABLE) != 0u) ev.events |= (uint32_t)EPOLLIN;
        if ((interest & ASX_POSIX_REACTOR_WRITABLE) != 0u) ev.events |= (uint32_t)EPOLLOUT;
        ev.data.u64 = token;
        if (e != NULL) {
            if (epoll_ctl(rc->epoll_fd, EPOLL_CTL_MOD, fd, &ev) != 0) {
                if (errno != ENOENT || epoll_ctl(rc->epoll_fd, EPOLL_CTL_ADD, fd, &ev) != 0) {
                    return (errno == EBADF || errno == EPERM) ? ASX_E_INVALID_ARGUMENT
                                                              : ASX_E_INVALID_STATE;
                }
            }
        } else {
            if (rc->count >= ASX_POSIX_REACTOR_MAX_FDS) return ASX_E_RESOURCE_EXHAUSTED;
            if (epoll_ctl(rc->epoll_fd, EPOLL_CTL_ADD, fd, &ev) != 0) {
                if (errno != EEXIST || epoll_ctl(rc->epoll_fd, EPOLL_CTL_MOD, fd, &ev) != 0) {
                    return (errno == EBADF || errno == EPERM || errno == EINVAL)
                               ? ASX_E_INVALID_ARGUMENT
                               : ASX_E_INVALID_STATE;
                }
            }
        }
    }
#else
    if (e == NULL && rc->count >= ASX_POSIX_REACTOR_MAX_FDS) return ASX_E_RESOURCE_EXHAUSTED;
#endif
    if (e == NULL) {
        e = &rc->entries[rc->count++];
        e->fd = fd;
    }
    e->interest = interest;
    e->token = token;
    e->armed = 1;
    return ASX_OK;
}

static void posix_reactor_forget(void *ctx, int fd) {
    asx_posix_reactor_ctx *rc = (asx_posix_reactor_ctx *)ctx;
    posix_reactor_entry *e;

    if (rc == NULL || fd < 0) return;
    e = posix_reactor_find(rc, fd);
    if (e == NULL) return;
#if ASX_POSIX_REACTOR_EPOLL
    if (rc->epoll_fd >= 0) (void)epoll_ctl(rc->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
#endif
    *e = rc->entries[rc->count - 1u];
    rc->count--;
}

static int posix_timeout_arg(uint32_t timeout_ms) {
    return timeout_ms > (uint32_t)INT32_MAX ? INT32_MAX : (int)timeout_ms;
}

static asx_status posix_reactor_poll(void *ctx, uint32_t timeout_ms, asx_reactor_event *out,
                                     uint32_t max_events, uint32_t *out_count) {
    asx_posix_reactor_ctx *rc = (asx_posix_reactor_ctx *)ctx;
    asx_status st;

    if (out_count == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    *out_count = 0;
    if (max_events == 0u) return ASX_OK;
    st = posix_reactor_ensure(rc);
    if (st != ASX_OK) return st;

#if ASX_POSIX_REACTOR_EPOLL
    {
        struct epoll_event events[ASX_POSIX_REACTOR_MAX_EVENTS];
        int cap = max_events < (uint32_t)ASX_POSIX_REACTOR_MAX_EVENTS
                      ? (int)max_events
                      : ASX_POSIX_REACTOR_MAX_EVENTS;
        int n = epoll_wait(rc->epoll_fd, events, cap, posix_timeout_arg(timeout_ms));
        int i;
        if (n < 0) return errno == EINTR ? ASX_OK : ASX_E_INVALID_STATE;
        for (i = 0; i < n; i++) {
            uint32_t ev = events[i].events;
            uint32_t armed = ASX_POSIX_REACTOR_READABLE | ASX_POSIX_REACTOR_WRITABLE;
            uint32_t j;
            if (events[i].data.u64 == ASX_POSIX_REACTOR_WAKE_TOKEN) {
                posix_reactor_wake_drain(rc);
                continue;
            }
            /* Disarm the matching table entry (one-shot). */
            for (j = 0; j < rc->count; j++) {
                if (rc->entries[j].token == events[i].data.u64 && rc->entries[j].armed) {
                    armed = rc->entries[j].interest;
                    rc->entries[j].armed = 0;
                    break;
                }
            }
            out[*out_count].token = events[i].data.u64;
            out[*out_count].ready =
                posix_ready_bits((ev & ((uint32_t)EPOLLIN | (uint32_t)EPOLLRDHUP)) != 0u,
                                 (ev & (uint32_t)EPOLLOUT) != 0u,
                                 (ev & ((uint32_t)EPOLLERR | (uint32_t)EPOLLHUP)) != 0u, armed);
            (*out_count)++;
        }
    }
#else
    {
        struct pollfd pfds[ASX_POSIX_REACTOR_MAX_FDS + 1u];
        uint32_t map[ASX_POSIX_REACTOR_MAX_FDS];
        uint32_t i;
        uint32_t n = 0;
        uint32_t nfds;
        int rv;

        for (i = 0; i < rc->count; i++) {
            if (!rc->entries[i].armed) continue;
            pfds[n].fd = rc->entries[i].fd;
            pfds[n].events = 0;
            if ((rc->entries[i].interest & ASX_POSIX_REACTOR_READABLE) != 0u) {
                pfds[n].events |= POLLIN;
            }
            if ((rc->entries[i].interest & ASX_POSIX_REACTOR_WRITABLE) != 0u) {
                pfds[n].events |= POLLOUT;
            }
            pfds[n].revents = 0;
            map[n] = i;
            n++;
        }
        nfds = n;
        if (rc->wake_rd >= 0) {
            pfds[nfds].fd = rc->wake_rd;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            nfds++;
        }
        rv = poll(nfds > 0u ? pfds : NULL, (nfds_t)nfds, posix_timeout_arg(timeout_ms));
        if (rv < 0) return errno == EINTR ? ASX_OK : ASX_E_INVALID_STATE;
        if (nfds > n && pfds[n].revents != 0) posix_reactor_wake_drain(rc);
        for (i = 0; i < n && *out_count < max_events; i++) {
            posix_reactor_entry *e;
            if (pfds[i].revents == 0) continue;
            e = &rc->entries[map[i]];
            out[*out_count].token = e->token;
            out[*out_count].ready =
                posix_ready_bits((pfds[i].revents & POLLIN) != 0, (pfds[i].revents & POLLOUT) != 0,
                                 (pfds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0,
                                 e->interest);
            e->armed = 0;
            (*out_count)++;
        }
    }
#endif
    return ASX_OK;
}

/* Count-only wait: sleeps until something registered is ready or the
 * timeout passes, without consuming one-shot arming. */
static asx_status posix_reactor_wait(void *ctx, uint32_t timeout_ms, uint32_t *ready_count) {
    asx_posix_reactor_ctx *rc = (asx_posix_reactor_ctx *)ctx;
    asx_status st;
    int rv;

    if (ready_count == NULL) return ASX_E_INVALID_ARGUMENT;
    *ready_count = 0;
    st = posix_reactor_ensure(rc);
    if (st != ASX_OK) return st;

#if ASX_POSIX_REACTOR_EPOLL
    {
        /* The epoll fd itself is readable while it holds pending events. */
        struct pollfd pfd;
        pfd.fd = rc->epoll_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        rv = poll(&pfd, 1, posix_timeout_arg(timeout_ms));
        if (rv < 0) return errno == EINTR ? ASX_OK : ASX_E_INVALID_STATE;
        *ready_count = (rv > 0 && (pfd.revents & POLLIN) != 0) ? 1u : 0u;
        /* The wake pipe lives in the epoll set: consume any notify. */
        if (*ready_count > 0u) posix_reactor_wake_drain(rc);
    }
#else
    {
        struct pollfd pfds[ASX_POSIX_REACTOR_MAX_FDS + 1u];
        uint32_t i;
        uint32_t n = 0;
        for (i = 0; i < rc->count; i++) {
            if (!rc->entries[i].armed) continue;
            pfds[n].fd = rc->entries[i].fd;
            pfds[n].events = 0;
            if ((rc->entries[i].interest & ASX_POSIX_REACTOR_READABLE) != 0u) {
                pfds[n].events |= POLLIN;
            }
            if ((rc->entries[i].interest & ASX_POSIX_REACTOR_WRITABLE) != 0u) {
                pfds[n].events |= POLLOUT;
            }
            pfds[n].revents = 0;
            n++;
        }
        if (rc->wake_rd >= 0) {
            pfds[n].fd = rc->wake_rd;
            pfds[n].events = POLLIN;
            pfds[n].revents = 0;
            n++;
        }
        rv = poll(n > 0u ? pfds : NULL, (nfds_t)n, posix_timeout_arg(timeout_ms));
        if (rv < 0) return errno == EINTR ? ASX_OK : ASX_E_INVALID_STATE;
        *ready_count = (uint32_t)rv;
        if (rc->wake_rd >= 0 && pfds[n - 1u].revents != 0) posix_reactor_wake_drain(rc);
    }
#endif
    return ASX_OK;
}

asx_status asx_posix_reactor_register_fd(void *reactor_ctx, int fd, uint32_t interest) {
    /* Token defaults to the fd itself for direct users of this helper. */
    return posix_reactor_arm(reactor_ctx, fd, interest, (uint64_t)(uint32_t)fd);
}

void asx_posix_reactor_deregister_fd(void *reactor_ctx, int fd) {
    posix_reactor_forget(reactor_ctx, fd);
}

void asx_posix_reactor_reset(void *reactor_ctx) {
    asx_posix_reactor_ctx *rc = (asx_posix_reactor_ctx *)reactor_ctx;
    if (rc == NULL) return;
#if ASX_POSIX_REACTOR_EPOLL
    if (rc->epoll_fd >= 0) {
        (void)close(rc->epoll_fd);
        rc->epoll_fd = -1;
    }
#endif
    /* The wake pipe was registered in the closed epoll set: recreate it
     * with the next one. */
    if (rc->wake_rd >= 0) {
        (void)close(rc->wake_rd);
        (void)close(rc->wake_wr);
        rc->wake_rd = -1;
        rc->wake_wr = -1;
    }
    rc->count = 0;
}

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

    /* Reactor: epoll on Linux, poll(2) elsewhere; one-shot fd readiness */
    hooks->reactor.ctx = &g_reactor_ctx;
    hooks->reactor.wait_fn = posix_reactor_wait;
    hooks->reactor.ghost_wait_fn = posix_ghost_reactor_wait;
    hooks->reactor.register_fn = posix_reactor_arm;
    hooks->reactor.deregister_fn = posix_reactor_forget;
    hooks->reactor.poll_fn = posix_reactor_poll;
    hooks->reactor.notify_fn = posix_reactor_notify;
    /* Create the epoll set and wake pipe now, before any pool thread can
     * notify, so no wake is lost to lazy creation. */
    (void)posix_reactor_ensure(&g_reactor_ctx);

    hooks->blocking.ctx = &g_blocking_pool;
    hooks->blocking.submit_fn = posix_blocking_submit;
    hooks->blocking.shutdown_fn = posix_blocking_shutdown;
    hooks->blocking.capacity_fn = posix_blocking_capacity;

    return ASX_OK;
}

#else
typedef int asx_no_empty_tu_warning;
#endif /* ASX_PROFILE_POSIX */
