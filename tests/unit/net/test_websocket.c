/*
 * test_websocket.c — RFC 6455 codec, handshake, engine, and TCP e2e tests
 *
 * Covers:
 *   - RFC 6455 §5.7 framing examples (unmasked/masked "Hello", fragmented
 *     "Hel"+"lo", ping/pong, 256-byte and 64 KiB binary length encodings)
 *   - header decode strictness, header validation rules, masking, UTF-8
 *   - opening handshake (RFC 6455 §1.3 accept, request/response validation,
 *     subprotocol negotiation, rejection responses)
 *   - engine protocol errors mapped to close codes 1002 / 1007 / 1009
 *   - close handshake, backpressure, and an end-to-end client <-> server
 *     exchange over the in-memory TCP loopback
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/asx_config.h>
#include <asx/net/websocket.h>
#include <asx/runtime/runtime.h>
#include <string.h>

static const uint8_t k_rfc_mask[4] = {0x37, 0xfa, 0x21, 0x3d};
static const uint8_t k_hello[5] = {0x48, 0x65, 0x6c, 0x6c, 0x6f};

static asx_ws_engine g_srv;
static asx_ws_engine g_cli;
static uint8_t g_big[65536u + 16u];

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    const uint8_t *bytes;
    uint32_t len;
    uint32_t pos;
    uint32_t wrap_to; /* index to restart from once exhausted */
} fixed_rng;

static asx_status fixed_random(void *ctx, uint8_t *out, uint32_t len) {
    fixed_rng *r = (fixed_rng *)ctx;
    uint32_t i;
    for (i = 0u; i < len; i++) {
        if (r->pos >= r->len) r->pos = r->wrap_to;
        out[i] = r->bytes[r->pos++];
    }
    return ASX_OK;
}

static asx_status failing_random(void *ctx, uint8_t *out, uint32_t len) {
    (void)ctx;
    (void)out;
    (void)len;
    return ASX_E_HOOK_MISSING;
}

static void install_entropy(void) {
    asx_runtime_hooks hooks;
    asx_runtime_reset();
    (void)asx_runtime_hooks_init(&hooks);
    (void)asx_runtime_set_hooks(&hooks);
}

static asx_status init_open(asx_ws_engine *ws, asx_ws_role role, uint32_t max_message,
                            fixed_rng *rng) {
    asx_ws_config cfg;
    asx_ws_config_init(&cfg, role);
    cfg.skip_handshake = 1;
    cfg.max_message = max_message;
    if (rng != NULL) {
        cfg.random_fn = fixed_random;
        cfg.random_ctx = rng;
    }
    return asx_ws_engine_init(ws, &cfg);
}

/* Encode a client->server (masked with the RFC key) frame. */
static uint32_t masked_frame(asx_ws_opcode op, int fin, const void *payload, uint32_t len,
                             uint8_t *out, uint32_t cap) {
    uint32_t n = 0u;
    if (asx_ws_frame_encode(op, fin, k_rfc_mask, (const uint8_t *)payload, len, out, cap, &n) !=
        ASX_OK) {
        return 0u;
    }
    return n;
}

static int output_equals(const asx_ws_engine *ws, const uint8_t *expect, uint32_t n) {
    const uint8_t *p = NULL;
    uint32_t len = asx_ws_engine_output(ws, &p);
    return len == n && (n == 0u || memcmp(p, expect, n) == 0);
}

static int output_starts_with(const asx_ws_engine *ws, const char *prefix) {
    const uint8_t *p = NULL;
    uint32_t len = asx_ws_engine_output(ws, &p);
    size_t n = strlen(prefix);
    return len >= n && memcmp(p, prefix, n) == 0;
}

static int output_contains(const asx_ws_engine *ws, const char *needle) {
    const uint8_t *p = NULL;
    uint32_t len = asx_ws_engine_output(ws, &p);
    size_t n = strlen(needle);
    uint32_t i;
    for (i = 0u; p != NULL && i + n <= len; i++) {
        if (memcmp(p + i, needle, n) == 0) return 1;
    }
    return 0;
}

static void clear_output(asx_ws_engine *ws) {
    uint32_t n = asx_ws_engine_output(ws, NULL);
    asx_status st = asx_ws_engine_consume_output(ws, n);
    (void)st;
}

/* Feed all bytes; returns the final status. */
static asx_status feed_all(asx_ws_engine *ws, const uint8_t *data, uint32_t len) {
    uint32_t used = 0u;
    return asx_ws_engine_feed(ws, data, len, &used);
}

static void pump(asx_ws_conn a, asx_ws_conn b, unsigned rounds) {
    unsigned i;
    for (i = 0u; i < rounds; i++) {
        asx_status sa = asx_ws_poll(a);
        asx_status sb = asx_ws_poll(b);
        (void)sa;
        (void)sb;
    }
}

/* ================================================================== */
/* RFC 6455 §5.7 framing examples                                      */
/* ================================================================== */

TEST(rfc_single_frame_unmasked_text) {
    static const uint8_t expect[] = {0x81, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f};
    uint8_t out[16];
    uint32_t n = 0u;
    asx_ws_frame_header h;

    ASSERT_EQ(asx_ws_frame_encode(ASX_WS_OPCODE_TEXT, 1, NULL, k_hello, 5u, out, sizeof(out), &n),
              ASX_OK);
    ASSERT_EQ(n, sizeof(expect));
    ASSERT_TRUE(memcmp(out, expect, n) == 0);

    ASSERT_EQ(asx_ws_header_decode(expect, sizeof(expect), &h), ASX_OK);
    ASSERT_EQ(h.fin, 1u);
    ASSERT_EQ(h.opcode, ASX_WS_OPCODE_TEXT);
    ASSERT_EQ(h.masked, 0u);
    ASSERT_EQ(h.payload_len, 5u);
    ASSERT_EQ(h.header_len, 2u);
}

TEST(rfc_single_frame_masked_text) {
    static const uint8_t expect[] = {0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d,
                                     0x7f, 0x9f, 0x4d, 0x51, 0x58};
    uint8_t out[16];
    uint8_t payload[5];
    uint32_t n = 0u;
    asx_ws_frame_header h;

    ASSERT_EQ(asx_ws_frame_encode(ASX_WS_OPCODE_TEXT, 1, k_rfc_mask, k_hello, 5u, out, sizeof(out),
                                  &n),
              ASX_OK);
    ASSERT_EQ(n, sizeof(expect));
    ASSERT_TRUE(memcmp(out, expect, n) == 0);

    ASSERT_EQ(asx_ws_header_decode(expect, sizeof(expect), &h), ASX_OK);
    ASSERT_EQ(h.masked, 1u);
    ASSERT_EQ(h.header_len, 6u);
    ASSERT_TRUE(memcmp(h.mask_key, k_rfc_mask, 4u) == 0);
    memcpy(payload, expect + h.header_len, 5u);
    asx_ws_mask(payload, 5u, h.mask_key, 0u);
    ASSERT_TRUE(memcmp(payload, "Hello", 5u) == 0);
}

TEST(rfc_fragmented_unmasked_text) {
    static const uint8_t first[] = {0x01, 0x03, 0x48, 0x65, 0x6c};
    static const uint8_t second[] = {0x80, 0x02, 0x6c, 0x6f};
    uint8_t out[16];
    uint32_t n = 0u;

    ASSERT_EQ(asx_ws_frame_encode(ASX_WS_OPCODE_TEXT, 0, NULL, k_hello, 3u, out, sizeof(out), &n),
              ASX_OK);
    ASSERT_EQ(n, sizeof(first));
    ASSERT_TRUE(memcmp(out, first, n) == 0);
    ASSERT_EQ(asx_ws_frame_encode(ASX_WS_OPCODE_CONTINUATION, 1, NULL, k_hello + 3, 2u, out,
                                  sizeof(out), &n),
              ASX_OK);
    ASSERT_EQ(n, sizeof(second));
    ASSERT_TRUE(memcmp(out, second, n) == 0);
}

TEST(rfc_ping_and_masked_pong) {
    static const uint8_t ping[] = {0x89, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f};
    static const uint8_t pong[] = {0x8a, 0x85, 0x37, 0xfa, 0x21, 0x3d,
                                   0x7f, 0x9f, 0x4d, 0x51, 0x58};
    uint8_t out[16];
    uint32_t n = 0u;

    ASSERT_EQ(asx_ws_frame_encode(ASX_WS_OPCODE_PING, 1, NULL, k_hello, 5u, out, sizeof(out), &n),
              ASX_OK);
    ASSERT_TRUE(n == sizeof(ping) && memcmp(out, ping, n) == 0);
    ASSERT_EQ(asx_ws_frame_encode(ASX_WS_OPCODE_PONG, 1, k_rfc_mask, k_hello, 5u, out, sizeof(out),
                                  &n),
              ASX_OK);
    ASSERT_TRUE(n == sizeof(pong) && memcmp(out, pong, n) == 0);
}

