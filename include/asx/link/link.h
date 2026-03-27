/*
 * asx/link/link.h — public long-lived coordination link
 *
 * A link packages a session pair plus per-direction counters into a
 * reusable coordination primitive for request/response style flows.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_LINK_LINK_H
#define ASX_LINK_LINK_H

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <asx/session/session.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    asx_session_pair session;
    uint32_t requests_sent;
    uint32_t requests_received;
    uint32_t responses_sent;
    uint32_t responses_received;
} asx_link;

typedef struct {
    asx_session_state state;
    uint32_t outstanding_obligations;
    uint32_t requests_sent;
    uint32_t requests_received;
    uint32_t responses_sent;
    uint32_t responses_received;
} asx_link_summary;

/* Open a coordination link backed by a session pair in the given region. */
ASX_API ASX_MUST_USE asx_status asx_link_open(asx_region_id region, uint32_t capacity,
                                              asx_link *out_link);
/* Close a link, dropping both session endpoints. */
ASX_API void asx_link_close(asx_link *link);
/* Send a request value through the link (initiator to responder). */
ASX_API ASX_MUST_USE asx_status asx_link_send_request(asx_link *link, uint64_t value);
/* Receive a pending request from the link. */
ASX_API ASX_MUST_USE asx_status asx_link_recv_request(asx_link *link, uint64_t *out_value);
/* Send a response value through the link (responder to initiator). */
ASX_API ASX_MUST_USE asx_status asx_link_send_response(asx_link *link, uint64_t value);
/* Receive a pending response from the link. */
ASX_API ASX_MUST_USE asx_status asx_link_recv_response(asx_link *link, uint64_t *out_value);
/* Capture a summary snapshot of the link's current state and counters. */
ASX_API ASX_MUST_USE asx_status asx_link_capture_summary(const asx_link *link,
                                                         asx_link_summary *out_summary);
/* Return the human-readable name for a session state. */
ASX_API const char *asx_link_state_name(asx_session_state state);

#ifdef __cplusplus
}
#endif

#endif /* ASX_LINK_LINK_H */
