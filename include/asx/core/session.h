/*
 * asx/core/session.h — bidirectional session channel with obligation tracking
 *
 * A session is a pair of in-memory per-direction queues providing bidirectional
 * request/response communication between two peers. Each session
 * maintains an obligation count tracking outstanding requests.
 *
 * Semantics:
 *   - Two endpoints: initiator and responder
 *   - Each endpoint can send to the other and receive from the other
 *   - Request/response tracking via obligation counter
 *   - Session becomes half-closed when one peer drops and fully closed when both drop
 *
 * Walking skeleton: single-threaded, non-blocking, fixed arena.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_CORE_SESSION_H
#define ASX_CORE_SESSION_H

#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>
#include <asx/core/channel.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ASX_MAX_SESSIONS 8u
#define ASX_SESSION_MAX_CAPACITY 16u

/* -------------------------------------------------------------------
 * Session state
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_SESSION_OPEN = 0,
    ASX_SESSION_HALF_CLOSED = 1, /* one peer dropped */
    ASX_SESSION_CLOSED = 2       /* both peers dropped or drained */
} asx_session_state;

/* -------------------------------------------------------------------
 * Endpoint handle
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t slot;
    uint16_t generation;
    int is_initiator; /* 1 = initiator, 0 = responder */
} asx_session_endpoint;

/* -------------------------------------------------------------------
 * API: Lifecycle
 * ------------------------------------------------------------------- */

/* Create a session, returning initiator and responder endpoints.
 * capacity is the per-direction channel capacity.
 * region is the owning region for the underlying channels.
 * Returns ASX_OK on success.
 * Returns ASX_E_INVALID_ARGUMENT for malformed handles or capacities.
 * Returns ASX_E_INVALID_STATE if the region is not OPEN. */
ASX_API ASX_MUST_USE asx_status asx_session_create(asx_region_id region, uint32_t capacity,
                                                   asx_session_endpoint *out_initiator,
                                                   asx_session_endpoint *out_responder);

/* Drop an endpoint. If the peer is still alive, session goes to
 * HALF_CLOSED. If both dropped, session goes to CLOSED. */
ASX_API void asx_session_endpoint_drop(asx_session_endpoint *ep);

/* -------------------------------------------------------------------
 * API: Send / Receive
 * ------------------------------------------------------------------- */

/* Send a request (initiator→responder) or response (responder→initiator).
 * Increments the outstanding obligation count when sending a request.
 * Returns ASX_OK on success.
 * Returns ASX_E_CHANNEL_FULL if the direction's buffer is full.
 * Returns ASX_E_DISCONNECTED if peer dropped. */
ASX_API ASX_MUST_USE asx_status asx_session_send(asx_session_endpoint *ep, uint64_t value);

/* Try to receive from the peer's send direction.
 * Decrements obligation count when the initiator receives a response,
 * retiring an outstanding request/response obligation.
 * Returns ASX_OK and fills *out_value on success.
 * Returns ASX_E_WOULD_BLOCK if no messages.
 * Returns ASX_E_DISCONNECTED if peer dropped and no messages remain. */
ASX_API ASX_MUST_USE asx_status asx_session_try_recv(asx_session_endpoint *ep, uint64_t *out_value);

/* -------------------------------------------------------------------
 * API: Query
 * ------------------------------------------------------------------- */

/* Get the session state. */
ASX_API asx_session_state asx_session_get_state(uint32_t slot, uint16_t gen);

/* Get the number of outstanding obligations (unresponded requests). */
ASX_API uint32_t asx_session_obligations(uint32_t slot, uint16_t gen);

/* Check if the peer endpoint is still alive. */
ASX_API int asx_session_peer_alive(const asx_session_endpoint *ep);

/* -------------------------------------------------------------------
 * Reset (test support)
 * ------------------------------------------------------------------- */

ASX_API void asx_session_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_CORE_SESSION_H */
