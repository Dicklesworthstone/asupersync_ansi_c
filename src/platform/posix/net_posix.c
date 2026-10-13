/*
 * posix/net_posix.c — native non-blocking TCP/UDP sockets for the net API
 *
 * Real BSD sockets behind the asx net surface. Every descriptor is
 * non-blocking and close-on-exec. When an operation would block, the
 * socket arms one-shot reactor interest for the task currently being
 * polled and parks it (asx_io_wait), so the scheduler's idle loop blocks
 * in epoll/poll and wakes exactly that task on readiness.
 *
 * Semantics (shared with the public net API):
 *   - read:  ASX_OK with n > 0 bytes; ASX_OK with 0 bytes = orderly EOF;
 *            ASX_E_PENDING = would block (task parked on readability);
 *            ASX_E_DISCONNECTED = reset / refused / broken connection.
 *   - write: ASX_OK with 1..len bytes (partial writes are normal);
 *            ASX_E_PENDING = would block; ASX_E_DISCONNECTED on EPIPE/reset.
 *   - connect: returns a stream immediately; the first read/write
 *            completes the non-blocking handshake (PENDING until then).
 *
 * SPDX-License-Identifier: MIT
 */

#ifdef ASX_PROFILE_POSIX

#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* ASX_ANALYZER_WAIVER("feature-test macro the platform headers require") */
#endif

#include "../../net/net_native.h"
#include <arpa/inet.h>
#include <asx/runtime/io_driver.h>
#include <asx/runtime/journal.h>
#include <asx/runtime/journal_replay.h>
#include <asx/runtime/runtime.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef ASX_NATIVE_MAX_SOCKETS
#define ASX_NATIVE_MAX_SOCKETS 64u
#endif

#ifndef ASX_NATIVE_LISTEN_BACKLOG
#define ASX_NATIVE_LISTEN_BACKLOG 128
#endif

#if defined(MSG_NOSIGNAL)
#define ASX_NATIVE_SEND_FLAGS MSG_NOSIGNAL
#else
#define ASX_NATIVE_SEND_FLAGS 0
#endif

typedef enum {
    NATIVE_FREE = 0,
    NATIVE_LISTENER = 1,
    NATIVE_STREAM = 2,
    NATIVE_UDP = 3
} native_kind;

typedef struct {
    int fd;
    uint32_t generation;
    native_kind kind;
    int connecting; /* non-blocking connect in flight */
    int registered; /* io token valid */
    asx_io_token token;
    asx_socket_addr local;
    asx_socket_addr peer;
    int has_peer;
    /* A replay's socket: the handle the recording named it by. A handle's
     * generation is process history, so this run's handle for the socket
     * need not equal it; requests are held to this one. */
    uint64_t jid;
} native_sock;

static native_sock g_socks[ASX_NATIVE_MAX_SOCKETS];

/* Native calls that ran their real implementation. A replay serves every
 * call from the journal and leaves this where it was. */
static uint32_t g_real_calls;

uint32_t asx_native_net_real_calls(void) { return g_real_calls; }

static native_sock *native_lookup(asx_native_handle h, native_kind kind);

/* Journal views (RP1): a handle as one u64 (the recording's name for a
 * replay's socket) and an address as its record. */
static uint64_t native_journal_id(asx_native_handle h) {
    const native_sock *s = native_lookup(h, NATIVE_FREE);
    if (s != NULL && s->jid != 0u) return s->jid;
    return ((uint64_t)h.slot << 32) | (uint64_t)h.generation;
}

static void native_journal_addr(const asx_socket_addr *a, asx_journal_addr *out) {
    memset(out, 0, sizeof(*out));
    if (a == NULL) return;
    out->family = (uint8_t)a->family;
    memcpy(out->bytes, a->addr, sizeof(out->bytes));
    out->port = a->port;
}