TEST(rfc_256_byte_binary_uses_16_bit_length) {
    static uint8_t payload[256];
    static uint8_t out[300];
    uint32_t n = 0u;
    asx_ws_frame_header h;

    memset(payload, 0xAB, sizeof(payload));
    ASSERT_EQ(asx_ws_frame_encode(ASX_WS_OPCODE_BINARY, 1, NULL, payload, 256u, out, sizeof(out),
                                  &n),
              ASX_OK);
    ASSERT_EQ(n, 260u);
    ASSERT_EQ(out[0], 0x82);
    ASSERT_EQ(out[1], 0x7E);
    ASSERT_EQ(out[2], 0x01);
    ASSERT_EQ(out[3], 0x00);
    ASSERT_EQ(asx_ws_header_decode(out, n, &h), ASX_OK);
    ASSERT_EQ(h.payload_len, 256u);
    ASSERT_EQ(h.header_len, 4u);
}

TEST(rfc_64kib_binary_uses_64_bit_length) {
    static const uint8_t expect_hdr[] = {0x82, 0x7F, 0x00, 0x00, 0x00,
                                         0x00, 0x00, 0x01, 0x00, 0x00};
    static uint8_t payload[65536];
    uint32_t n = 0u;
    asx_ws_frame_header h;

    memset(payload, 0x5A, sizeof(payload));
    ASSERT_EQ(asx_ws_frame_encode(ASX_WS_OPCODE_BINARY, 1, NULL, payload, 65536u, g_big,
                                  sizeof(g_big), &n),
              ASX_OK);
    ASSERT_EQ(n, 65546u);
    ASSERT_TRUE(memcmp(g_big, expect_hdr, sizeof(expect_hdr)) == 0);
    ASSERT_EQ(asx_ws_header_decode(g_big, n, &h), ASX_OK);
    ASSERT_EQ(h.payload_len, 65536u);
    ASSERT_EQ(h.header_len, 10u);
    /* 65535 still fits the 16-bit form */
    h.payload_len = 65535u;
    h.masked = 0u;
    ASSERT_EQ(asx_ws_header_encode(&h, g_big, 16u, &n), ASX_OK);
    ASSERT_EQ(n, 4u);
    ASSERT_EQ(g_big[1], 0x7E);
    /* buffer too small for the payload */
    ASSERT_EQ(asx_ws_frame_encode(ASX_WS_OPCODE_BINARY, 1, NULL, payload, 65536u, g_big, 1000u, &n),
              ASX_E_BUFFER_TOO_SMALL);
}

/* ================================================================== */
/* Codec strictness                                                    */
/* ================================================================== */

TEST(header_decode_reports_pending_for_partial_headers) {
    static const uint8_t frame[] = {0x82, 0xFF, 0, 0, 0, 0, 0, 1, 0, 0, 1, 2, 3, 4};
    asx_ws_frame_header h;
    uint32_t i;

    for (i = 0u; i < sizeof(frame); i++) {
        ASSERT_EQ(asx_ws_header_decode(frame, i, &h), ASX_E_PENDING);
    }
    ASSERT_EQ(asx_ws_header_decode(frame, sizeof(frame), &h), ASX_OK);
    ASSERT_EQ(h.header_len, 14u);
    ASSERT_EQ(h.masked, 1u);
}

