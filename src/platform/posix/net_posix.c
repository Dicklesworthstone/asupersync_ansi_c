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
#define _GNU_SOURCE
#endif

#include "../../net/net_native.h"
#include <arpa/inet.h>
#include <asx/runtime/io_driver.h>
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
} native_sock;

static native_sock g_socks[ASX_NATIVE_MAX_SOCKETS];

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

static void native_free(native_sock *s) {
    uint32_t gen = s->generation;
    if (s->registered) asx_io_deregister(&s->token);
    if (s->fd >= 0) (void)close(s->fd);
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
        struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)(void *)ss;
        s6->sin6_family = AF_INET6;
        s6->sin6_port = htons(a->port);
        memcpy(&s6->sin6_addr, a->addr, 16);
        return (socklen_t)sizeof(*s6);
    } else {
        struct sockaddr_in *s4 = (struct sockaddr_in *)(void *)ss;
        s4->sin_family = AF_INET;
        s4->sin_port = htons(a->port);
        memcpy(&s4->sin_addr, a->addr, 4);
        return (socklen_t)sizeof(*s4);
    }
}

static void native_from_sockaddr(const struct sockaddr_storage *ss, asx_socket_addr *a) {
    memset(a, 0, sizeof(*a));
    if (ss->ss_family == AF_INET6) {
        const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *)(const void *)ss;
        a->family = ASX_AF_INET6;
        a->port = ntohs(s6->sin6_port);
        memcpy(a->addr, &s6->sin6_addr, 16);
    } else {
        const struct sockaddr_in *s4 = (const struct sockaddr_in *)(const void *)ss;
        a->family = ASX_AF_INET4;
        a->port = ntohs(s4->sin_port);
        memcpy(a->addr, &s4->sin_addr, 4);
    }
}

static void native_refresh_local(native_sock *s) {
    struct sockaddr_storage ss;
    socklen_t len = (socklen_t)sizeof(ss);
    if (getsockname(s->fd, (struct sockaddr *)(void *)&ss, &len) == 0) {
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

asx_status asx_native_tcp_listen(const asx_socket_addr *addr, asx_native_handle *out) {
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
    if (bind(fd, (struct sockaddr *)(void *)&ss, len) != 0 ||
        listen(fd, ASX_NATIVE_LISTEN_BACKLOG) != 0) {
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

asx_status asx_native_tcp_accept(asx_native_handle listener, asx_native_handle *out,
                                 asx_socket_addr *peer) {
    native_sock *l = native_lookup(listener, NATIVE_LISTENER);
    native_sock *s;
    struct sockaddr_storage ss;
    socklen_t len = (socklen_t)sizeof(ss);
    int fd;

    if (l == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    fd = accept(l->fd, (struct sockaddr *)(void *)&ss, &len);
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

asx_status asx_native_tcp_connect(const asx_socket_addr *addr, asx_native_handle *out) {
    struct sockaddr_storage ss;
    socklen_t len;
    native_sock *s;
    int fd;
    int rv;

    if (addr == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    fd = native_open(addr, SOCK_STREAM);
    if (fd < 0) return native_errno_status(errno);
    len = native_to_sockaddr(addr, &ss);
    rv = connect(fd, (struct sockaddr *)(void *)&ss, len);
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

asx_status asx_native_tcp_read(asx_native_handle stream, uint8_t *dst, uint32_t cap,
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

asx_status asx_native_tcp_write(asx_native_handle stream, const uint8_t *src, uint32_t len,
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

asx_status asx_native_tcp_shutdown_write(asx_native_handle stream) {
    native_sock *s = native_lookup(stream, NATIVE_STREAM);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    if (shutdown(s->fd, SHUT_WR) != 0 && errno != ENOTCONN) return native_errno_status(errno);
    return ASX_OK;
}

asx_status asx_native_tcp_set_nodelay(asx_native_handle stream, int enabled) {
    native_sock *s = native_lookup(stream, NATIVE_STREAM);
    int v = enabled ? 1 : 0;
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    if (setsockopt(s->fd, IPPROTO_TCP, TCP_NODELAY, &v, sizeof(v)) != 0) {
        return native_errno_status(errno);
    }
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* UDP                                                                 */
/* ------------------------------------------------------------------ */

asx_status asx_native_udp_bind(const asx_socket_addr *addr, asx_native_handle *out) {
    struct sockaddr_storage ss;
    socklen_t len;
    native_sock *s;
    int fd;

    if (addr == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    fd = native_open(addr, SOCK_DGRAM);
    if (fd < 0) return native_errno_status(errno);
    len = native_to_sockaddr(addr, &ss);
    if (bind(fd, (struct sockaddr *)(void *)&ss, len) != 0) {
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

asx_status asx_native_udp_connect(asx_native_handle sock, const asx_socket_addr *peer) {
    native_sock *s = native_lookup(sock, NATIVE_UDP);
    struct sockaddr_storage ss;
    socklen_t len;

    if (s == NULL || peer == NULL) return ASX_E_INVALID_ARGUMENT;
    len = native_to_sockaddr(peer, &ss);
    if (connect(s->fd, (struct sockaddr *)(void *)&ss, len) != 0) {
        return native_errno_status(errno);
    }
    s->peer = *peer;
    s->has_peer = 1;
    return ASX_OK;
}

asx_status asx_native_udp_send(asx_native_handle sock, const uint8_t *src, uint32_t len,
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
        n = sendto(s->fd, src, (size_t)len, ASX_NATIVE_SEND_FLAGS, (struct sockaddr *)(void *)&ss,
                   sl);
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

asx_status asx_native_udp_recv(asx_native_handle sock, uint8_t *dst, uint32_t cap,
                               uint32_t *out_read, asx_socket_addr *from) {
    native_sock *s = native_lookup(sock, NATIVE_UDP);
    struct sockaddr_storage ss;
    socklen_t sl = (socklen_t)sizeof(ss);
    ssize_t n;

    if (s == NULL || out_read == NULL || (dst == NULL && cap > 0u)) return ASX_E_INVALID_ARGUMENT;
    *out_read = 0;
    n = recvfrom(s->fd, dst, (size_t)cap, 0, (struct sockaddr *)(void *)&ss, &sl);
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

asx_status asx_native_resolve(const char *host, const asx_resolve_options *opts,
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

#else
typedef int asx_net_posix_empty_translation_unit;
#endif /* ASX_PROFILE_POSIX */