/* The address a record holds, as the socket API carries it. */
static void native_from_journal_addr(const asx_journal_addr *a, asx_socket_addr *out) {
    memset(out, 0, sizeof(*out));
    out->family = a->family == 0u ? ASX_AF_INET4 : ASX_AF_INET6;
    memcpy(out->addr, a->bytes, sizeof(out->addr));
    out->port = a->port;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static native_sock *native_lookup(asx_native_handle h, native_kind kind) {
    uint32_t idx;
    if (!asx_net_slot_is_native(h.slot)) return NULL;
    idx = h.slot & ~ASX_NET_NATIVE_SLOT_BIT;
    if (idx >= ASX_NATIVE_MAX_SOCKETS) return NULL;
    if (g_socks[idx].kind == NATIVE_FREE) return NULL;
    if (g_socks[idx].generation != h.generation) return NULL;
    if (kind != NATIVE_FREE && g_socks[idx].kind != kind) return NULL;
    return &g_socks[idx];
}

static native_sock *native_alloc(native_kind kind, int fd, asx_native_handle *out) {
    uint32_t idx;
    for (idx = 0; idx < ASX_NATIVE_MAX_SOCKETS; idx++) {
        if (g_socks[idx].kind == NATIVE_FREE) break;
    }
    if (idx >= ASX_NATIVE_MAX_SOCKETS) return NULL;
    {
        native_sock *s = &g_socks[idx];
        uint32_t gen = s->generation + 1u;
        if (gen == 0u) gen = 1u;
        memset(s, 0, sizeof(*s));
        s->generation = gen;
        s->kind = kind;
        s->fd = fd;
        out->slot = idx | ASX_NET_NATIVE_SLOT_BIT;
        out->generation = gen;
        return s;
    }
}

/* A socket a replay serves: nothing is opened, and the IO driver is given
 * a descriptor number from the virtual range to register in its place. */
static native_sock *native_alloc_virtual(native_kind kind, asx_native_handle *out) {
    native_sock *s = native_alloc(kind, -1, out);
    if (s != NULL) s->fd = ASX_REPLAY_VIRTUAL_FD_BASE + (int)(s - g_socks);
    return s;
}

static void native_free(native_sock *s) {
    uint32_t gen = s->generation;
    if (s->registered) asx_io_deregister(&s->token);
    if (s->fd >= 0 && !asx_replay_fd_is_virtual(s->fd)) {
        g_real_calls++;
        (void)close(s->fd);
    }
    memset(s, 0, sizeof(*s));
    s->generation = gen;
    s->fd = -1;
}

static int native_set_flags(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) return -1;
    fl = fcntl(fd, F_GETFD, 0);
    if (fl < 0 || fcntl(fd, F_SETFD, fl | FD_CLOEXEC) < 0) return -1;
#if defined(SO_NOSIGPIPE)
    {
        int one = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    }
#endif
    return 0;
}

static socklen_t native_to_sockaddr(const asx_socket_addr *a, struct sockaddr_storage *ss) {
    memset(ss, 0, sizeof(*ss));
    if (a->family == ASX_AF_INET6) {
        struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)ss;
        s6->sin6_family = AF_INET6;
        s6->sin6_port = htons(a->port);
        memcpy(&s6->sin6_addr, a->addr, 16);
        return (socklen_t)sizeof(*s6);
    } else {
        struct sockaddr_in *s4 = (struct sockaddr_in *)ss;
        s4->sin_family = AF_INET;
        s4->sin_port = htons(a->port);
        memcpy(&s4->sin_addr, a->addr, 4);
        return (socklen_t)sizeof(*s4);
    }
}

static void native_from_sockaddr(const struct sockaddr_storage *ss, asx_socket_addr *a) {
    memset(a, 0, sizeof(*a));
    if (ss->ss_family == AF_INET6) {
        const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *)ss;
        a->family = ASX_AF_INET6;
        a->port = ntohs(s6->sin6_port);
        memcpy(a->addr, &s6->sin6_addr, 16);
    } else {
        const struct sockaddr_in *s4 = (const struct sockaddr_in *)ss;
        a->family = ASX_AF_INET4;
        a->port = ntohs(s4->sin_port);
        memcpy(a->addr, &s4->sin_addr, 4);
    }
}

static void native_refresh_local(native_sock *s) {
    struct sockaddr_storage ss;
    socklen_t len = (socklen_t)sizeof(ss);
    memset(&ss, 0, sizeof(ss));
    if (getsockname(s->fd, (struct sockaddr *)&ss, &len) == 0) {
        native_from_sockaddr(&ss, &s->local);
    }
}

static asx_status native_errno_status(int err) {
    switch (err) {
    case EAGAIN:
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
#endif
    case EINPROGRESS: return ASX_E_PENDING;
    case EADDRINUSE: return ASX_E_ALREADY_EXISTS;
    case EACCES:
    case EPERM: return ASX_E_PERMISSION_DENIED;
    case EMFILE:
    case ENFILE:
    case ENOBUFS:
    case ENOMEM: return ASX_E_RESOURCE_EXHAUSTED;
    case EINVAL:
    case EBADF:
    case EAFNOSUPPORT:
    case EADDRNOTAVAIL: return ASX_E_INVALID_ARGUMENT;
    case ETIMEDOUT: return ASX_E_TIMED_OUT;
    default: return ASX_E_DISCONNECTED;
    }
}

/* The IO driver must be live for readiness-driven parking. */
static asx_status native_ensure_io(native_sock *s) {
    asx_status st;
    if (s->registered) return ASX_OK;
    if (!asx_io_driver_is_initialized()) {
        st = asx_io_driver_init();
        if (st != ASX_OK) return st;
    }
    st = asx_io_register_fd(s->fd, &s->token);
    if (st != ASX_OK) return st;
    s->registered = 1;
    return ASX_OK;
}

/* Arm readiness for the current task and park it. Always PENDING unless
 * arming fails. */
static asx_status native_wait(native_sock *s, asx_io_interest interest) {
    asx_status st = native_ensure_io(s);
    if (st != ASX_OK) return st;
    return asx_io_wait(&s->token, interest);
}

