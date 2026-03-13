/*
 * ex_channel_flow.c — MPSC channel: reserve-send-recv coordination
 *
 * Demonstrates bounded MPSC channel usage:
 *   1. Creating a channel with bounded capacity within a region
 *   2. Two-phase send: reserve a permit, then send or abort
 *   3. Receiving messages in FIFO order
 *   4. Channel close and cleanup
 *
 * The two-phase send (reserve → send) prevents partial writes and
 * ensures backpressure is applied before the caller invests in
 * constructing the message.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <asx/core/channel.h>
#include <stdio.h>

#define IGNORE_RC(expr)                                                                            \
    do {                                                                                           \
        asx_status ignore_rc_ = (expr);                                                            \
        (void)ignore_rc_;                                                                          \
    } while (0)

static int g_pass = 0;
static int g_fail = 0;

#define SCENARIO_BEGIN(id)                                                                         \
    do {                                                                                           \
        const char *_scenario_id = (id);                                                           \
        int _scenario_ok = 1;                                                                      \
    (void)0

#define SCENARIO_CHECK(cond, msg)                                                                  \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("SCENARIO %s fail %s\n", _scenario_id, (msg));                                  \
            _scenario_ok = 0;                                                                      \
            g_fail++;                                                                              \
            goto _scenario_end;                                                                    \
        }                                                                                          \
    } while (0)

#define SCENARIO_END()                                                                             \
    _scenario_end:                                                                                 \
    if (_scenario_ok) {                                                                            \
        printf("SCENARIO %s pass\n", _scenario_id);                                                \
        g_pass++;                                                                                  \
    }                                                                                              \
    }                                                                                              \
    while (0)

/* -------------------------------------------------------------------
 * Scenario: channel_flow.reserve_send_recv
 *
 * Creates a channel, sends three messages using the two-phase
 * reserve/send protocol, and receives them in FIFO order.
 * ------------------------------------------------------------------- */

static void scenario_reserve_send_recv(void) {
    SCENARIO_BEGIN("channel_flow.reserve_send_recv");

    asx_runtime_reset();
    asx_channel_reset();

    asx_region_id rid;
    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");

    asx_channel_id ch;
    SCENARIO_CHECK(asx_channel_create(rid, 8, &ch) == ASX_OK, "channel_create");

    /* Send three values using reserve → send */
    uint32_t i;
    for (i = 0; i < 3; i++) {
        asx_send_permit permit;
        SCENARIO_CHECK(asx_channel_try_reserve(ch, &permit) == ASX_OK, "reserve");
        SCENARIO_CHECK(asx_send_permit_send(&permit, (uint64_t)(i + 1u) * 10u) == ASX_OK, "send");
    }

    /* Check queue length */
    uint32_t len;
    SCENARIO_CHECK(asx_channel_queue_len(ch, &len) == ASX_OK && len == 3, "len_3");

    /* Receive in FIFO order */
    uint64_t val;
    SCENARIO_CHECK(asx_channel_try_recv(ch, &val) == ASX_OK && val == 10, "recv_10");
    SCENARIO_CHECK(asx_channel_try_recv(ch, &val) == ASX_OK && val == 20, "recv_20");
    SCENARIO_CHECK(asx_channel_try_recv(ch, &val) == ASX_OK && val == 30, "recv_30");

    SCENARIO_CHECK(asx_channel_queue_len(ch, &len) == ASX_OK && len == 0, "empty_after");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario: channel_flow.reserve_abort
 *
 * Demonstrates aborting a reserved permit, which releases capacity
 * without enqueuing a message.
 * ------------------------------------------------------------------- */

static void scenario_reserve_abort(void) {
    SCENARIO_BEGIN("channel_flow.reserve_abort");

    asx_runtime_reset();
    asx_channel_reset();

    asx_region_id rid;
    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");

    asx_channel_id ch;
    SCENARIO_CHECK(asx_channel_create(rid, 4, &ch) == ASX_OK, "channel_create");

    /* Reserve then abort — channel should remain empty */
    asx_send_permit permit;
    SCENARIO_CHECK(asx_channel_try_reserve(ch, &permit) == ASX_OK, "reserve");
    asx_send_permit_abort(&permit);

    uint32_t len;
    SCENARIO_CHECK(asx_channel_queue_len(ch, &len) == ASX_OK && len == 0, "still_empty");

    /* Should still be able to reserve and send normally */
    SCENARIO_CHECK(asx_channel_try_reserve(ch, &permit) == ASX_OK, "reserve_again");
    SCENARIO_CHECK(asx_send_permit_send(&permit, 42) == ASX_OK, "send_after_abort");
    SCENARIO_CHECK(asx_channel_queue_len(ch, &len) == ASX_OK && len == 1, "one_after_send");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario: channel_flow.close_sender
 *
 * Closing the sender side signals receivers that no more messages
 * will arrive — recv returns ASX_E_DISCONNECTED on an empty
 * closed channel.
 * ------------------------------------------------------------------- */

static void scenario_close_sender(void) {
    SCENARIO_BEGIN("channel_flow.close_sender");

    asx_runtime_reset();
    asx_channel_reset();

    asx_region_id rid;
    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");

    asx_channel_id ch;
    SCENARIO_CHECK(asx_channel_create(rid, 4, &ch) == ASX_OK, "channel_create");

    /* Send one message, then close sender */
    asx_send_permit permit;
    SCENARIO_CHECK(asx_channel_try_reserve(ch, &permit) == ASX_OK, "reserve");
    SCENARIO_CHECK(asx_send_permit_send(&permit, 99) == ASX_OK, "send");
    SCENARIO_CHECK(asx_channel_close_sender(ch) == ASX_OK, "close_sender");

    /* Can still receive buffered messages */
    uint64_t val;
    SCENARIO_CHECK(asx_channel_try_recv(ch, &val) == ASX_OK && val == 99, "recv_buffered");

    /* Next recv on empty closed channel returns DISCONNECTED */
    SCENARIO_CHECK(asx_channel_try_recv(ch, &val) == ASX_E_DISCONNECTED, "recv_disconnected");

    SCENARIO_END();
}

int main(void) {
    scenario_reserve_send_recv();
    scenario_reserve_abort();
    scenario_close_sender();

    printf("SCENARIO _summary %s total=%d pass=%d fail=%d\n", g_fail > 0 ? "fail" : "pass",
           g_pass + g_fail, g_pass, g_fail);

    return g_fail > 0 ? 1 : 0;
}
