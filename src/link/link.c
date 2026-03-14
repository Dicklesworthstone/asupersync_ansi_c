/*
 * link.c — public coordination link built on top of session pairs
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/link/link.h>
#include <string.h>

asx_status asx_link_open(asx_region_id region, uint32_t capacity, asx_link *out_link) {
    if (out_link == NULL) return ASX_E_INVALID_ARGUMENT;
    memset(out_link, 0, sizeof(*out_link));
    return asx_session_open(region, capacity, &out_link->session);
}

void asx_link_close(asx_link *link) {
    if (link == NULL) return;
    asx_session_close_initiator(&link->session);
    asx_session_close_responder(&link->session);
}

asx_status asx_link_send_request(asx_link *link, uint64_t value) {
    asx_status st;

    if (link == NULL) return ASX_E_INVALID_ARGUMENT;
    st = asx_session_send_request(&link->session, value);
    if (st == ASX_OK) link->requests_sent++;
    return st;
}

asx_status asx_link_recv_request(asx_link *link, uint64_t *out_value) {
    asx_status st;

    if (link == NULL) return ASX_E_INVALID_ARGUMENT;
    st = asx_session_recv_request(&link->session, out_value);
    if (st == ASX_OK) link->requests_received++;
    return st;
}

asx_status asx_link_send_response(asx_link *link, uint64_t value) {
    asx_status st;

    if (link == NULL) return ASX_E_INVALID_ARGUMENT;
    st = asx_session_send_response(&link->session, value);
    if (st == ASX_OK) link->responses_sent++;
    return st;
}

asx_status asx_link_recv_response(asx_link *link, uint64_t *out_value) {
    asx_status st;

    if (link == NULL) return ASX_E_INVALID_ARGUMENT;
    st = asx_session_recv_response(&link->session, out_value);
    if (st == ASX_OK) link->responses_received++;
    return st;
}

asx_status asx_link_capture_summary(const asx_link *link, asx_link_summary *out_summary) {
    if (link == NULL || out_summary == NULL) return ASX_E_INVALID_ARGUMENT;

    out_summary->state = asx_session_pair_state(&link->session);
    out_summary->outstanding_obligations = asx_session_pair_obligations(&link->session);
    out_summary->requests_sent = link->requests_sent;
    out_summary->requests_received = link->requests_received;
    out_summary->responses_sent = link->responses_sent;
    out_summary->responses_received = link->responses_received;
    return ASX_OK;
}

const char *asx_link_state_name(asx_session_state state) { return asx_session_state_name(state); }