static int native_open(const asx_socket_addr *addr, int type) {
    int family = (addr->family == ASX_AF_INET6) ? AF_INET6 : AF_INET;
    int fd = socket(family, type, 0);
    if (fd < 0) return -1;
    if (native_set_flags(fd) != 0) {
        (void)close(fd);
        return -1;
    }
    return fd;
}

/* Complete a non-blocking connect. ASX_OK once connected. */
static asx_status native_finish_connect(native_sock *s) {
    struct pollfd pfd;
    int err = 0;
    socklen_t len = (socklen_t)sizeof(err);

    if (!s->connecting) return ASX_OK;
    pfd.fd = s->fd;
    pfd.events = POLLOUT;
    pfd.revents = 0;
    if (poll(&pfd, 1, 0) <= 0) return native_wait(s, ASX_IO_WRITABLE);
    if (getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) err = errno;
    if (err != 0)
        return err == EINPROGRESS ? native_wait(s, ASX_IO_WRITABLE) : native_errno_status(err);
    s->connecting = 0;
    native_refresh_local(s);
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Replay                                                              */
/* ------------------------------------------------------------------ */

/* The result a replay serves for a call that was PENDING in the recording:
 * the task parks on the socket's virtual descriptor as it did, and the
 * recorded reactor poll returns its token. `recorded` is returned when the
 * socket is gone (the program no longer matches the recording, which the
 * next request reports). */
static asx_status native_replay_wait(asx_native_handle h, native_kind kind,
                                     asx_io_interest interest, asx_status recorded) {
    native_sock *s = native_lookup(h, kind);
    if (s == NULL) return recorded;
    return native_wait(s, interest);
}

/* A bind or listen served from the journal: the handle and the address
 * the recording got, on a socket that has no descriptor. */
static asx_status native_replay_bind(native_kind kind, const asx_journal_addr *want,
                                     asx_native_handle *out) {
    asx_journal_addr local;
    int32_t status = 0;
    uint64_t handle = 0u;
    native_sock *s;

    if (kind == NATIVE_UDP) {
        (void)asx_replay_serve_udp_bind(want, &status, &handle, &local);
    } else {
        (void)asx_replay_serve_net_listen(want, &status, &handle, &local);
    }
    if (status != (int32_t)ASX_OK) return (asx_status)status;
    s = native_alloc_virtual(kind, out);
    if (s == NULL) {
        (void)asx_replay_check_handle(handle, 0u, "socket table full where the recording had room");
        return ASX_E_REPLAY_MISMATCH;
    }
    native_from_journal_addr(&local, &s->local);
    s->jid = handle;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Reset                                                               */
/* ------------------------------------------------------------------ */

void asx_native_net_reset(void) {
    uint32_t i;
    for (i = 0; i < ASX_NATIVE_MAX_SOCKETS; i++) {
        if (g_socks[i].kind != NATIVE_FREE) {
            /* The IO driver is reset separately; just close the fd. */
            g_socks[i].registered = 0;
            native_free(&g_socks[i]);
        }
    }
}

/* ------------------------------------------------------------------ */
/* TCP                                                                 */
/* ------------------------------------------------------------------ */

static asx_status native_tcp_listen_impl(const asx_socket_addr *addr, asx_native_handle *out) {
    struct sockaddr_storage ss;
    socklen_t len;
    native_sock *s;
    int fd;
    int one = 1;

    if (addr == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    fd = native_open(addr, SOCK_STREAM);
    if (fd < 0) return native_errno_status(errno);
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    len = native_to_sockaddr(addr, &ss);
    if (bind(fd, (struct sockaddr *)&ss, len) != 0 || listen(fd, ASX_NATIVE_LISTEN_BACKLOG) != 0) {
        asx_status st = native_errno_status(errno);
        (void)close(fd);
        return st;
    }
    s = native_alloc(NATIVE_LISTENER, fd, out);
    if (s == NULL) {
        (void)close(fd);
        return ASX_E_RESOURCE_EXHAUSTED;
    }
    native_refresh_local(s);
    return ASX_OK;
}

asx_status asx_native_tcp_listen(const asx_socket_addr *addr, asx_native_handle *out) {
    asx_journal_addr want;
    asx_journal_addr local;
    native_sock *s;
    asx_status st;

    if (addr == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    native_journal_addr(addr, &want);
    if (asx_replay_active()) return native_replay_bind(NATIVE_LISTENER, &want, out);
    g_real_calls++;
    st = native_tcp_listen_impl(addr, out);
    s = st == ASX_OK ? native_lookup(*out, NATIVE_LISTENER) : NULL;
    native_journal_addr(s != NULL ? &s->local : NULL, &local);
    asx_journal_note_net_listen(&want, (int32_t)st, st == ASX_OK ? native_journal_id(*out) : 0u,
                                &local);
    return st;
}

static asx_status native_tcp_accept_impl(asx_native_handle listener, asx_native_handle *out,
                                         asx_socket_addr *peer) {
    native_sock *l = native_lookup(listener, NATIVE_LISTENER);
    native_sock *s;
    struct sockaddr_storage ss;
    socklen_t len = (socklen_t)sizeof(ss);
    int fd;

    if (l == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    /* accept() fills the peer address only up to the length it reports; the
     * rest of ss must not be indeterminate when native_from_sockaddr reads it. */
    memset(&ss, 0, sizeof(ss));
    fd = accept(l->fd, (struct sockaddr *)&ss, &len);
    if (fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == ECONNABORTED) {
            return native_wait(l, ASX_IO_READABLE);
        }
        return native_errno_status(errno);
    }
    if (native_set_flags(fd) != 0) {
        (void)close(fd);
        return ASX_E_INVALID_STATE;
    }
    s = native_alloc(NATIVE_STREAM, fd, out);
    if (s == NULL) {
        (void)close(fd);
        return ASX_E_RESOURCE_EXHAUSTED;
    }
    native_from_sockaddr(&ss, &s->peer);
    s->has_peer = 1;
    native_refresh_local(s);
    if (peer != NULL) *peer = s->peer;
    return ASX_OK;
}

static asx_status native_replay_accept(asx_native_handle listener, asx_native_handle *out,
                                       asx_socket_addr *peer) {
    asx_journal_addr jpeer;
    asx_journal_addr jlocal;
    int32_t status = 0;
    uint64_t stream = 0u;
    native_sock *s;

    (void)asx_replay_serve_net_accept(native_journal_id(listener), &status, &stream, &jpeer,
                                      &jlocal);
    if (status == (int32_t)ASX_E_PENDING) {
        return native_replay_wait(listener, NATIVE_LISTENER, ASX_IO_READABLE, ASX_E_PENDING);
    }
    if (status != (int32_t)ASX_OK) return (asx_status)status;
    s = native_alloc_virtual(NATIVE_STREAM, out);
    if (s == NULL) {
        (void)asx_replay_check_handle(stream, 0u, "socket table full where the recording had room");
        return ASX_E_REPLAY_MISMATCH;
    }
    native_from_journal_addr(&jpeer, &s->peer);
    s->has_peer = 1;
    native_from_journal_addr(&jlocal, &s->local);
    s->jid = stream;
    if (peer != NULL) *peer = s->peer;
    return ASX_OK;
}

asx_status asx_native_tcp_accept(asx_native_handle listener, asx_native_handle *out,
                                 asx_socket_addr *peer) {
    asx_socket_addr got;
    asx_journal_addr rec_peer;
    asx_journal_addr rec_local;
    native_sock *s;
    asx_status st;

    if (asx_replay_active()) {
        if (out == NULL) return ASX_E_INVALID_ARGUMENT;
        return native_replay_accept(listener, out, peer);
    }
    g_real_calls++;
    memset(&got, 0, sizeof(got));
    st = native_tcp_accept_impl(listener, out, &got);
    if (peer != NULL && st == ASX_OK) *peer = got;
    if (out != NULL) {
        s = st == ASX_OK ? native_lookup(*out, NATIVE_STREAM) : NULL;
        native_journal_addr(&got, &rec_peer);
        native_journal_addr(s != NULL ? &s->local : NULL, &rec_local);
        asx_journal_note_net_accept(native_journal_id(listener), (int32_t)st,
                                    st == ASX_OK ? native_journal_id(*out) : 0u, &rec_peer,
                                    &rec_local);
    }
    return st;
}

static asx_status native_tcp_connect_impl(const asx_socket_addr *addr, asx_native_handle *out) {
    struct sockaddr_storage ss;
    socklen_t len;
    native_sock *s;
    int fd;
    int rv;

    if (addr == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    fd = native_open(addr, SOCK_STREAM);
    if (fd < 0) return native_errno_status(errno);
    len = native_to_sockaddr(addr, &ss);
    rv = connect(fd, (struct sockaddr *)&ss, len);
    if (rv != 0 && errno != EINPROGRESS && errno != EINTR) {
        asx_status st = native_errno_status(errno);
        (void)close(fd);
        return st == ASX_E_PENDING ? ASX_E_DISCONNECTED : st;
    }
    s = native_alloc(NATIVE_STREAM, fd, out);
    if (s == NULL) {
        (void)close(fd);
        return ASX_E_RESOURCE_EXHAUSTED;
    }
    s->peer = *addr;
    s->has_peer = 1;
    s->connecting = (rv != 0);
    native_refresh_local(s);
    return ASX_OK;
}

static asx_status native_replay_connect(const asx_journal_addr *to, const asx_socket_addr *addr,
                                        asx_native_handle *out) {
    asx_journal_addr jlocal;
    int32_t status = 0;
    uint64_t stream = 0u;
    native_sock *s;

    (void)asx_replay_serve_net_connect(to, &status, &stream, &jlocal);
    if (status != (int32_t)ASX_OK) return (asx_status)status;
    s = native_alloc_virtual(NATIVE_STREAM, out);
    if (s == NULL) {
        (void)asx_replay_check_handle(stream, 0u, "socket table full where the recording had room");
        return ASX_E_REPLAY_MISMATCH;
    }
    s->peer = *addr;
    s->has_peer = 1;
    native_from_journal_addr(&jlocal, &s->local);
    s->jid = stream;
    return ASX_OK;
}

asx_status asx_native_tcp_connect(const asx_socket_addr *addr, asx_native_handle *out) {
    asx_journal_addr rec;
    asx_journal_addr rec_local;
    native_sock *s;
    asx_status st;

    if (addr == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    native_journal_addr(addr, &rec);
    if (asx_replay_active()) return native_replay_connect(&rec, addr, out);
    g_real_calls++;
    st = native_tcp_connect_impl(addr, out);
    s = st == ASX_OK ? native_lookup(*out, NATIVE_STREAM) : NULL;
    native_journal_addr(s != NULL ? &s->local : NULL, &rec_local);
    asx_journal_note_net_connect(&rec, (int32_t)st, st == ASX_OK ? native_journal_id(*out) : 0u,
                                 &rec_local);
    return st;
}

static asx_status native_tcp_read_impl(asx_native_handle stream, uint8_t *dst, uint32_t cap,
                                       uint32_t *out_read) {
    native_sock *s = native_lookup(stream, NATIVE_STREAM);
    asx_status st;
    ssize_t n;

    if (s == NULL || out_read == NULL || (dst == NULL && cap > 0u)) return ASX_E_INVALID_ARGUMENT;
    *out_read = 0;
    if (cap == 0u) return ASX_E_BUFFER_TOO_SMALL;
    st = native_finish_connect(s);
    if (st != ASX_OK) return st;

    n = recv(s->fd, dst, (size_t)cap, 0);
    if (n > 0) {
        *out_read = (uint32_t)n;
        return ASX_OK;
    }
    if (n == 0) return ASX_OK; /* orderly EOF */
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return native_wait(s, ASX_IO_READABLE);
    }
    return native_errno_status(errno);
}

asx_status asx_native_tcp_read(asx_native_handle stream, uint8_t *dst, uint32_t cap,
                               uint32_t *out_read) {
    asx_status st;
    if (asx_replay_active()) {
        int32_t status = 0;
        uint32_t n = 0u;
        if (out_read == NULL) return ASX_E_INVALID_ARGUMENT;
        (void)asx_replay_serve_net_read(native_journal_id(stream), cap, &status, dst, &n);
        *out_read = n;
        st = (asx_status)status;
        if (st == ASX_E_PENDING) {
            st = native_replay_wait(stream, NATIVE_STREAM, ASX_IO_READABLE, st);
        }
        return st;
    }
    g_real_calls++;
    st = native_tcp_read_impl(stream, dst, cap, out_read);
    if (out_read != NULL) {
        asx_journal_note_net_read(native_journal_id(stream), cap, (int32_t)st, dst, *out_read);
    }
    return st;
}

static asx_status native_tcp_write_impl(asx_native_handle stream, const uint8_t *src, uint32_t len,
                                        uint32_t *out_written) {
    native_sock *s = native_lookup(stream, NATIVE_STREAM);
    asx_status st;
    ssize_t n;

    if (s == NULL || out_written == NULL || (src == NULL && len > 0u)) {
        return ASX_E_INVALID_ARGUMENT;
    }
    *out_written = 0;
    st = native_finish_connect(s);
    if (st != ASX_OK) return st;
    if (len == 0u) return ASX_OK;

    n = send(s->fd, src, (size_t)len, ASX_NATIVE_SEND_FLAGS);
    if (n >= 0) {
        *out_written = (uint32_t)n;
        return ASX_OK;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return native_wait(s, ASX_IO_WRITABLE);
    }
    return native_errno_status(errno);
}

asx_status asx_native_tcp_write(asx_native_handle stream, const uint8_t *src, uint32_t len,
                                uint32_t *out_written) {
    asx_status st;
    if (asx_replay_active()) {
        int32_t status = 0;
        uint32_t written = 0u;
        if (out_written == NULL) return ASX_E_INVALID_ARGUMENT;
        (void)asx_replay_serve_net_write(native_journal_id(stream), len, &status, &written);
        *out_written = written;
        st = (asx_status)status;
        if (st == ASX_E_PENDING) {
            st = native_replay_wait(stream, NATIVE_STREAM, ASX_IO_WRITABLE, st);
        }
        return st;
    }
    g_real_calls++;
    st = native_tcp_write_impl(stream, src, len, out_written);
    if (out_written != NULL) {
        asx_journal_note_net_write(native_journal_id(stream), len, (int32_t)st, *out_written);
    }
    return st;
}

/* A socket control call: the status the recording got, served from the
 * journal in a replay. */
static asx_status native_ctl_status(uint32_t op, asx_native_handle h, asx_status real) {
    asx_journal_note_net_ctl((uint8_t)op, native_journal_id(h), (int32_t)real);
    return real;
}

static asx_status native_replay_ctl(uint32_t op, asx_native_handle h) {
    int32_t status = 0;
    (void)asx_replay_serve_net_ctl((uint8_t)op, native_journal_id(h), &status);
    return (asx_status)status;
}

asx_status asx_native_tcp_shutdown_write(asx_native_handle stream) {
    native_sock *s;
    asx_status st = ASX_OK;
    if (asx_replay_active()) {
        return native_replay_ctl(ASX_JOURNAL_NET_CTL_SHUTDOWN_WRITE, stream);
    }
    g_real_calls++;
    s = native_lookup(stream, NATIVE_STREAM);
    if (s == NULL) {
        st = ASX_E_INVALID_ARGUMENT;
    } else if (shutdown(s->fd, SHUT_WR) != 0 && errno != ENOTCONN) {
        st = native_errno_status(errno);
    }
    return native_ctl_status(ASX_JOURNAL_NET_CTL_SHUTDOWN_WRITE, stream, st);
}

asx_status asx_native_tcp_set_nodelay(asx_native_handle stream, int enabled) {
    native_sock *s;
    int v = enabled ? 1 : 0;
    asx_status st = ASX_OK;
    if (asx_replay_active()) return native_replay_ctl(ASX_JOURNAL_NET_CTL_NODELAY, stream);
    g_real_calls++;
    s = native_lookup(stream, NATIVE_STREAM);
    if (s == NULL) {
        st = ASX_E_INVALID_ARGUMENT;
    } else if (setsockopt(s->fd, IPPROTO_TCP, TCP_NODELAY, &v, sizeof(v)) != 0) {
        st = native_errno_status(errno);
    }
    return native_ctl_status(ASX_JOURNAL_NET_CTL_NODELAY, stream, st);
}

/* ------------------------------------------------------------------ */
/* UDP                                                                 */
/* ------------------------------------------------------------------ */

static asx_status native_udp_bind_impl(const asx_socket_addr *addr, asx_native_handle *out) {
    struct sockaddr_storage ss;
    socklen_t len;
    native_sock *s;
    int fd;

    if (addr == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    fd = native_open(addr, SOCK_DGRAM);
    if (fd < 0) return native_errno_status(errno);
    len = native_to_sockaddr(addr, &ss);
    if (bind(fd, (struct sockaddr *)&ss, len) != 0) {
        asx_status st = native_errno_status(errno);
        (void)close(fd);
        return st;
    }
    s = native_alloc(NATIVE_UDP, fd, out);
    if (s == NULL) {
        (void)close(fd);
        return ASX_E_RESOURCE_EXHAUSTED;
    }
    native_refresh_local(s);
    return ASX_OK;
}

asx_status asx_native_udp_bind(const asx_socket_addr *addr, asx_native_handle *out) {
    asx_journal_addr want;
    asx_journal_addr local;
    native_sock *s;
    asx_status st;

    if (addr == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    native_journal_addr(addr, &want);
    if (asx_replay_active()) return native_replay_bind(NATIVE_UDP, &want, out);
    g_real_calls++;
    st = native_udp_bind_impl(addr, out);
    s = st == ASX_OK ? native_lookup(*out, NATIVE_UDP) : NULL;
    native_journal_addr(s != NULL ? &s->local : NULL, &local);
    asx_journal_note_udp_bind(&want, (int32_t)st, st == ASX_OK ? native_journal_id(*out) : 0u,
                              &local);
    return st;
}

asx_status asx_native_udp_connect(asx_native_handle sock, const asx_socket_addr *peer) {
    native_sock *s;
    struct sockaddr_storage ss;
    socklen_t len;
    asx_status st = ASX_OK;

    if (asx_replay_active()) {
        st = native_replay_ctl(ASX_JOURNAL_NET_CTL_UDP_CONNECT, sock);
        s = native_lookup(sock, NATIVE_UDP);
        if (st == ASX_OK && s != NULL && peer != NULL) {
            s->peer = *peer;
            s->has_peer = 1;
        }
        return st;
    }
    g_real_calls++;
    s = native_lookup(sock, NATIVE_UDP);
    if (s == NULL || peer == NULL) {
        st = ASX_E_INVALID_ARGUMENT;
    } else {
        len = native_to_sockaddr(peer, &ss);
        if (connect(s->fd, (struct sockaddr *)&ss, len) != 0) {
            st = native_errno_status(errno);
        } else {
            s->peer = *peer;
            s->has_peer = 1;
        }
    }
    return native_ctl_status(ASX_JOURNAL_NET_CTL_UDP_CONNECT, sock, st);
}

static asx_status native_udp_send_impl(asx_native_handle sock, const uint8_t *src, uint32_t len,
                                       const asx_socket_addr *to, uint32_t *out_written) {
    native_sock *s = native_lookup(sock, NATIVE_UDP);
    ssize_t n;

    if (s == NULL || out_written == NULL || (src == NULL && len > 0u)) {
        return ASX_E_INVALID_ARGUMENT;
    }
    *out_written = 0;
    if (to != NULL) {
        struct sockaddr_storage ss;
        socklen_t sl = native_to_sockaddr(to, &ss);
        n = sendto(s->fd, src, (size_t)len, ASX_NATIVE_SEND_FLAGS, (struct sockaddr *)&ss, sl);
    } else {
        if (!s->has_peer) return ASX_E_INVALID_STATE;
        n = send(s->fd, src, (size_t)len, ASX_NATIVE_SEND_FLAGS);
    }
    if (n >= 0) {
        *out_written = (uint32_t)n;
        return ASX_OK;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return native_wait(s, ASX_IO_WRITABLE);
    }
    if (errno == EMSGSIZE) return ASX_E_BUFFER_TOO_SMALL;
    return native_errno_status(errno);
}

asx_status asx_native_udp_send(asx_native_handle sock, const uint8_t *src, uint32_t len,
                               const asx_socket_addr *to, uint32_t *out_written) {
    asx_status st;
    if (asx_replay_active()) {
        int32_t status = 0;
        uint32_t written = 0u;
        if (out_written == NULL) return ASX_E_INVALID_ARGUMENT;
        (void)asx_replay_serve_udp_send(native_journal_id(sock), len, &status, &written);
        *out_written = written;
        st = (asx_status)status;
        if (st == ASX_E_PENDING) st = native_replay_wait(sock, NATIVE_UDP, ASX_IO_WRITABLE, st);
        return st;
    }
    g_real_calls++;
    st = native_udp_send_impl(sock, src, len, to, out_written);
    if (out_written != NULL) {
        asx_journal_note_udp_send(native_journal_id(sock), len, (int32_t)st, *out_written);
    }
    return st;
}

static asx_status native_udp_recv_impl(asx_native_handle sock, uint8_t *dst, uint32_t cap,
                                       uint32_t *out_read, asx_socket_addr *from) {
    native_sock *s = native_lookup(sock, NATIVE_UDP);
    struct sockaddr_storage ss;
    socklen_t sl = (socklen_t)sizeof(ss);
    ssize_t n;

    if (s == NULL || out_read == NULL || (dst == NULL && cap > 0u)) return ASX_E_INVALID_ARGUMENT;
    *out_read = 0;
    /* recvfrom() fills the source address only up to the length it reports. */
    memset(&ss, 0, sizeof(ss));
    n = recvfrom(s->fd, dst, (size_t)cap, 0, (struct sockaddr *)&ss, &sl);
    if (n >= 0) {
        *out_read = (uint32_t)n;
        if (from != NULL) native_from_sockaddr(&ss, from);
        return ASX_OK;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return native_wait(s, ASX_IO_READABLE);
    }
    return native_errno_status(errno);
}

asx_status asx_native_udp_recv(asx_native_handle sock, uint8_t *dst, uint32_t cap,
                               uint32_t *out_read, asx_socket_addr *from) {
    asx_socket_addr got;
    asx_journal_addr rec;
    asx_status st;
    if (asx_replay_active()) {
        int32_t status = 0;
        uint32_t n = 0u;
        asx_journal_addr jfrom;
        if (out_read == NULL) return ASX_E_INVALID_ARGUMENT;
        (void)asx_replay_serve_udp_recv(native_journal_id(sock), cap, &status, dst, &n, &jfrom);
        *out_read = n;
        st = (asx_status)status;
        if (st == ASX_OK && from != NULL) native_from_journal_addr(&jfrom, from);
        if (st == ASX_E_PENDING) st = native_replay_wait(sock, NATIVE_UDP, ASX_IO_READABLE, st);
        return st;
    }
    g_real_calls++;
    memset(&got, 0, sizeof(got));
    st = native_udp_recv_impl(sock, dst, cap, out_read, &got);
    if (from != NULL && st == ASX_OK) *from = got;
    if (out_read != NULL) {
        native_journal_addr(&got, &rec);
        asx_journal_note_udp_recv(native_journal_id(sock), cap, (int32_t)st, dst, *out_read, &rec);
    }
    return st;
}

/* ------------------------------------------------------------------ */
/* Shared                                                              */
/* ------------------------------------------------------------------ */

asx_status asx_native_close(asx_native_handle h) {
    native_sock *s = native_lookup(h, NATIVE_FREE);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    native_free(s);
    return ASX_OK;
}

asx_status asx_native_local_addr(asx_native_handle h, asx_socket_addr *out) {
    native_sock *s = native_lookup(h, NATIVE_FREE);
    if (s == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    *out = s->local;
    return ASX_OK;
}

asx_status asx_native_peer_addr(asx_native_handle h, asx_socket_addr *out) {
    native_sock *s = native_lookup(h, NATIVE_FREE);
    if (s == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!s->has_peer) return ASX_E_INVALID_STATE;
    *out = s->peer;
    return ASX_OK;
}

int asx_native_is_alive(asx_native_handle h) { return native_lookup(h, NATIVE_FREE) != NULL; }

/* ------------------------------------------------------------------ */
/* Name resolution                                                     */
/* ------------------------------------------------------------------ */

static asx_status native_resolve_impl(const char *host, const asx_resolve_options *opts,
                                      asx_resolve_result *out) {
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *ai;
    int rc;

    if (host == NULL || opts == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (!opts->allow_ipv4 && !opts->allow_ipv6) return ASX_E_INVALID_ARGUMENT;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = opts->allow_ipv4 && opts->allow_ipv6 ? AF_UNSPEC
                      : opts->allow_ipv6                   ? AF_INET6
                                                           : AF_INET;
    hints.ai_socktype = SOCK_STREAM; /* one entry per address */
    rc = getaddrinfo(host, NULL, &hints, &res);
    if (rc != 0) {
        if (rc == EAI_AGAIN) return ASX_E_TIMED_OUT;
        if (rc == EAI_MEMORY) return ASX_E_RESOURCE_EXHAUSTED;
        return ASX_E_NOT_FOUND;
    }

    for (ai = res; ai != NULL && out->count < ASX_RESOLVE_MAX_RESULTS; ai = ai->ai_next) {
        struct sockaddr_storage ss;
        asx_socket_addr a;
        uint32_t i;
        int dup = 0;

        if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6) continue;
        if (ai->ai_addr == NULL || ai->ai_addrlen > (socklen_t)sizeof(ss)) continue;
        memset(&ss, 0, sizeof(ss));
        memcpy(&ss, ai->ai_addr, (size_t)ai->ai_addrlen);
        native_from_sockaddr(&ss, &a);
        a.port = opts->port;
        for (i = 0; i < out->count; i++) {
            if (asx_socket_addr_eq(&out->addrs[i], &a)) dup = 1;
        }
        if (!dup) out->addrs[out->count++] = a;
    }
    freeaddrinfo(res);
    return out->count > 0u ? ASX_OK : ASX_E_NOT_FOUND;
}

/* `job` is nonzero for a lookup a blocking job makes: it is noted as made
 * off the runtime thread so a replay can run that job again. */
static asx_status native_resolve_common(const char *host, const asx_resolve_options *opts,
                                        asx_resolve_result *out, int job) {
    asx_journal_addr recs[ASX_RESOLVE_MAX_RESULTS];
    asx_status st;
    uint32_t i;
    uint32_t n = 0u;
    if (out != NULL && host != NULL && opts != NULL && asx_replay_active()) {
        /* A replay answers from the journal; the host must be the one
         * that was resolved when it was recorded. */
        int32_t recorded = 0;
        if (asx_replay_serve_resolve(asx_journal_text_hash(host), &recorded, recs,
                                     (uint32_t)ASX_RESOLVE_MAX_RESULTS, &n)) {
            memset(out, 0, sizeof(*out));
            for (i = 0; i < n; i++) {
                ASX_CHECKPOINT_WAIVER("bounded: at most ASX_RESOLVE_MAX_RESULTS addresses");
                out->addrs[i].family = recs[i].family == 0u ? ASX_AF_INET4 : ASX_AF_INET6;
                memcpy(out->addrs[i].addr, recs[i].bytes, sizeof(out->addrs[i].addr));
                out->addrs[i].port = recs[i].port;
            }
            out->count = n;
            return (asx_status)recorded;
        }
    }
    g_real_calls++;
    st = native_resolve_impl(host, opts, out);
    if (out != NULL && host != NULL && opts != NULL) {
        n = out->count < ASX_RESOLVE_MAX_RESULTS ? out->count : (uint32_t)ASX_RESOLVE_MAX_RESULTS;
        for (i = 0; i < n; i++) {
            ASX_CHECKPOINT_WAIVER("bounded: at most ASX_RESOLVE_MAX_RESULTS addresses");
            native_journal_addr(&out->addrs[i], &recs[i]);
        }
        if (job) {
            asx_journal_note_resolve_job(asx_journal_text_hash(host), (int32_t)st, recs, n);
        } else {
            asx_journal_note_resolve(asx_journal_text_hash(host), (int32_t)st, recs, n);
        }
    }
    return st;
}

asx_status asx_native_resolve(const char *host, const asx_resolve_options *opts,
                              asx_resolve_result *out) {
    return native_resolve_common(host, opts, out, 0);
}

asx_status asx_native_resolve_job(const char *host, const asx_resolve_options *opts,
                                  asx_resolve_result *out) {
    return native_resolve_common(host, opts, out, 1);
}

#else
typedef int asx_net_posix_empty_translation_unit;
#endif /* ASX_PROFILE_POSIX */
