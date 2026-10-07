/*
 * net_native.h — internal bridge between the net API and native sockets
 *
 * NOT part of the public API. net.c dispatches here for handles whose
 * slot carries ASX_NET_NATIVE_SLOT_BIT; the POSIX implementation lives in
 * src/platform/posix/net_posix.c and is only linked in POSIX builds.
 *
 * Native operations are non-blocking. When an operation would block it
 * arms one-shot reactor interest for the current task, parks it, and
 * returns ASX_E_PENDING, so poll functions can simply propagate PENDING.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_NET_NATIVE_H
#define ASX_NET_NATIVE_H

#include <asx/asx_status.h>
#include <asx/net/net.h>
#include <stdint.h>

/* Handles with this bit set in `slot` refer to native sockets. */
#define ASX_NET_NATIVE_SLOT_BIT 0x80000000u

static inline int asx_net_slot_is_native(uint32_t slot) {
    return (slot & ASX_NET_NATIVE_SLOT_BIT) != 0u;
}

/* Native handle: slot (with the native bit) + generation. */
typedef struct {
    uint32_t slot;
    uint32_t generation;
} asx_native_handle;

/* Reset: close every native socket (test/runtime teardown). */
void asx_native_net_reset(void);

/* TCP */
asx_status asx_native_tcp_listen(const asx_socket_addr *addr, asx_native_handle *out);
asx_status asx_native_tcp_accept(asx_native_handle listener, asx_native_handle *out,
                                 asx_socket_addr *peer);
asx_status asx_native_tcp_connect(const asx_socket_addr *addr, asx_native_handle *out);
asx_status asx_native_tcp_read(asx_native_handle stream, uint8_t *dst, uint32_t cap,
                               uint32_t *out_read);
asx_status asx_native_tcp_write(asx_native_handle stream, const uint8_t *src, uint32_t len,
                                uint32_t *out_written);
asx_status asx_native_tcp_shutdown_write(asx_native_handle stream);
asx_status asx_native_tcp_set_nodelay(asx_native_handle stream, int enabled);

/* UDP */
asx_status asx_native_udp_bind(const asx_socket_addr *addr, asx_native_handle *out);
asx_status asx_native_udp_connect(asx_native_handle sock, const asx_socket_addr *peer);
asx_status asx_native_udp_send(asx_native_handle sock, const uint8_t *src, uint32_t len,
                               const asx_socket_addr *to, uint32_t *out_written);
asx_status asx_native_udp_recv(asx_native_handle sock, uint8_t *dst, uint32_t cap,
                               uint32_t *out_read, asx_socket_addr *from);

/* Name resolution (getaddrinfo; blocking, thread safe). Fills `out` with
 * up to ASX_RESOLVE_MAX_RESULTS distinct addresses in resolver order,
 * each carrying opts->port, filtered by opts->allow_ipv4/allow_ipv6. */
asx_status asx_native_resolve(const char *host, const asx_resolve_options *opts,
                              asx_resolve_result *out);

/* Shared */
asx_status asx_native_close(asx_native_handle h);
asx_status asx_native_local_addr(asx_native_handle h, asx_socket_addr *out);
asx_status asx_native_peer_addr(asx_native_handle h, asx_socket_addr *out);
int asx_native_is_alive(asx_native_handle h);

#endif /* ASX_NET_NATIVE_H */
