/*
 * posix/signal_posix.c — native POSIX signals for the signal API
 *
 * Self-pipe trick: the handler installed for a subscribed kind performs
 * only async-signal-safe work — it writes the kind's index (one byte) to
 * a non-blocking, close-on-exec pipe and, for INT/TERM, sets a
 * sig_atomic_t shutdown flag; errno is preserved. If the pipe is full the
 * kind's overflow flag guarantees at least one delivery is still counted.
 *
 * Waiting: each waiting subscription gets its own close-on-exec duplicate
 * of the pipe's read end registered with the IO driver, so every parked
 * subscriber has an independent one-shot readiness registration (the IO
 * driver wakes exactly one task per registration). Whoever polls first
 * drains the pipe and signal.c wakes the waiters of every subscription
 * that received a delivery. Registrations are always deregistered before
 * the duplicate is closed, so no stale epoll entry survives.
 *
 * The pipe itself lives for the rest of the process once created: a
 * handler running concurrently on another thread may still hold its write
 * end, so closing it could redirect that byte into an unrelated, reused
 * descriptor. Reset drains it instead.
 *
 * Handlers use SA_RESTART; a reactor wait interrupted by a signal returns
 * early and the next wait observes the readable pipe.
 *
 * SPDX-License-Identifier: MIT
 */

#ifdef ASX_PROFILE_POSIX

#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "../../signal/signal_native.h"
#include <asx/runtime/io_driver.h>
#include <asx/runtime/runtime.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    int open;
    int fd;
    int registered;
    asx_io_token token;
} sig_waiter;

static volatile sig_atomic_t g_pipe_wr = -1;
static int g_pipe_rd = -1;
static volatile sig_atomic_t g_overflow[ASX_SIGNAL_KIND_COUNT];
static volatile sig_atomic_t g_native_shutdown;
static uint32_t g_refcount[ASX_SIGNAL_KIND_COUNT];
static int g_installed[ASX_SIGNAL_KIND_COUNT];
static struct sigaction g_saved[ASX_SIGNAL_KIND_COUNT];
static sig_waiter g_waiters[ASX_MAX_SIGNAL_SUBSCRIPTIONS];

static int sig_os_number(int index) {
    switch (index) {
    case 0: return SIGHUP;
    case 1: return SIGINT;
    case 2: return SIGUSR1;
    case 3: return SIGUSR2;
    default: return SIGTERM;
    }
}

/* Async-signal-safe: comparisons, one write(2), sig_atomic_t stores. */
static void sig_handler(int signo) {
    int saved_errno = errno;
    int index = -1;
    if (signo == SIGHUP) index = 0;
    if (signo == SIGINT) index = 1;
    if (signo == SIGUSR1) index = 2;
    if (signo == SIGUSR2) index = 3;
    if (signo == SIGTERM) index = 4;
    if (index >= 0) {
        unsigned char code = (unsigned char)index;
        int fd = (int)g_pipe_wr;
        if (signo == SIGINT || signo == SIGTERM) g_native_shutdown = 1;
        if (fd < 0 || write(fd, &code, 1u) != 1) g_overflow[index] = 1;
    }
    errno = saved_errno;
}

static asx_status sig_errno_status(int err) {
    switch (err) {
    case EMFILE:
    case ENFILE:
    case ENOMEM: return ASX_E_RESOURCE_EXHAUSTED;
    case EINVAL: return ASX_E_INVALID_ARGUMENT;
    case EPERM: return ASX_E_PERMISSION_DENIED;
    default: return ASX_E_INVALID_STATE;
    }
}

#if !defined(__linux__)
static int sig_set_flags(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) return -1;
    fl = fcntl(fd, F_GETFD, 0);
    if (fl < 0 || fcntl(fd, F_SETFD, fl | FD_CLOEXEC) < 0) return -1;
    return 0;
}
#endif

static asx_status sig_ensure_pipe(void) {
    int fds[2];
    if (g_pipe_rd >= 0) return ASX_OK;
#if defined(__linux__)
    if (pipe2(fds, O_NONBLOCK | O_CLOEXEC) != 0) return sig_errno_status(errno);
#else
    if (pipe(fds) != 0) return sig_errno_status(errno);
    if (sig_set_flags(fds[0]) != 0 || sig_set_flags(fds[1]) != 0) {
        asx_status st = sig_errno_status(errno);
        (void)close(fds[0]);
        (void)close(fds[1]);
        return st;
    }
#endif
    g_pipe_rd = fds[0];
    g_pipe_wr = fds[1];
    return ASX_OK;
}

