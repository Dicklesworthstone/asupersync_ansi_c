/*
 * asx/session/session.h — public session helpers
 *
 * Promotes the existing bidirectional session channel into a dedicated
 * public family with named peer roles and consistent helper entry points.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_SESSION_SESSION_H
#define ASX_SESSION_SESSION_H

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <asx/core/session.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    asx_session_endpoint initiator;
    asx_session_endpoint responder;
} asx_session_pair;

static inline ASX_MUST_USE asx_status asx_session_open(asx_region_id region, uint32_t capacity,
                                                       asx_session_pair *out_pair) {
    if (out_pair == NULL) return ASX_E_INVALID_ARGUMENT;
    return asx_session_create(region, capacity, &out_pair->initiator, &out_pair->responder);
}

static inline void asx_session_close_initiator(asx_session_pair *pair) {
    if (pair != NULL) asx_session_endpoint_drop(&pair->initiator);
}

static inline void asx_session_close_responder(asx_session_pair *pair) {
    if (pair != NULL) asx_session_endpoint_drop(&pair->responder);
}

static inline ASX_MUST_USE asx_status asx_session_send_request(asx_session_pair *pair,
                                                               uint64_t value) {
    if (pair == NULL) return ASX_E_INVALID_ARGUMENT;
    return asx_session_send(&pair->initiator, value);
}

static inline ASX_MUST_USE asx_status asx_session_recv_request(asx_session_pair *pair,
                                                               uint64_t *out_value) {
    if (pair == NULL) return ASX_E_INVALID_ARGUMENT;
    return asx_session_try_recv(&pair->responder, out_value);
}

static inline ASX_MUST_USE asx_status asx_session_send_response(asx_session_pair *pair,
                                                                uint64_t value) {
    if (pair == NULL) return ASX_E_INVALID_ARGUMENT;
    return asx_session_send(&pair->responder, value);
}

static inline ASX_MUST_USE asx_status asx_session_recv_response(asx_session_pair *pair,
                                                                uint64_t *out_value) {
    if (pair == NULL) return ASX_E_INVALID_ARGUMENT;
    return asx_session_try_recv(&pair->initiator, out_value);
}

static inline asx_session_state asx_session_pair_state(const asx_session_pair *pair) {
    if (pair == NULL) return ASX_SESSION_CLOSED;
    return asx_session_get_state(pair->initiator.slot, pair->initiator.generation);
}

static inline uint32_t asx_session_pair_obligations(const asx_session_pair *pair) {
    if (pair == NULL) return 0u;
    return asx_session_obligations(pair->initiator.slot, pair->initiator.generation);
}

static inline const char *asx_session_state_name(asx_session_state state) {
    switch (state) {
    case ASX_SESSION_OPEN: return "open";
    case ASX_SESSION_HALF_CLOSED: return "half_closed";
    case ASX_SESSION_CLOSED: return "closed";
    default: return "unknown";
    }
}

#ifdef __cplusplus
}
#endif

#endif /* ASX_SESSION_SESSION_H */