TEST(header_decode_rejects_non_minimal_and_msb_lengths) {
    static const uint8_t nonmin16[] = {0x82, 0x7E, 0x00, 0x05};
    static const uint8_t nonmin64[] = {0x82, 0x7F, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
    static const uint8_t msb64[] = {0x82, 0x7F, 0x80, 0, 0, 0, 0, 0, 0, 0};
    asx_ws_frame_header h;

    ASSERT_EQ(asx_ws_header_decode(nonmin16, sizeof(nonmin16), &h), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_ws_header_decode(nonmin64, sizeof(nonmin64), &h), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_ws_header_decode(msb64, sizeof(msb64), &h), ASX_E_INVALID_ARGUMENT);
}

TEST(header_check_rules) {
    asx_ws_frame_header h;

    memset(&h, 0, sizeof(h));
    h.fin = 1u;
    h.opcode = ASX_WS_OPCODE_TEXT;
    h.masked = 1u;
    ASSERT_EQ(asx_ws_header_check(&h, ASX_WS_ROLE_SERVER), 0u);
    ASSERT_EQ(asx_ws_header_check(&h, ASX_WS_ROLE_CLIENT), ASX_WS_CLOSE_PROTOCOL_ERROR);
    h.masked = 0u;
    ASSERT_EQ(asx_ws_header_check(&h, ASX_WS_ROLE_CLIENT), 0u);
    ASSERT_EQ(asx_ws_header_check(&h, ASX_WS_ROLE_SERVER), ASX_WS_CLOSE_PROTOCOL_ERROR);

    h.rsv = 4u; /* RSV1 */
    ASSERT_EQ(asx_ws_header_check(&h, ASX_WS_ROLE_CLIENT), ASX_WS_CLOSE_PROTOCOL_ERROR);
    h.rsv = 0u;
    h.opcode = 0x3u; /* reserved data opcode */
    ASSERT_EQ(asx_ws_header_check(&h, ASX_WS_ROLE_CLIENT), ASX_WS_CLOSE_PROTOCOL_ERROR);
    h.opcode = 0xBu; /* reserved control opcode */
    ASSERT_EQ(asx_ws_header_check(&h, ASX_WS_ROLE_CLIENT), ASX_WS_CLOSE_PROTOCOL_ERROR);
    h.opcode = ASX_WS_OPCODE_PING;
    h.fin = 0u; /* fragmented control frame */
    ASSERT_EQ(asx_ws_header_check(&h, ASX_WS_ROLE_CLIENT), ASX_WS_CLOSE_PROTOCOL_ERROR);
    h.fin = 1u;
    h.payload_len = 126u; /* oversized control frame */
    ASSERT_EQ(asx_ws_header_check(&h, ASX_WS_ROLE_CLIENT), ASX_WS_CLOSE_PROTOCOL_ERROR);
    h.payload_len = 125u;
    ASSERT_EQ(asx_ws_header_check(&h, ASX_WS_ROLE_CLIENT), 0u);
}

TEST(frame_encode_rejects_bad_control_frames) {
    uint8_t payload[126];
    uint8_t out[200];
    uint32_t n = 0u;

    memset(payload, 0, sizeof(payload));
    ASSERT_EQ(asx_ws_frame_encode(ASX_WS_OPCODE_PING, 1, NULL, payload, 126u, out, sizeof(out), &n),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_ws_frame_encode(ASX_WS_OPCODE_PING, 0, NULL, payload, 1u, out, sizeof(out), &n),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_ws_frame_encode((asx_ws_opcode)3, 1, NULL, payload, 1u, out, sizeof(out), &n),
              ASX_E_INVALID_ARGUMENT);
}

TEST(mask_is_chunk_offset_consistent) {
    uint8_t whole[37];
    uint8_t parts[37];
    unsigned i;

    for (i = 0u; i < sizeof(whole); i++) whole[i] = (uint8_t)(i * 13u);
    memcpy(parts, whole, sizeof(parts));
    asx_ws_mask(whole, sizeof(whole), k_rfc_mask, 0u);
    asx_ws_mask(parts, 5u, k_rfc_mask, 0u);
    asx_ws_mask(parts + 5, 11u, k_rfc_mask, 5u);
    asx_ws_mask(parts + 16, 21u, k_rfc_mask, 16u);
    ASSERT_TRUE(memcmp(whole, parts, sizeof(whole)) == 0);
    asx_ws_mask(whole, sizeof(whole), k_rfc_mask, 0u); /* involution */
    for (i = 0u; i < sizeof(whole); i++) ASSERT_EQ(whole[i], (uint8_t)(i * 13u));
}

TEST(utf8_validation_strict) {
    static const uint8_t greek[] = {0xCE, 0xBA, 0xE1, 0xBD, 0xB9, 0xCF,
                                    0x83, 0xCE, 0xBC, 0xCE, 0xB5};
    static const uint8_t emoji[] = {0xF0, 0x9F, 0x98, 0x80};
    static const uint8_t max_cp[] = {0xF4, 0x8F, 0xBF, 0xBF};
    static const uint8_t overlong2[] = {0xC0, 0x80};
    static const uint8_t overlong3[] = {0xE0, 0x80, 0x80};
    static const uint8_t surrogate[] = {0xED, 0xA0, 0x80};
    static const uint8_t too_big[] = {0xF4, 0x90, 0x80, 0x80};
    static const uint8_t lone_cont[] = {0x80};
    static const uint8_t truncated[] = {0xE2, 0x82};
    static const uint8_t f5[] = {0xF5, 0x80, 0x80, 0x80};
    asx_ws_utf8 v;

    ASSERT_TRUE(asx_ws_utf8_valid((const uint8_t *)"Hello", 5u));
    ASSERT_TRUE(asx_ws_utf8_valid(greek, sizeof(greek)));
    ASSERT_TRUE(asx_ws_utf8_valid(emoji, sizeof(emoji)));
    ASSERT_TRUE(asx_ws_utf8_valid(max_cp, sizeof(max_cp)));
    ASSERT_TRUE(asx_ws_utf8_valid(NULL, 0u));
    ASSERT_FALSE(asx_ws_utf8_valid(overlong2, sizeof(overlong2)));
    ASSERT_FALSE(asx_ws_utf8_valid(overlong3, sizeof(overlong3)));
    ASSERT_FALSE(asx_ws_utf8_valid(surrogate, sizeof(surrogate)));
    ASSERT_FALSE(asx_ws_utf8_valid(too_big, sizeof(too_big)));
    ASSERT_FALSE(asx_ws_utf8_valid(lone_cont, sizeof(lone_cont)));
    ASSERT_FALSE(asx_ws_utf8_valid(truncated, sizeof(truncated)));
    ASSERT_FALSE(asx_ws_utf8_valid(f5, sizeof(f5)));

    /* A code point split across feeds stays valid. */
    asx_ws_utf8_init(&v);
    ASSERT_TRUE(asx_ws_utf8_feed(&v, emoji, 1u));
    ASSERT_FALSE(asx_ws_utf8_complete(&v));
    ASSERT_TRUE(asx_ws_utf8_feed(&v, emoji + 1, 3u));
    ASSERT_TRUE(asx_ws_utf8_complete(&v));
}

TEST(close_code_ranges) {
    ASSERT_TRUE(asx_ws_close_code_sendable(1000u));
    ASSERT_TRUE(asx_ws_close_code_sendable(1011u));
    ASSERT_TRUE(asx_ws_close_code_sendable(4999u));
    ASSERT_FALSE(asx_ws_close_code_sendable(999u));
    ASSERT_FALSE(asx_ws_close_code_sendable(1005u));
    ASSERT_FALSE(asx_ws_close_code_sendable(1006u));
    ASSERT_FALSE(asx_ws_close_code_sendable(1015u));
    ASSERT_FALSE(asx_ws_close_code_sendable(2000u));
    ASSERT_FALSE(asx_ws_close_code_sendable(5000u));
    ASSERT_TRUE(asx_ws_close_code_receivable(2000u));
    ASSERT_FALSE(asx_ws_close_code_receivable(1004u));
    ASSERT_FALSE(asx_ws_close_code_receivable(1005u));
    ASSERT_FALSE(asx_ws_close_code_receivable(1015u));
}

/* ================================================================== */
/* Opening handshake                                                   */
/* ================================================================== */

static const char k_rfc_request[] = "GET /chat HTTP/1.1\r\n"
                                    "Host: server.example.com\r\n"
                                    "Upgrade: websocket\r\n"
                                    "Connection: Upgrade\r\n"
                                    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                    "Origin: http://example.com\r\n"
                                    "Sec-WebSocket-Protocol: chat, superchat\r\n"
                                    "Sec-WebSocket-Version: 13\r\n"
                                    "\r\n";

static const char k_rfc_response[] = "HTTP/1.1 101 Switching Protocols\r\n"
                                     "Upgrade: websocket\r\n"
                                     "Connection: Upgrade\r\n"
                                     "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
                                     "Sec-WebSocket-Protocol: chat\r\n"
                                     "\r\n";

TEST(handshake_accept_rfc_example) {
    char accept[ASX_WS_ACCEPT_LEN + 1];
    ASSERT_EQ(asx_ws_compute_accept("dGhlIHNhbXBsZSBub25jZQ==", 24u, accept), ASX_OK);
    ASSERT_STR_EQ(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST(handshake_build_request) {
    char out[512];
    size_t n = 0u;

    ASSERT_EQ(asx_ws_handshake_build_request("server.example.com", "/chat", "http://example.com",
                                             "chat, superchat", "dGhlIHNhbXBsZSBub25jZQ==", out,
                                             sizeof(out), &n),
              ASX_OK);
    ASSERT_EQ(n, strlen(out));
    ASSERT_TRUE(strncmp(out, "GET /chat HTTP/1.1\r\nHost: server.example.com\r\n", 46) == 0);
    ASSERT_TRUE(strstr(out, "\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n") != NULL);
    ASSERT_TRUE(strstr(out, "\r\nSec-WebSocket-Version: 13\r\n") != NULL);
    ASSERT_TRUE(strstr(out, "\r\nSec-WebSocket-Protocol: chat, superchat\r\n") != NULL);
    ASSERT_TRUE(strstr(out, "\r\nOrigin: http://example.com\r\n") != NULL);
    ASSERT_TRUE(n >= 4u && memcmp(out + n - 4u, "\r\n\r\n", 4u) == 0);

    /* header injection, bad key, missing host, small buffer */
    ASSERT_EQ(asx_ws_handshake_build_request("evil\r\nX: y", "/", NULL, NULL,
                                             "dGhlIHNhbXBsZSBub25jZQ==", out, sizeof(out), &n),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_ws_handshake_build_request("h", "/", NULL, NULL, "short", out, sizeof(out), &n),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_ws_handshake_build_request(NULL, "/", NULL, NULL, "dGhlIHNhbXBsZSBub25jZQ==", out,
                                             sizeof(out), &n),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_ws_handshake_build_request("h", "/", NULL, NULL, "dGhlIHNhbXBsZSBub25jZQ==", out,
                                             32u, &n),
              ASX_E_BUFFER_TOO_SMALL);
}

TEST(server_engine_answers_rfc_request) {
    asx_ws_config cfg;
    uint32_t used = 0u;

    asx_ws_config_init(&cfg, ASX_WS_ROLE_SERVER);
    cfg.protocols = "superchat, chat";
    ASSERT_EQ(asx_ws_engine_init(&g_srv, &cfg), ASX_OK);
    ASSERT_EQ(asx_ws_engine_state(&g_srv), ASX_WS_STATE_CONNECTING);

    /* deliver the request in two pieces */
    ASSERT_EQ(asx_ws_engine_feed(&g_srv, (const uint8_t *)k_rfc_request, 40u, &used), ASX_OK);
    ASSERT_EQ(used, 40u);
    ASSERT_EQ(asx_ws_engine_state(&g_srv), ASX_WS_STATE_CONNECTING);
    ASSERT_EQ(asx_ws_engine_feed(&g_srv, (const uint8_t *)k_rfc_request + 40,
                                 (uint32_t)(sizeof(k_rfc_request) - 1u - 40u), &used),
              ASX_OK);
    ASSERT_EQ(asx_ws_engine_state(&g_srv), ASX_WS_STATE_OPEN);
    /* client preference order wins: "chat" was offered first */
    ASSERT_STR_EQ(asx_ws_engine_protocol(&g_srv), "chat");
    ASSERT_TRUE(output_equals(&g_srv, (const uint8_t *)k_rfc_response,
                              (uint32_t)(sizeof(k_rfc_response) - 1u)));
}

static uint16_t server_reject_with(const char *request, const char *expect_status) {
    asx_ws_config cfg;
    asx_status st;

    asx_ws_config_init(&cfg, ASX_WS_ROLE_SERVER);
    if (asx_ws_engine_init(&g_srv, &cfg) != ASX_OK) return 1u;
    st = feed_all(&g_srv, (const uint8_t *)request, (uint32_t)strlen(request));
    if (st != ASX_E_DISCONNECTED) return 2u;
    if (asx_ws_engine_state(&g_srv) != ASX_WS_STATE_CLOSED) return 3u;
    if (!output_starts_with(&g_srv, expect_status)) return 4u;
    return asx_ws_engine_error_code(&g_srv);
}

TEST(server_engine_rejects_invalid_requests) {
    ASSERT_EQ(server_reject_with("GET / HTTP/1.1\r\nHost: h\r\nConnection: Upgrade\r\n"
                                 "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                 "Sec-WebSocket-Version: 13\r\n\r\n",
                                 "HTTP/1.1 400"),
              ASX_WS_CLOSE_PROTOCOL_ERROR); /* missing Upgrade */
    ASSERT_EQ(server_reject_with("GET / HTTP/1.1\r\nHost: h\r\nUpgrade: websocket\r\n"
                                 "Connection: Upgrade\r\n"
                                 "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                 "Sec-WebSocket-Version: 8\r\n\r\n",
                                 "HTTP/1.1 426"),
              ASX_WS_CLOSE_PROTOCOL_ERROR); /* unsupported version */
    ASSERT_TRUE(output_contains(&g_srv, "Sec-WebSocket-Version: 13\r\n"));
    ASSERT_EQ(server_reject_with("GET / HTTP/1.1\r\nHost: h\r\nUpgrade: websocket\r\n"
                                 "Connection: Upgrade\r\nSec-WebSocket-Key: c2hvcnQ=\r\n"
                                 "Sec-WebSocket-Version: 13\r\n\r\n",
                                 "HTTP/1.1 400"),
              ASX_WS_CLOSE_PROTOCOL_ERROR); /* key is not 16 bytes */
    ASSERT_EQ(server_reject_with("POST / HTTP/1.1\r\nHost: h\r\nUpgrade: websocket\r\n"
                                 "Connection: Upgrade\r\n"
                                 "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                 "Sec-WebSocket-Version: 13\r\n\r\n",
                                 "HTTP/1.1 400"),
              ASX_WS_CLOSE_PROTOCOL_ERROR); /* wrong method */
    ASSERT_EQ(server_reject_with("GET / HTTP/1.1\r\nUpgrade: websocket\r\n"
                                 "Connection: Upgrade\r\n"
                                 "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                 "Sec-WebSocket-Version: 13\r\n\r\n",
                                 "HTTP/1.1 400"),
              ASX_WS_CLOSE_PROTOCOL_ERROR); /* missing Host */
    ASSERT_EQ(server_reject_with("GET / HTTP/1.1\r\nHost: h\r\n folded\r\nUpgrade: websocket\r\n"
                                 "Connection: Upgrade\r\n"
                                 "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                 "Sec-WebSocket-Version: 13\r\n\r\n",
                                 "HTTP/1.1 400"),
              ASX_WS_CLOSE_PROTOCOL_ERROR); /* obsolete line folding */
}

TEST(client_engine_handshake_and_trailing_frame) {
    static const uint8_t nonce_then_mask[] = {'t', 'h', 'e', ' ', 's', 'a', 'm',  'p',  'l',  'e',
                                              ' ', 'n', 'o', 'n', 'c', 'e', 0x37, 0xfa, 0x21, 0x3d};
    static const uint8_t trailing[] = {0x81, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f};
    uint8_t wire[512];
    uint8_t msg[16];
    fixed_rng rng = {nonce_then_mask, sizeof(nonce_then_mask), 0u, 16u};
    asx_ws_config cfg;
    asx_ws_opcode op = ASX_WS_OPCODE_BINARY;
    uint32_t len = 0u;
    uint32_t used = 0u;
    size_t rlen = sizeof(k_rfc_response) - 1u;

    asx_ws_config_init(&cfg, ASX_WS_ROLE_CLIENT);
    cfg.host = "server.example.com";
    cfg.path = "/chat";
    cfg.origin = "http://example.com";
    cfg.protocols = "chat, superchat";
    cfg.random_fn = fixed_random;
    cfg.random_ctx = &rng;
    ASSERT_EQ(asx_ws_engine_init(&g_cli, &cfg), ASX_OK);
    ASSERT_TRUE(output_starts_with(&g_cli, "GET /chat HTTP/1.1\r\n"));
    ASSERT_TRUE(output_contains(&g_cli, "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"));
    clear_output(&g_cli);

    /* The 101 response and the first frame arrive in one read. */
    memcpy(wire, k_rfc_response, rlen);
    memcpy(wire + rlen, trailing, sizeof(trailing));
    ASSERT_EQ(asx_ws_engine_feed(&g_cli, wire, (uint32_t)(rlen + sizeof(trailing)), &used), ASX_OK);
    ASSERT_EQ(used, (uint32_t)(rlen + sizeof(trailing)));
    ASSERT_EQ(asx_ws_engine_state(&g_cli), ASX_WS_STATE_OPEN);
    ASSERT_STR_EQ(asx_ws_engine_protocol(&g_cli), "chat");
    ASSERT_EQ(asx_ws_engine_recv(&g_cli, &op, msg, sizeof(msg), &len), ASX_OK);
    ASSERT_EQ(op, ASX_WS_OPCODE_TEXT);
    ASSERT_TRUE(len == 5u && memcmp(msg, "Hello", 5u) == 0);

    /* Client frames are masked with keys from the random source. */
    ASSERT_EQ(asx_ws_engine_send(&g_cli, ASX_WS_OPCODE_TEXT, k_hello, 5u), ASX_OK);
    {
        static const uint8_t expect[] = {0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d,
                                         0x7f, 0x9f, 0x4d, 0x51, 0x58};
        ASSERT_TRUE(output_equals(&g_cli, expect, sizeof(expect)));
    }
}

static uint16_t client_reject_with(const char *response) {
    static const uint8_t nonce[] = "the sample nonce";
    fixed_rng rng = {nonce, 16u, 0u, 0u};
    asx_ws_config cfg;

    asx_ws_config_init(&cfg, ASX_WS_ROLE_CLIENT);
    cfg.host = "server.example.com";
    cfg.protocols = "chat";
    cfg.random_fn = fixed_random;
    cfg.random_ctx = &rng;
    if (asx_ws_engine_init(&g_cli, &cfg) != ASX_OK) return 1u;
    if (feed_all(&g_cli, (const uint8_t *)response, (uint32_t)strlen(response)) !=
        ASX_E_DISCONNECTED) {
        return 2u;
    }
    if (asx_ws_engine_state(&g_cli) != ASX_WS_STATE_CLOSED) return 3u;
    return asx_ws_engine_error_code(&g_cli);
}

TEST(client_engine_rejects_invalid_responses) {
    ASSERT_EQ(client_reject_with("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                                 "Connection: Upgrade\r\n"
                                 "Sec-WebSocket-Accept: AAAAAAAAAAAAAAAAAAAAAAAAAAA=\r\n\r\n"),
              ASX_WS_CLOSE_PROTOCOL_ERROR); /* wrong accept */
    ASSERT_EQ(client_reject_with("HTTP/1.1 200 OK\r\nUpgrade: websocket\r\n"
                                 "Connection: Upgrade\r\n"
                                 "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n"),
              ASX_WS_CLOSE_PROTOCOL_ERROR); /* not 101 */
    ASSERT_EQ(client_reject_with("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                                 "Connection: Upgrade\r\n"
                                 "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
                                 "Sec-WebSocket-Protocol: superchat\r\n\r\n"),
              ASX_WS_CLOSE_PROTOCOL_ERROR); /* protocol never offered */
    ASSERT_EQ(client_reject_with("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                                 "Connection: Upgrade\r\n"
                                 "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
                                 "Sec-WebSocket-Extensions: permessage-deflate\r\n\r\n"),
              ASX_WS_CLOSE_PROTOCOL_ERROR); /* extension never offered */
}

TEST(engine_init_validates_config) {
    asx_ws_config cfg;

    asx_ws_config_init(&cfg, ASX_WS_ROLE_CLIENT); /* no host */
    cfg.random_fn = fixed_random;
    {
        static const uint8_t nonce[] = "the sample nonce";
        fixed_rng rng = {nonce, 16u, 0u, 0u};
        cfg.random_ctx = &rng;
        ASSERT_EQ(asx_ws_engine_init(&g_cli, &cfg), ASX_E_INVALID_ARGUMENT);
    }
    cfg.host = "h";
    cfg.random_fn = failing_random;
    ASSERT_EQ(asx_ws_engine_init(&g_cli, &cfg), ASX_E_HOOK_MISSING);
    ASSERT_EQ(asx_ws_engine_init(NULL, &cfg), ASX_E_INVALID_ARGUMENT);
    asx_ws_config_init(&cfg, ASX_WS_ROLE_SERVER);
    cfg.protocols = "bad\r\nheader";
    ASSERT_EQ(asx_ws_engine_init(&g_srv, &cfg), ASX_E_INVALID_ARGUMENT);
}

/* ================================================================== */
/* Engine receive path                                                 */
/* ================================================================== */

TEST(server_engine_reassembles_masked_fragments_with_interleaved_ping) {
    static const uint8_t pong[] = {0x8a, 0x02, 'h', 'i'};
    uint8_t wire[64];
    uint8_t msg[16];
    uint32_t n = 0u;
    asx_ws_opcode op = ASX_WS_OPCODE_BINARY;
    uint32_t len = 0u;

    ASSERT_EQ(init_open(&g_srv, ASX_WS_ROLE_SERVER, 0u, NULL), ASX_OK);
    n += masked_frame(ASX_WS_OPCODE_TEXT, 0, "Hel", 3u, wire + n, (uint32_t)sizeof(wire) - n);
    n += masked_frame(ASX_WS_OPCODE_PING, 1, "hi", 2u, wire + n, (uint32_t)sizeof(wire) - n);
    n +=
        masked_frame(ASX_WS_OPCODE_CONTINUATION, 1, "lo", 2u, wire + n, (uint32_t)sizeof(wire) - n);

    /* byte-at-a-time delivery exercises every partial-header path */
    {
        uint32_t i;
        for (i = 0u; i < n; i++) ASSERT_EQ(feed_all(&g_srv, wire + i, 1u), ASX_OK);
    }
    ASSERT_EQ(g_srv.pings_received, 1u);
    ASSERT_TRUE(output_equals(&g_srv, pong, sizeof(pong))); /* automatic, unmasked */
    ASSERT_EQ(asx_ws_engine_recv(&g_srv, &op, msg, sizeof(msg), &len), ASX_OK);
    ASSERT_EQ(op, ASX_WS_OPCODE_TEXT);
    ASSERT_TRUE(len == 5u && memcmp(msg, "Hello", 5u) == 0);
    ASSERT_EQ(asx_ws_engine_recv(&g_srv, &op, msg, sizeof(msg), &len), ASX_E_PENDING);
}

TEST(engine_backpressure_one_message_at_a_time) {
    uint8_t wire[64];
    uint8_t msg[16];
    uint32_t n = 0u;
    uint32_t used = 0u;
    uint32_t first;
    asx_ws_opcode op = ASX_WS_OPCODE_TEXT;
    uint32_t len = 0u;

    ASSERT_EQ(init_open(&g_srv, ASX_WS_ROLE_SERVER, 0u, NULL), ASX_OK);
    n += masked_frame(ASX_WS_OPCODE_BINARY, 1, "one", 3u, wire + n, (uint32_t)sizeof(wire) - n);
    first = n;
    n += masked_frame(ASX_WS_OPCODE_BINARY, 1, "two", 3u, wire + n, (uint32_t)sizeof(wire) - n);

    ASSERT_EQ(asx_ws_engine_feed(&g_srv, wire, n, &used), ASX_OK);
    ASSERT_EQ(used, first); /* stops after the first complete message */
    ASSERT_EQ(asx_ws_engine_recv(&g_srv, &op, msg, 2u, &len), ASX_E_BUFFER_TOO_SMALL);
    ASSERT_EQ(len, 3u); /* retained, required size reported */
    ASSERT_EQ(asx_ws_engine_recv(&g_srv, &op, msg, sizeof(msg), &len), ASX_OK);
    ASSERT_TRUE(op == ASX_WS_OPCODE_BINARY && len == 3u && memcmp(msg, "one", 3u) == 0);
    ASSERT_EQ(asx_ws_engine_feed(&g_srv, wire + used, n - used, &used), ASX_OK);
    ASSERT_EQ(asx_ws_engine_recv(&g_srv, &op, msg, sizeof(msg), &len), ASX_OK);
    ASSERT_TRUE(len == 3u && memcmp(msg, "two", 3u) == 0);
}

/* Feed a frame into a fresh OPEN engine and report the failure close code
 * plus the close frame it queued. */
static uint16_t fail_code_for(asx_ws_role role, uint32_t max_message, const uint8_t *wire,
                              uint32_t n) {
    asx_ws_engine *ws = (role == ASX_WS_ROLE_SERVER) ? &g_srv : &g_cli;
    static const uint8_t mask_key[4] = {1, 2, 3, 4};
    fixed_rng rng = {mask_key, 4u, 0u, 0u};
    const uint8_t *out = NULL;
    uint32_t olen;
    uint16_t code;

    if (init_open(ws, role, max_message, &rng) != ASX_OK) return 1u;
    if (feed_all(ws, wire, n) != ASX_E_DISCONNECTED) return 2u;
    if (asx_ws_engine_state(ws) != ASX_WS_STATE_CLOSED) return 3u;
    code = asx_ws_engine_error_code(ws);
    /* the queued close frame must carry the same code */
    olen = asx_ws_engine_output(ws, &out);
    if (role == ASX_WS_ROLE_SERVER) {
        if (olen != 4u || out[0] != 0x88 || out[1] != 0x02) return 4u;
        if ((uint16_t)((out[2] << 8) | out[3]) != code) return 5u;
    } else {
        uint8_t body[2];
        if (olen != 8u || out[0] != 0x88 || out[1] != 0x82) return 6u;
        body[0] = out[6];
        body[1] = out[7];
        asx_ws_mask(body, 2u, out + 2, 0u);
        if ((uint16_t)((body[0] << 8) | body[1]) != code) return 7u;
    }
    /* further input is ignored */
    if (feed_all(ws, wire, n) != ASX_E_DISCONNECTED) return 8u;
    return code;
}

TEST(engine_protocol_errors_map_to_1002) {
    uint8_t wire[300];
    uint32_t n;

    /* unmasked client->server frame */
    ASSERT_EQ(asx_ws_frame_encode(ASX_WS_OPCODE_TEXT, 1, NULL, k_hello, 5u, wire, sizeof(wire), &n),
              ASX_OK);
    ASSERT_EQ(fail_code_for(ASX_WS_ROLE_SERVER, 0u, wire, n), ASX_WS_CLOSE_PROTOCOL_ERROR);

    /* masked server->client frame */
    n = masked_frame(ASX_WS_OPCODE_TEXT, 1, "Hello", 5u, wire, sizeof(wire));
    ASSERT_EQ(fail_code_for(ASX_WS_ROLE_CLIENT, 0u, wire, n), ASX_WS_CLOSE_PROTOCOL_ERROR);

    /* reserved bit RSV1 */
    n = masked_frame(ASX_WS_OPCODE_TEXT, 1, "Hello", 5u, wire, sizeof(wire));
    wire[0] |= 0x40u;
    ASSERT_EQ(fail_code_for(ASX_WS_ROLE_SERVER, 0u, wire, n), ASX_WS_CLOSE_PROTOCOL_ERROR);

    /* reserved opcode 0x3 */
    n = masked_frame(ASX_WS_OPCODE_BINARY, 1, "x", 1u, wire, sizeof(wire));
    wire[0] = (uint8_t)((wire[0] & 0xF0u) | 0x03u);
    ASSERT_EQ(fail_code_for(ASX_WS_ROLE_SERVER, 0u, wire, n), ASX_WS_CLOSE_PROTOCOL_ERROR);

    /* continuation without a started message */
    n = masked_frame(ASX_WS_OPCODE_CONTINUATION, 1, "x", 1u, wire, sizeof(wire));
    ASSERT_EQ(fail_code_for(ASX_WS_ROLE_SERVER, 0u, wire, n), ASX_WS_CLOSE_PROTOCOL_ERROR);

    /* new data frame while a fragmented message is in progress */
    n = masked_frame(ASX_WS_OPCODE_TEXT, 0, "a", 1u, wire, sizeof(wire));
    n += masked_frame(ASX_WS_OPCODE_TEXT, 1, "b", 1u, wire + n, (uint32_t)sizeof(wire) - n);
    ASSERT_EQ(fail_code_for(ASX_WS_ROLE_SERVER, 0u, wire, n), ASX_WS_CLOSE_PROTOCOL_ERROR);

    /* fragmented ping (FIN = 0) */
    n = masked_frame(ASX_WS_OPCODE_PING, 1, "p", 1u, wire, sizeof(wire));
    wire[0] &= 0x7Fu;
    ASSERT_EQ(fail_code_for(ASX_WS_ROLE_SERVER, 0u, wire, n), ASX_WS_CLOSE_PROTOCOL_ERROR);

    /* ping with a 126-byte payload (16-bit length form) */
    {
        static const uint8_t big_ping[] = {0x89, 0xFE, 0x00, 0x7E, 0, 0, 0, 0};
        ASSERT_EQ(fail_code_for(ASX_WS_ROLE_SERVER, 0u, big_ping, sizeof(big_ping)),
                  ASX_WS_CLOSE_PROTOCOL_ERROR);
    }

    /* non-minimal length encoding */
    {
        static const uint8_t nonmin[] = {0x82, 0xFE, 0x00, 0x05, 0, 0, 0, 0};
        ASSERT_EQ(fail_code_for(ASX_WS_ROLE_SERVER, 0u, nonmin, sizeof(nonmin)),
                  ASX_WS_CLOSE_PROTOCOL_ERROR);
    }

    /* close frame with a 1-byte payload, and with a forbidden code */
    {
        uint8_t one = 0x03;
        uint8_t bad_code[2] = {0x03, 0xED}; /* 1005 */
        n = masked_frame(ASX_WS_OPCODE_CLOSE, 1, &one, 1u, wire, sizeof(wire));
        ASSERT_EQ(fail_code_for(ASX_WS_ROLE_SERVER, 0u, wire, n), ASX_WS_CLOSE_PROTOCOL_ERROR);
        n = masked_frame(ASX_WS_OPCODE_CLOSE, 1, bad_code, 2u, wire, sizeof(wire));
        ASSERT_EQ(fail_code_for(ASX_WS_ROLE_SERVER, 0u, wire, n), ASX_WS_CLOSE_PROTOCOL_ERROR);
    }
}

TEST(engine_invalid_utf8_maps_to_1007) {
    static const uint8_t bad[] = {'o', 'k', 0xC0, 0x80};
    static const uint8_t euro_head[] = {0xE2, 0x82};
    static const uint8_t euro_tail[] = {0xAC};
    uint8_t wire[64];
    uint8_t msg[8];
    uint32_t n;
    asx_ws_opcode op = ASX_WS_OPCODE_BINARY;
    uint32_t len = 0u;

    n = masked_frame(ASX_WS_OPCODE_TEXT, 1, bad, sizeof(bad), wire, sizeof(wire));
    ASSERT_EQ(fail_code_for(ASX_WS_ROLE_SERVER, 0u, wire, n), ASX_WS_CLOSE_INVALID_PAYLOAD);

    /* truncated code point at FIN */
    n = masked_frame(ASX_WS_OPCODE_TEXT, 1, euro_head, 2u, wire, sizeof(wire));
    ASSERT_EQ(fail_code_for(ASX_WS_ROLE_SERVER, 0u, wire, n), ASX_WS_CLOSE_INVALID_PAYLOAD);

    /* close reason with invalid UTF-8 */
    {
        uint8_t body[4] = {0x03, 0xE8, 0xFF, 0xFE};
        n = masked_frame(ASX_WS_OPCODE_CLOSE, 1, body, 4u, wire, sizeof(wire));
        ASSERT_EQ(fail_code_for(ASX_WS_ROLE_SERVER, 0u, wire, n), ASX_WS_CLOSE_INVALID_PAYLOAD);
    }

    /* but a code point split across fragments is fine */
    ASSERT_EQ(init_open(&g_srv, ASX_WS_ROLE_SERVER, 0u, NULL), ASX_OK);
    n = masked_frame(ASX_WS_OPCODE_TEXT, 0, euro_head, 2u, wire, sizeof(wire));
    n += masked_frame(ASX_WS_OPCODE_CONTINUATION, 1, euro_tail, 1u, wire + n,
                      (uint32_t)sizeof(wire) - n);
    ASSERT_EQ(feed_all(&g_srv, wire, n), ASX_OK);
    ASSERT_EQ(asx_ws_engine_recv(&g_srv, &op, msg, sizeof(msg), &len), ASX_OK);
    ASSERT_TRUE(op == ASX_WS_OPCODE_TEXT && len == 3u && msg[0] == 0xE2 && msg[2] == 0xAC);
}

TEST(engine_oversized_message_maps_to_1009) {
    static uint8_t payload[17];
    uint8_t wire[64];
    uint32_t n;

    memset(payload, 'a', sizeof(payload));
    n = masked_frame(ASX_WS_OPCODE_BINARY, 1, payload, 17u, wire, sizeof(wire));
    ASSERT_EQ(fail_code_for(ASX_WS_ROLE_SERVER, 16u, wire, n), ASX_WS_CLOSE_MESSAGE_TOO_BIG);

    /* the limit applies to the reassembled message, not each fragment */
    n = masked_frame(ASX_WS_OPCODE_BINARY, 0, payload, 10u, wire, sizeof(wire));
    n += masked_frame(ASX_WS_OPCODE_CONTINUATION, 1, payload, 7u, wire + n,
                      (uint32_t)sizeof(wire) - n);
    ASSERT_EQ(fail_code_for(ASX_WS_ROLE_SERVER, 16u, wire, n), ASX_WS_CLOSE_MESSAGE_TOO_BIG);

    /* a 64 KiB header is refused before any payload arrives */
    {
        static const uint8_t hdr64k[] = {0x82, 0xFF, 0, 0, 0, 0, 0, 1, 0, 0, 1, 2, 3, 4};
        ASSERT_EQ(fail_code_for(ASX_WS_ROLE_SERVER, 0u, hdr64k, sizeof(hdr64k)),
                  ASX_WS_CLOSE_MESSAGE_TOO_BIG);
    }
}

/* ================================================================== */
/* Engine send path and close handshake                                */
/* ================================================================== */

TEST(engine_send_validation_is_failure_atomic) {
    static const uint8_t bad_utf8[] = {0xC3};
    static const uint8_t euro[] = {0xE2, 0x82, 0xAC};
    uint8_t big[126];

    memset(big, 0, sizeof(big));
    ASSERT_EQ(init_open(&g_srv, ASX_WS_ROLE_SERVER, 0u, NULL), ASX_OK);
    ASSERT_EQ(asx_ws_engine_send(&g_srv, ASX_WS_OPCODE_TEXT, bad_utf8, 1u), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_ws_engine_send(&g_srv, ASX_WS_OPCODE_PING, big, 126u), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_ws_engine_send(&g_srv, ASX_WS_OPCODE_CLOSE, NULL, 0u), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_ws_engine_send_frame(&g_srv, ASX_WS_OPCODE_CONTINUATION, 1, euro, 1u),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_ws_engine_output(&g_srv, NULL), 0u); /* nothing queued */

    /* fragmented text may split a code point across frames */
    ASSERT_EQ(asx_ws_engine_send_frame(&g_srv, ASX_WS_OPCODE_TEXT, 0, euro, 2u), ASX_OK);
    ASSERT_EQ(asx_ws_engine_send_frame(&g_srv, ASX_WS_OPCODE_BINARY, 1, euro, 1u),
              ASX_E_INVALID_ARGUMENT); /* message still open */
    ASSERT_EQ(asx_ws_engine_send(&g_srv, ASX_WS_OPCODE_PING, NULL, 0u), ASX_OK); /* interleave */
    ASSERT_EQ(asx_ws_engine_send_frame(&g_srv, ASX_WS_OPCODE_CONTINUATION, 1, euro + 2, 1u),
              ASX_OK);
    {
        static const uint8_t expect[] = {0x01, 0x02, 0xE2, 0x82, 0x89, 0x00, 0x80, 0x01, 0xAC};
        ASSERT_TRUE(output_equals(&g_srv, expect, sizeof(expect)));
    }
    /* sending before the handshake completes is refused */
    {
        asx_ws_config cfg;
        asx_ws_config_init(&cfg, ASX_WS_ROLE_SERVER);
        ASSERT_EQ(asx_ws_engine_init(&g_srv, &cfg), ASX_OK);
        ASSERT_EQ(asx_ws_engine_send(&g_srv, ASX_WS_OPCODE_TEXT, k_hello, 5u), ASX_E_INVALID_STATE);
        ASSERT_EQ(asx_ws_engine_close(&g_srv, ASX_WS_CLOSE_NORMAL, NULL), ASX_E_INVALID_STATE);
    }
}

TEST(close_handshake_initiated_locally) {
    static const uint8_t expect_close[] = {0x88, 0x06, 0x03, 0xE8, 'd', 'o', 'n', 'e'};
    char long_reason[130];
    uint8_t wire[16];
    uint32_t n;

    ASSERT_EQ(init_open(&g_srv, ASX_WS_ROLE_SERVER, 0u, NULL), ASX_OK);
    memset(long_reason, 'r', sizeof(long_reason) - 1u);
    long_reason[sizeof(long_reason) - 1u] = '\0';
    ASSERT_EQ(asx_ws_engine_close(&g_srv, 1005u, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_ws_engine_close(&g_srv, ASX_WS_CLOSE_NORMAL, long_reason),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_ws_engine_close(&g_srv, 0u, "reason-without-code"), ASX_E_INVALID_ARGUMENT);

    ASSERT_EQ(asx_ws_engine_close(&g_srv, ASX_WS_CLOSE_NORMAL, "done"), ASX_OK);
    ASSERT_EQ(asx_ws_engine_state(&g_srv), ASX_WS_STATE_CLOSING);
    ASSERT_TRUE(output_equals(&g_srv, expect_close, sizeof(expect_close)));
    ASSERT_EQ(asx_ws_engine_send(&g_srv, ASX_WS_OPCODE_TEXT, k_hello, 5u), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_ws_engine_close(&g_srv, ASX_WS_CLOSE_NORMAL, NULL), ASX_E_INVALID_STATE);
    clear_output(&g_srv);

    /* peer echoes the close: handshake complete, nothing more to send */
    {
        uint8_t code[2] = {0x03, 0xE8};
        n = masked_frame(ASX_WS_OPCODE_CLOSE, 1, code, 2u, wire, sizeof(wire));
    }
    ASSERT_EQ(feed_all(&g_srv, wire, n), ASX_OK);
    ASSERT_EQ(asx_ws_engine_state(&g_srv), ASX_WS_STATE_CLOSED);
    ASSERT_EQ(asx_ws_engine_close_code(&g_srv), ASX_WS_CLOSE_NORMAL);
    ASSERT_EQ(asx_ws_engine_output(&g_srv, NULL), 0u);
    ASSERT_EQ(asx_ws_engine_error_code(&g_srv), 0u);
}

TEST(close_handshake_initiated_by_peer) {
    static const uint8_t echo[] = {0x88, 0x02, 0x03, 0xE9};
    uint8_t body[5] = {0x03, 0xE9, 'b', 'y', 'e'}; /* 1001 "bye" */
    uint8_t wire[32];
    uint32_t n;
    uint32_t used = 0u;

    ASSERT_EQ(init_open(&g_srv, ASX_WS_ROLE_SERVER, 0u, NULL), ASX_OK);
    n = masked_frame(ASX_WS_OPCODE_CLOSE, 1, body, sizeof(body), wire, sizeof(wire));
    n += masked_frame(ASX_WS_OPCODE_TEXT, 1, "late", 4u, wire + n, (uint32_t)sizeof(wire) - n);
    ASSERT_EQ(asx_ws_engine_feed(&g_srv, wire, n, &used), ASX_OK);
    ASSERT_EQ(used, n); /* trailing data after close is discarded */
    ASSERT_EQ(asx_ws_engine_state(&g_srv), ASX_WS_STATE_CLOSED);
    ASSERT_EQ(asx_ws_engine_close_code(&g_srv), ASX_WS_CLOSE_GOING_AWAY);
    ASSERT_STR_EQ(asx_ws_engine_close_reason(&g_srv), "bye");
    ASSERT_TRUE(output_equals(&g_srv, echo, sizeof(echo)));

    /* an empty close payload reports 1005 and is echoed empty */
    {
        static const uint8_t empty_echo[] = {0x88, 0x00};
        ASSERT_EQ(init_open(&g_srv, ASX_WS_ROLE_SERVER, 0u, NULL), ASX_OK);
        n = masked_frame(ASX_WS_OPCODE_CLOSE, 1, NULL, 0u, wire, sizeof(wire));
        ASSERT_EQ(feed_all(&g_srv, wire, n), ASX_OK);
        ASSERT_EQ(asx_ws_engine_close_code(&g_srv), ASX_WS_CLOSE_NO_STATUS);
        ASSERT_TRUE(output_equals(&g_srv, empty_echo, sizeof(empty_echo)));
    }
}

TEST(transport_close_is_abnormal) {
    ASSERT_EQ(init_open(&g_srv, ASX_WS_ROLE_SERVER, 0u, NULL), ASX_OK);
    asx_ws_engine_transport_closed(&g_srv);
    ASSERT_EQ(asx_ws_engine_state(&g_srv), ASX_WS_STATE_CLOSED);
    ASSERT_EQ(asx_ws_engine_close_code(&g_srv), ASX_WS_CLOSE_ABNORMAL);
}

/* ================================================================== */
/* End to end over the in-memory TCP loopback                          */
/* ================================================================== */

typedef struct {
    asx_tcp_listener lis;
    asx_tcp_stream client_tcp;
    asx_tcp_stream server_tcp;
} tcp_pair;

static int tcp_pair_open(tcp_pair *p, uint16_t port) {
    asx_socket_addr addr = asx_socket_addr_loopback(port);
    if (asx_tcp_listener_bind(&p->lis, &addr) != ASX_OK) return 0;
    if (asx_tcp_connect(&p->client_tcp, &addr) != ASX_OK) return 0;
    if (asx_tcp_listener_poll_accept(p->lis, &p->server_tcp, NULL) != ASX_OK) return 0;
    return 1;
}

static void tcp_pair_close(tcp_pair *p) {
    (void)asx_tcp_stream_close(p->client_tcp);
    (void)asx_tcp_stream_close(p->server_tcp);
    (void)asx_tcp_listener_close(p->lis);
}

static asx_status recv_pumped(asx_ws_conn rx, asx_ws_conn other, asx_ws_opcode *op, void *buf,
                              uint32_t cap, uint32_t *len) {
    unsigned i;
    asx_status st = ASX_E_PENDING;
    for (i = 0u; i < 64u; i++) {
        st = asx_ws_poll_recv(rx, op, buf, cap, len);
        if (st != ASX_E_PENDING) return st;
        st = asx_ws_poll(other);
        (void)st;
        st = ASX_E_PENDING;
    }
    return st;
}

TEST(e2e_client_server_over_tcp_loopback) {
    static uint8_t big_out[10000];
    static uint8_t big_in[12000];
    tcp_pair tp;
    asx_ws_config ccfg;
    asx_ws_config scfg;
    asx_ws_conn client;
    asx_ws_conn server;
    asx_ws_opcode op = ASX_WS_OPCODE_CONTINUATION;
    uint8_t buf[64];
    uint32_t len = 0u;
    unsigned i;

    asx_net_reset();
    asx_ws_reset();
    install_entropy();
    ASSERT_TRUE(tcp_pair_open(&tp, 13100u));

    asx_ws_config_init(&ccfg, ASX_WS_ROLE_CLIENT);
    ccfg.host = "127.0.0.1:13100";
    ccfg.path = "/chat";
    ccfg.protocols = "chat, superchat";
    asx_ws_config_init(&scfg, ASX_WS_ROLE_SERVER);
    scfg.protocols = "superchat";

    ASSERT_EQ(asx_ws_connect(&client, tp.client_tcp, &ccfg), ASX_OK);
    ASSERT_EQ(asx_ws_accept(&server, tp.server_tcp, &scfg), ASX_OK);
    ASSERT_EQ(asx_ws_conn_role(client), ASX_WS_ROLE_CLIENT);
    ASSERT_EQ(asx_ws_conn_role(server), ASX_WS_ROLE_SERVER);
    ASSERT_EQ(asx_ws_conn_state(client), ASX_WS_STATE_CONNECTING);

    pump(server, client, 4u);
    ASSERT_EQ(asx_ws_conn_state(server), ASX_WS_STATE_OPEN);
    ASSERT_EQ(asx_ws_conn_state(client), ASX_WS_STATE_OPEN);
    ASSERT_STR_EQ(asx_ws_engine_protocol(asx_ws_conn_engine(client)), "superchat");
    ASSERT_STR_EQ(asx_ws_engine_protocol(asx_ws_conn_engine(server)), "superchat");

    /* client -> server text */
    ASSERT_EQ(asx_ws_send(client, ASX_WS_OPCODE_TEXT, "hello server", 12u), ASX_OK);
    ASSERT_EQ(recv_pumped(server, client, &op, buf, sizeof(buf), &len), ASX_OK);
    ASSERT_TRUE(op == ASX_WS_OPCODE_TEXT && len == 12u && memcmp(buf, "hello server", 12u) == 0);

    /* server -> client binary */
    ASSERT_EQ(asx_ws_send(server, ASX_WS_OPCODE_BINARY, "\x00\x01\x02", 3u), ASX_OK);
    ASSERT_EQ(recv_pumped(client, server, &op, buf, sizeof(buf), &len), ASX_OK);
    ASSERT_TRUE(op == ASX_WS_OPCODE_BINARY && len == 3u && buf[2] == 0x02);

    /* ping from the client is answered automatically by the server */
    ASSERT_EQ(asx_ws_send(client, ASX_WS_OPCODE_PING, "beat", 4u), ASX_OK);
    pump(server, client, 4u);
    ASSERT_EQ(asx_ws_conn_engine(server)->pings_received, 1u);
    ASSERT_EQ(asx_ws_conn_engine(client)->pongs_received, 1u);

    /* fragmented server -> client message */
    ASSERT_EQ(asx_ws_send_frame(server, ASX_WS_OPCODE_TEXT, 0, "Hel", 3u), ASX_OK);
    ASSERT_EQ(asx_ws_send_frame(server, ASX_WS_OPCODE_CONTINUATION, 1, "lo", 2u), ASX_OK);
    ASSERT_EQ(recv_pumped(client, server, &op, buf, sizeof(buf), &len), ASX_OK);
    ASSERT_TRUE(op == ASX_WS_OPCODE_TEXT && len == 5u && memcmp(buf, "Hello", 5u) == 0);

    /* a 10000-byte message crosses the 4 KiB in-memory TCP buffers */
    for (i = 0u; i < sizeof(big_out); i++) big_out[i] = (uint8_t)(i * 31u + 7u);
    ASSERT_EQ(asx_ws_send(client, ASX_WS_OPCODE_BINARY, big_out, sizeof(big_out)), ASX_OK);
    ASSERT_EQ(recv_pumped(server, client, &op, big_in, sizeof(big_in), &len), ASX_OK);
    ASSERT_TRUE(op == ASX_WS_OPCODE_BINARY && len == sizeof(big_out));
    ASSERT_TRUE(memcmp(big_in, big_out, sizeof(big_out)) == 0);

    /* close handshake: client initiates, server echoes */
    ASSERT_EQ(asx_ws_close(client, ASX_WS_CLOSE_NORMAL, "bye"), ASX_OK);
    ASSERT_EQ(asx_ws_conn_state(client), ASX_WS_STATE_CLOSING);
    pump(server, client, 4u);
    ASSERT_EQ(asx_ws_conn_state(server), ASX_WS_STATE_CLOSED);
    ASSERT_EQ(asx_ws_conn_state(client), ASX_WS_STATE_CLOSED);
    ASSERT_EQ(asx_ws_engine_close_code(asx_ws_conn_engine(server)), ASX_WS_CLOSE_NORMAL);
    ASSERT_STR_EQ(asx_ws_engine_close_reason(asx_ws_conn_engine(server)), "bye");
    ASSERT_EQ(asx_ws_engine_close_code(asx_ws_conn_engine(client)), ASX_WS_CLOSE_NORMAL);
    ASSERT_EQ(asx_ws_send(client, ASX_WS_OPCODE_TEXT, "x", 1u), ASX_E_INVALID_STATE);

    ASSERT_EQ(asx_ws_conn_release(client), ASX_OK);
    ASSERT_EQ(asx_ws_conn_release(server), ASX_OK);
    ASSERT_FALSE(asx_ws_conn_is_alive(client));
    ASSERT_EQ(asx_ws_poll(client), ASX_E_INVALID_ARGUMENT); /* stale handle */
    ASSERT_EQ(asx_ws_conn_state(client), ASX_WS_STATE_CLOSED);
    tcp_pair_close(&tp);
}

TEST(e2e_server_rejects_plain_http_client) {
    static const char req[] = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    tcp_pair tp;
    asx_ws_conn server;
    asx_buf src;
    asx_buf_mut dst;
    uint32_t n = 0u;
    unsigned i;
    asx_status st = ASX_OK;

    asx_net_reset();
    asx_ws_reset();
    install_entropy();
    ASSERT_TRUE(tcp_pair_open(&tp, 13101u));
    ASSERT_EQ(asx_ws_accept(&server, tp.server_tcp, NULL), ASX_OK);

    src = asx_buf_from(req, (uint32_t)(sizeof(req) - 1u));
    ASSERT_EQ(asx_tcp_stream_poll_write(tp.client_tcp, &src, &n), ASX_OK);
    for (i = 0u; i < 4u && st == ASX_OK; i++) st = asx_ws_poll(server);
    ASSERT_EQ(st, ASX_E_DISCONNECTED);
    ASSERT_EQ(asx_ws_conn_state(server), ASX_WS_STATE_CLOSED);

    asx_buf_mut_init(&dst);
    ASSERT_EQ(asx_tcp_stream_poll_read(tp.client_tcp, &dst, &n), ASX_OK);
    ASSERT_TRUE(n >= 12u && memcmp(dst.data, "HTTP/1.1 400", 12u) == 0);

    ASSERT_EQ(asx_ws_conn_release(server), ASX_OK);
    tcp_pair_close(&tp);
}

TEST(e2e_stream_closed_underneath_is_abnormal) {
    tcp_pair tp;
    asx_ws_config ccfg;
    asx_ws_conn client;
    asx_ws_conn server;

    asx_net_reset();
    asx_ws_reset();
    install_entropy();
    ASSERT_TRUE(tcp_pair_open(&tp, 13103u));
    asx_ws_config_init(&ccfg, ASX_WS_ROLE_CLIENT);
    ccfg.host = "localhost";
    ASSERT_EQ(asx_ws_connect(&client, tp.client_tcp, &ccfg), ASX_OK);
    ASSERT_EQ(asx_ws_accept(&server, tp.server_tcp, NULL), ASX_OK);
    pump(server, client, 4u);
    ASSERT_EQ(asx_ws_conn_state(client), ASX_WS_STATE_OPEN);
    ASSERT_STR_EQ(asx_ws_engine_protocol(asx_ws_conn_engine(client)), "");

    ASSERT_EQ(asx_tcp_stream_close(tp.server_tcp), ASX_OK);
    ASSERT_EQ(asx_ws_poll(server), ASX_E_DISCONNECTED);
    ASSERT_EQ(asx_ws_conn_state(server), ASX_WS_STATE_CLOSED);
    ASSERT_EQ(asx_ws_engine_close_code(asx_ws_conn_engine(server)), ASX_WS_CLOSE_ABNORMAL);

    ASSERT_EQ(asx_ws_conn_release(client), ASX_OK);
    ASSERT_EQ(asx_ws_conn_release(server), ASX_OK);
    (void)asx_tcp_stream_close(tp.client_tcp);
    (void)asx_tcp_listener_close(tp.lis);
}

TEST(conn_slots_exhaust_and_validate_args) {
    tcp_pair tp;
    asx_ws_conn conns[ASX_MAX_WS_CONNECTIONS];
    asx_ws_conn extra;
    asx_ws_config ccfg;
    uint32_t i;

    asx_net_reset();
    asx_ws_reset();
    install_entropy();
    ASSERT_TRUE(tcp_pair_open(&tp, 13102u));
    for (i = 0u; i < ASX_MAX_WS_CONNECTIONS; i++) {
        ASSERT_EQ(asx_ws_accept(&conns[i], tp.server_tcp, NULL), ASX_OK);
    }
    ASSERT_EQ(asx_ws_accept(&extra, tp.server_tcp, NULL), ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(asx_ws_conn_release(conns[0]), ASX_OK);
    ASSERT_EQ(asx_ws_conn_release(conns[0]), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_ws_accept(&extra, tp.server_tcp, NULL), ASX_OK);
    ASSERT_TRUE(extra.slot == conns[0].slot && extra.generation != conns[0].generation);

    asx_ws_reset();
    asx_ws_config_init(&ccfg, ASX_WS_ROLE_CLIENT);
    ASSERT_EQ(asx_ws_connect(&extra, tp.client_tcp, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_ws_connect(&extra, tp.client_tcp, &ccfg), ASX_E_INVALID_ARGUMENT); /* no host */
    ccfg.host = "h";
    ASSERT_EQ(asx_ws_connect(NULL, tp.client_tcp, &ccfg), ASX_E_INVALID_ARGUMENT);
    tcp_pair_close(&tp);
    ASSERT_EQ(asx_ws_connect(&extra, tp.client_tcp, &ccfg), ASX_E_INVALID_ARGUMENT); /* dead tcp */
}

/* ================================================================== */
/* main                                                                */
/* ================================================================== */

int main(void) {
    fprintf(stderr, "=== websocket tests ===\n");

    /* RFC 6455 §5.7 framing examples */
    RUN_TEST(rfc_single_frame_unmasked_text);
    RUN_TEST(rfc_single_frame_masked_text);
    RUN_TEST(rfc_fragmented_unmasked_text);
    RUN_TEST(rfc_ping_and_masked_pong);
    RUN_TEST(rfc_256_byte_binary_uses_16_bit_length);
    RUN_TEST(rfc_64kib_binary_uses_64_bit_length);

    /* Codec strictness */
    RUN_TEST(header_decode_reports_pending_for_partial_headers);
    RUN_TEST(header_decode_rejects_non_minimal_and_msb_lengths);
    RUN_TEST(header_check_rules);
    RUN_TEST(frame_encode_rejects_bad_control_frames);
    RUN_TEST(mask_is_chunk_offset_consistent);
    RUN_TEST(utf8_validation_strict);
    RUN_TEST(close_code_ranges);

    /* Opening handshake */
    RUN_TEST(handshake_accept_rfc_example);
    RUN_TEST(handshake_build_request);
    RUN_TEST(server_engine_answers_rfc_request);
    RUN_TEST(server_engine_rejects_invalid_requests);
    RUN_TEST(client_engine_handshake_and_trailing_frame);
    RUN_TEST(client_engine_rejects_invalid_responses);
    RUN_TEST(engine_init_validates_config);

    /* Engine receive path */
    RUN_TEST(server_engine_reassembles_masked_fragments_with_interleaved_ping);
    RUN_TEST(engine_backpressure_one_message_at_a_time);
    RUN_TEST(engine_protocol_errors_map_to_1002);
    RUN_TEST(engine_invalid_utf8_maps_to_1007);
    RUN_TEST(engine_oversized_message_maps_to_1009);

    /* Engine send path and close handshake */
    RUN_TEST(engine_send_validation_is_failure_atomic);
    RUN_TEST(close_handshake_initiated_locally);
    RUN_TEST(close_handshake_initiated_by_peer);
    RUN_TEST(transport_close_is_abnormal);

    /* End to end over TCP loopback */
    RUN_TEST(e2e_client_server_over_tcp_loopback);
    RUN_TEST(e2e_server_rejects_plain_http_client);
    RUN_TEST(e2e_stream_closed_underneath_is_abnormal);
    RUN_TEST(conn_slots_exhaust_and_validate_args);

    TEST_REPORT();
    return test_failures;
}