void asx_native_signal_reset(void) {
    uint32_t i;
    uint32_t discard[ASX_SIGNAL_KIND_COUNT];
    for (i = 0; i < ASX_MAX_SIGNAL_SUBSCRIPTIONS; i++) asx_native_signal_forget(i);
    for (i = 0; i < ASX_SIGNAL_KIND_COUNT; i++) {
        if (g_installed[i]) (void)sigaction(sig_os_number((int)i), &g_saved[i], NULL);
        g_installed[i] = 0;
        g_refcount[i] = 0u;
    }
    asx_native_signal_drain(discard);
    for (i = 0; i < ASX_SIGNAL_KIND_COUNT; i++) g_overflow[i] = 0;
    g_native_shutdown = 0;
}

asx_status asx_native_signal_acquire(asx_signal_kind kind) {
    int index = asx_signal_kind_index(kind);
    struct sigaction sa;
    asx_status st;

    if (index < 0) return ASX_E_INVALID_ARGUMENT;
    st = sig_ensure_pipe();
    if (st != ASX_OK) return st;
    if (g_refcount[index] == 0u) {
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = sig_handler;
        (void)sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        if (sigaction(sig_os_number(index), &sa, &g_saved[index]) != 0) {
            return sig_errno_status(errno);
        }
        g_installed[index] = 1;
    }
    g_refcount[index]++;
    return ASX_OK;
}

void asx_native_signal_release(asx_signal_kind kind) {
    int index = asx_signal_kind_index(kind);
    if (index < 0 || g_refcount[index] == 0u) return;
    g_refcount[index]--;
    if (g_refcount[index] == 0u && g_installed[index]) {
        (void)sigaction(sig_os_number(index), &g_saved[index], NULL);
        g_installed[index] = 0;
    }
}

void asx_native_signal_drain(uint32_t counts[ASX_SIGNAL_KIND_COUNT]) {
    unsigned char buf[64];
    uint32_t i;
    ssize_t n;

    for (i = 0; i < ASX_SIGNAL_KIND_COUNT; i++) counts[i] = 0u;
    if (g_pipe_rd < 0) return;
    for (;;) {
        n = read(g_pipe_rd, buf, sizeof(buf));
        if (n > 0) {
            ssize_t k;
            for (k = 0; k < n; k++) {
                if (buf[k] < ASX_SIGNAL_KIND_COUNT) counts[buf[k]]++;
            }
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        break; /* EAGAIN: drained */
    }
    for (i = 0; i < ASX_SIGNAL_KIND_COUNT; i++) {
        if (g_overflow[i]) {
            g_overflow[i] = 0;
            if (counts[i] == 0u) counts[i] = 1u;
        }
    }
}

static asx_status sig_waiter_register(sig_waiter *w) {
    asx_status st;
    if (!asx_io_driver_is_initialized()) {
        st = asx_io_driver_init();
        if (st != ASX_OK) return st;
    }
    st = asx_io_register_fd(w->fd, &w->token);
    if (st == ASX_OK) w->registered = 1;
    return st;
}

asx_status asx_native_signal_wait(uint32_t slot) {
    sig_waiter *w;
    asx_status st;

    if (slot >= ASX_MAX_SIGNAL_SUBSCRIPTIONS) return ASX_E_INVALID_ARGUMENT;
    if (asx_task_current() == ASX_INVALID_ID || g_pipe_rd < 0) return ASX_E_PENDING;
    w = &g_waiters[slot];
    if (!w->open) {
        w->fd = fcntl(g_pipe_rd, F_DUPFD_CLOEXEC, 3);
        if (w->fd < 0) return sig_errno_status(errno);
        w->open = 1;
        w->registered = 0;
    }
    if (!w->registered) {
        st = sig_waiter_register(w);
        if (st != ASX_OK) return st;
    }
    st = asx_io_wait(&w->token, ASX_IO_READABLE);
    if (st == ASX_E_NOT_FOUND) {
        /* The IO driver was re-initialized under us: register afresh. */
        w->registered = 0;
        st = sig_waiter_register(w);
        if (st != ASX_OK) return st;
        st = asx_io_wait(&w->token, ASX_IO_READABLE);
    }
    return st;
}

void asx_native_signal_forget(uint32_t slot) {
    sig_waiter *w;
    if (slot >= ASX_MAX_SIGNAL_SUBSCRIPTIONS) return;
    w = &g_waiters[slot];
    if (!w->open) return;
    /* Deregister first: epoll keeps entries for a duplicate while the
     * pipe's other descriptors stay open. A stale token is a no-op. */
    if (w->registered) asx_io_deregister(&w->token);
    (void)close(w->fd);
    w->fd = -1;
    w->open = 0;
    w->registered = 0;
}

int asx_native_signal_shutdown_flag(void) { return g_native_shutdown != 0; }

void asx_native_signal_clear_shutdown(void) { g_native_shutdown = 0; }

#else
typedef int asx_signal_posix_empty_translation_unit;
#endif /* ASX_PROFILE_POSIX */
