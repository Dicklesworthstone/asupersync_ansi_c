/*
 * test_http_wire.c — RFC 9112 HTTP/1.1 wire protocol tests
 *
 * Parser vectors fed in bulk, byte-at-a-time, and at every split point;
 * pipelining; chunked extensions and trailers; every request-smuggling
 * rejection; limit violations; response framing (204/304/HEAD/1xx/101/
 * close-delimited); serializer round-trips; and end-to-end client<->server
 * exchanges over the in-memory TCP loopback through the router.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/asx_config.h>
#include <asx/net/http.h>
#include <asx/net/net.h>
#include <asx/net/server.h>
#include <asx/runtime/browser_boundary.h>
#include <asx/runtime/runtime.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Parser capture harness                                              */
/* ------------------------------------------------------------------ */

#define CAP_MAX_MSGS 4u
#define CAP_BODY_MAX 1024u

typedef struct {
    uint32_t heads;
    uint32_t completes;
    uint32_t body_events;
    char text[CAP_MAX_MSGS][128]; /* request target or reason phrase */
    asx_http_status status[CAP_MAX_MSGS];
    int keep_alive[CAP_MAX_MSGS];
    uint8_t body[CAP_MAX_MSGS][CAP_BODY_MAX];
    uint32_t body_len[CAP_MAX_MSGS];
} capture;

static uint8_t g_head[8192];
static asx_http_parser g_parser;
static capture g_cap;

#define LIT(s) (s), (uint32_t)(sizeof(s) - 1u)

static void cap_event(const asx_http_parser *p, const asx_http_event *ev, capture *cap) {
    uint32_t idx;

    switch (ev->kind) {
    case ASX_HTTP_EVENT_HEAD:
        if (cap->heads < CAP_MAX_MSGS) {
            idx = cap->heads;
            (void)snprintf(cap->text[idx], sizeof(cap->text[idx]), "%s",
                           p->kind == ASX_HTTP_PARSE_REQUEST ? asx_http_parser_target(p)
                                                             : asx_http_parser_reason(p));
            cap->status[idx] = asx_http_parser_status(p);
            cap->keep_alive[idx] = asx_http_parser_keep_alive(p);
        }
        cap->heads++;
        break;
    case ASX_HTTP_EVENT_BODY:
        cap->body_events++;
        if (cap->heads > 0u && cap->heads <= CAP_MAX_MSGS) {
            idx = cap->heads - 1u;
            if (cap->body_len[idx] + ev->len <= CAP_BODY_MAX) {
                memcpy(cap->body[idx] + cap->body_len[idx], ev->data, ev->len);
                cap->body_len[idx] += ev->len;
            }
        }
        break;
    case ASX_HTTP_EVENT_MESSAGE_COMPLETE: cap->completes++; break;
    case ASX_HTTP_EVENT_NONE: break;
    }
}

/* Feed one slice, draining every event it produces. */
static asx_status feed_slice(asx_http_parser *p, const uint8_t *data, uint32_t len, capture *cap) {
    uint32_t pos = 0u;
    uint32_t guard;

    for (guard = 0u; guard < 100000u; guard++) {
        asx_http_event ev;
        uint32_t used = 0u;
        asx_status st = asx_http_parser_feed(p, data + pos, len - pos, &used, &ev);
        pos += used;
        if (st != ASX_OK) return st;
        if (ev.kind == ASX_HTTP_EVENT_NONE) return pos == len ? ASX_OK : ASX_E_INVALID_STATE;
        cap_event(p, &ev, cap);
    }
    return ASX_E_INVALID_STATE;
}

/* Feed `len` bytes in slices of `step` bytes (0 = a single slice). */
static asx_status feed_steps(asx_http_parser *p, const void *data, uint32_t len, uint32_t step,
                             capture *cap) {
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t off = 0u;

    if (step == 0u) step = len == 0u ? 1u : len;
    while (off < len) {
        uint32_t n = (len - off) < step ? (len - off) : step;
        asx_status st = feed_slice(p, bytes + off, n, cap);
        if (st != ASX_OK) return st;
        off += n;
    }
    return ASX_OK;
}

static asx_status parse_with(asx_http_parse_kind kind, const asx_http_limits *limits,
                             const void *data, uint32_t len, uint32_t step) {
    memset(&g_cap, 0, sizeof(g_cap));
    if (asx_http_parser_init(&g_parser, kind, limits, g_head, (uint32_t)sizeof(g_head)) != ASX_OK) {
        return ASX_E_INVALID_STATE;
    }
    return feed_steps(&g_parser, data, len, step, &g_cap);
}

static int cap_body_is(uint32_t idx, const char *expected) {
    size_t n = strlen(expected);
    return g_cap.body_len[idx] == (uint32_t)n && memcmp(g_cap.body[idx], expected, n) == 0;
}

/* ------------------------------------------------------------------ */
/* Request parsing                                                     */
/* ------------------------------------------------------------------ */

TEST(parse_simple_get) {
    static const char msg[] = "GET /index.html?x=1 HTTP/1.1\r\n"
                              "Host: example.com\r\n"
                              "User-Agent:  asx-test \t\r\n"
                              "Accept: */*\r\n"
                              "\r\n";
    const char *name = NULL;
    const char *value = NULL;

    ASSERT_EQ(parse_with(ASX_HTTP_PARSE_REQUEST, NULL, LIT(msg), 0u), ASX_OK);
    ASSERT_EQ(g_cap.heads, 1u);
    ASSERT_EQ(g_cap.completes, 1u);
    ASSERT_EQ(g_cap.body_events, 0u);
    ASSERT_EQ(asx_http_parser_method(&g_parser), ASX_HTTP_GET);
    ASSERT_STR_EQ(asx_http_parser_target(&g_parser), "/index.html?x=1");
    ASSERT_EQ(asx_http_parser_version(&g_parser), ASX_HTTP_VERSION_1_1);
    ASSERT_EQ(asx_http_parser_header_count(&g_parser), 3u);
    ASSERT_STR_EQ(asx_http_parser_header(&g_parser, "user-agent"), "asx-test");
    ASSERT_STR_EQ(asx_http_parser_header(&g_parser, "HOST"), "example.com");
    ASSERT_TRUE(asx_http_parser_header(&g_parser, "missing") == NULL);
    ASSERT_EQ(asx_http_parser_header_at(&g_parser, 2u, &name, &value), ASX_OK);
    ASSERT_STR_EQ(name, "Accept");
    ASSERT_STR_EQ(value, "*/*");
    ASSERT_EQ(asx_http_parser_header_at(&g_parser, 3u, &name, &value), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_http_parser_keep_alive(&g_parser), 1);
    ASSERT_EQ(asx_http_parser_body_framing(&g_parser), ASX_HTTP_BODY_FRAMING_NONE);
    ASSERT_EQ(asx_http_parser_expect_continue(&g_parser), 0);
    ASSERT_EQ(asx_http_parser_messages_completed(&g_parser), 1u);
    ASSERT_TRUE(asx_http_parser_is_idle(&g_parser));
}

TEST(parse_request_forms_and_versions) {
    ASSERT_EQ(parse_with(ASX_HTTP_PARSE_REQUEST, NULL, LIT("OPTIONS * HTTP/1.1\r\nHost: a\r\n\r\n"),
                         0u),
              ASX_OK);
    ASSERT_EQ(asx_http_parser_method(&g_parser), ASX_HTTP_OPTIONS);
    ASSERT_STR_EQ(asx_http_parser_target(&g_parser), "*");

    ASSERT_EQ(parse_with(ASX_HTTP_PARSE_REQUEST, NULL,
                         LIT("CONNECT example.com:443 HTTP/1.1\r\nHost: example.com\r\n\r\n"), 0u),
              ASX_OK);
    ASSERT_STR_EQ(asx_http_parser_target(&g_parser), "example.com:443");

    ASSERT_EQ(parse_with(ASX_HTTP_PARSE_REQUEST, NULL,
                         LIT("GET http://example.com/x?y HTTP/1.1\r\nHost: example.com\r\n\r\n"),
                         0u),
              ASX_OK);
    ASSERT_STR_EQ(asx_http_parser_target(&g_parser), "http://example.com/x?y");

    /* HTTP/1.0: Host optional, close by default. */
    ASSERT_EQ(parse_with(ASX_HTTP_PARSE_REQUEST, NULL, LIT("GET / HTTP/1.0\r\n\r\n"), 0u), ASX_OK);
    ASSERT_EQ(asx_http_parser_version(&g_parser), ASX_HTTP_VERSION_1_0);
    ASSERT_EQ(asx_http_parser_keep_alive(&g_parser), 0);

    /* Expect: 100-continue is honored only for HTTP/1.1. */
    ASSERT_EQ(parse_with(ASX_HTTP_PARSE_REQUEST, NULL,
                         LIT("PUT /f HTTP/1.1\r\nHost: a\r\nExpect: 100-Continue\r\n"
                             "Content-Length: 1\r\n\r\nx"),
                         0u),
              ASX_OK);
    ASSERT_EQ(asx_http_parser_expect_continue(&g_parser), 1);
    ASSERT_EQ(parse_with(ASX_HTTP_PARSE_REQUEST, NULL,
                         LIT("PUT /f HTTP/1.0\r\nExpect: 100-continue\r\n"
                             "Content-Length: 1\r\n\r\nx"),
                         0u),
              ASX_OK);
    ASSERT_EQ(asx_http_parser_expect_continue(&g_parser), 0);
}

TEST(parse_keep_alive_decisions_from_wire) {
    static const struct {
        const char *msg;
        int keep;
    } rows[] = {
        {"GET / HTTP/1.1\r\nHost: a\r\n\r\n", 1},
        {"GET / HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n", 0},
        {"GET / HTTP/1.1\r\nHost: a\r\nConnection: Upgrade, CLOSE\r\n\r\n", 0},
        {"GET / HTTP/1.1\r\nHost: a\r\nConnection: x-a\r\nConnection: close\r\n\r\n", 0},
        {"GET / HTTP/1.0\r\n\r\n", 0},
        {"GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n", 1},
        {"GET / HTTP/1.0\r\nConnection: Keep-Alive, close\r\n\r\n", 0},
    };
    uint32_t i;

    for (i = 0u; i < (uint32_t)(sizeof(rows) / sizeof(rows[0])); i++) {
        ASSERT_EQ(parse_with(ASX_HTTP_PARSE_REQUEST, NULL, rows[i].msg,
                             (uint32_t)strlen(rows[i].msg), 0u),
                  ASX_OK);
        ASSERT_EQ(asx_http_parser_keep_alive(&g_parser), rows[i].keep);
    }
}

static const char g_chunked_post[] = "POST /upload HTTP/1.1\r\n"
                                     "Host: h\r\n"
                                     "Transfer-Encoding: chunked\r\n"
                                     "\r\n"
                                     "5;name=val\r\n"
                                     "hello\r\n"
                                     "7\r\n"
                                     ", world\r\n"
                                     "0\r\n"
                                     "X-Sum: 42\r\n"
                                     "\r\n";

static int chunked_post_ok(void) {
    const char *name = NULL;
    const char *value = NULL;

    return g_cap.heads == 1u && g_cap.completes == 1u && cap_body_is(0u, "hello, world") &&
           asx_http_parser_body_framing(&g_parser) == ASX_HTTP_BODY_FRAMING_CHUNKED &&
           asx_http_parser_body_received(&g_parser) == 12u &&
           asx_http_parser_trailer_count(&g_parser) == 1u &&
           asx_http_parser_trailer_at(&g_parser, 0u, &name, &value) == ASX_OK &&
           strcmp(name, "X-Sum") == 0 && strcmp(value, "42") == 0 &&
           strcmp(asx_http_parser_target(&g_parser), "/upload") == 0;
}

TEST(parse_chunked_at_every_feed_granularity) {
    static const uint32_t steps[] = {0u, 1u, 2u, 3u, 5u, 7u, 11u, 64u};
    uint32_t i;

    for (i = 0u; i < (uint32_t)(sizeof(steps) / sizeof(steps[0])); i++) {
        ASSERT_EQ(parse_with(ASX_HTTP_PARSE_REQUEST, NULL, LIT(g_chunked_post), steps[i]), ASX_OK);
        ASSERT_TRUE(chunked_post_ok());
    }
}

TEST(parse_chunked_at_every_split_point) {
    const uint8_t *bytes = (const uint8_t *)g_chunked_post;
    uint32_t len = (uint32_t)(sizeof(g_chunked_post) - 1u);
    uint32_t k;

    for (k = 0u; k <= len; k++) {
        memset(&g_cap, 0, sizeof(g_cap));
        ASSERT_EQ(asx_http_parser_init(&g_parser, ASX_HTTP_PARSE_REQUEST, NULL, g_head,
                                       (uint32_t)sizeof(g_head)),
                  ASX_OK);
        ASSERT_EQ(feed_slice(&g_parser, bytes, k, &g_cap), ASX_OK);
        ASSERT_EQ(feed_slice(&g_parser, bytes + k, len - k, &g_cap), ASX_OK);
        ASSERT_TRUE(chunked_post_ok());
    }
}

TEST(parse_content_length_body_byte_at_a_time) {
    static const char msg[] =
        "PUT /doc HTTP/1.1\r\nHost: a\r\nContent-Length: 11\r\n\r\nhello world";

    ASSERT_EQ(parse_with(ASX_HTTP_PARSE_REQUEST, NULL, LIT(msg), 1u), ASX_OK);
    ASSERT_EQ(g_cap.completes, 1u);
    ASSERT_EQ(g_cap.body_events, 11u); /* one event per fed byte: no buffering */
    ASSERT_TRUE(cap_body_is(0u, "hello world"));
    ASSERT_EQ(asx_http_parser_content_length(&g_parser), 11u);
    ASSERT_EQ(asx_http_parser_body_framing(&g_parser), ASX_HTTP_BODY_FRAMING_LENGTH);
}

TEST(parse_pipelined_requests) {
    static const char msg[] = "GET /a HTTP/1.1\r\nHost: x\r\n\r\n"
                              "POST /b HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\nxyz"
                              "GET /c HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    static const uint32_t steps[] = {0u, 1u, 4u};
    uint32_t i;

    for (i = 0u; i < (uint32_t)(sizeof(steps) / sizeof(steps[0])); i++) {
        ASSERT_EQ(parse_with(ASX_HTTP_PARSE_REQUEST, NULL, LIT(msg), steps[i]), ASX_OK);
        ASSERT_EQ(g_cap.heads, 3u);
        ASSERT_EQ(g_cap.completes, 3u);
        ASSERT_STR_EQ(g_cap.text[0], "/a");
        ASSERT_STR_EQ(g_cap.text[1], "/b");
        ASSERT_STR_EQ(g_cap.text[2], "/c");
        ASSERT_EQ(g_cap.body_len[0], 0u);
        ASSERT_TRUE(cap_body_is(1u, "xyz"));
        ASSERT_EQ(g_cap.keep_alive[0], 1);
        ASSERT_EQ(g_cap.keep_alive[1], 1);
        ASSERT_EQ(g_cap.keep_alive[2], 0);
        ASSERT_EQ(asx_http_parser_messages_completed(&g_parser), 3u);
    }
}

TEST(parse_leading_empty_lines_bounded) {
    ASSERT_EQ(parse_with(ASX_HTTP_PARSE_REQUEST, NULL,
                         LIT("\r\n\r\nGET / HTTP/1.1\r\nHost: a\r\n\r\n"), 0u),
              ASX_OK);
    ASSERT_EQ(g_cap.completes, 1u);
    ASSERT_EQ(parse_with(ASX_HTTP_PARSE_REQUEST, NULL,
                         LIT("\r\n\r\n\r\n\r\n\r\nGET / HTTP/1.1\r\nHost: a\r\n\r\n"), 0u),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_http_parser_error(&g_parser), ASX_HTTP_PERR_BAD_START_LINE);
}

TEST(parse_chunk_extensions_and_trailers) {
    static const char msg[] = "POST /w HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: Chunked\r\n\r\n"
                              "4;a\r\nWiki\r\n"
                              "5 ;b=c ; d = \"e f\"\r\npedia\r\n"
                              "E;q=\"x;y \\\" z\"\r\n in\r\n\r\nchunks.\r\n"
                              "0;last\r\n"
                              "X-Trailer-A: one\r\n"
                              "X-Trailer-B:  two  \r\n"
                              "\r\n";
    const char *name = NULL;
    const char *value = NULL;
    uint32_t step;

    for (step = 0u; step < 3u; step++) {
        ASSERT_EQ(parse_with(ASX_HTTP_PARSE_REQUEST, NULL, LIT(msg), step), ASX_OK);
        ASSERT_EQ(g_cap.completes, 1u);
        ASSERT_TRUE(cap_body_is(0u, "Wikipedia in\r\n\r\nchunks."));
        ASSERT_EQ(asx_http_parser_trailer_count(&g_parser), 2u);
        ASSERT_EQ(asx_http_parser_trailer_at(&g_parser, 1u, &name, &value), ASX_OK);
        ASSERT_STR_EQ(name, "X-Trailer-B");
        ASSERT_STR_EQ(value, "two");
        ASSERT_EQ(asx_http_parser_trailer_at(&g_parser, 2u, &name, &value), ASX_E_NOT_FOUND);
        /* Headers stay intact next to the trailers. */
        ASSERT_STR_EQ(asx_http_parser_header(&g_parser, "host"), "x");
    }
}

/* ------------------------------------------------------------------ */
/* Request-smuggling and malformed-input rejections                    */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *msg;
    uint32_t len;
    asx_http_parse_error err;
} reject_vector;

#define RV(s, e) {(s), (uint32_t)(sizeof(s) - 1u), (e)}
#define RQ "POST / HTTP/1.1\r\nHost: a\r\n"
#define CH RQ "Transfer-Encoding: chunked\r\n\r\n"

static const reject_vector g_rejects[] = {
    /* Content-Length / Transfer-Encoding ambiguity (RFC 9112 section 6.3). */
    RV(RQ "Content-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\nhello",
       ASX_HTTP_PERR_CONTENT_LENGTH_AND_CHUNKED),
    RV(RQ "Transfer-Encoding: chunked\r\nContent-Length: 5\r\n\r\n",
       ASX_HTTP_PERR_CONTENT_LENGTH_AND_CHUNKED),
    RV(RQ "Content-Length: 5\r\nContent-Length: 5\r\n\r\nhello",
       ASX_HTTP_PERR_DUPLICATE_CONTENT_LENGTH),
    RV(RQ "Content-Length: 5\r\nContent-Length: 6\r\n\r\nhello",
       ASX_HTTP_PERR_DUPLICATE_CONTENT_LENGTH),
    RV(RQ "Content-Length: 5, 5\r\n\r\nhello", ASX_HTTP_PERR_BAD_CONTENT_LENGTH),
    RV(RQ "Content-Length: +5\r\n\r\nhello", ASX_HTTP_PERR_BAD_CONTENT_LENGTH),
    RV(RQ "Content-Length: -1\r\n\r\n", ASX_HTTP_PERR_BAD_CONTENT_LENGTH),
    RV(RQ "Content-Length: 5 5\r\n\r\n", ASX_HTTP_PERR_BAD_CONTENT_LENGTH),
    RV(RQ "Content-Length: 0x10\r\n\r\n", ASX_HTTP_PERR_BAD_CONTENT_LENGTH),
    RV(RQ "Content-Length: 99999999999999999999999\r\n\r\n", ASX_HTTP_PERR_BAD_CONTENT_LENGTH),
    RV(RQ "Content-Length: \r\n\r\n", ASX_HTTP_PERR_BAD_CONTENT_LENGTH),
    RV(RQ "Transfer-Encoding: gzip\r\n\r\n", ASX_HTTP_PERR_BAD_TRANSFER_ENCODING),
    RV(RQ "Transfer-Encoding: chunked, gzip\r\n\r\n", ASX_HTTP_PERR_BAD_TRANSFER_ENCODING),
    RV(RQ "Transfer-Encoding: chunked, chunked\r\n\r\n", ASX_HTTP_PERR_BAD_TRANSFER_ENCODING),
    RV(RQ "Transfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n",
       ASX_HTTP_PERR_BAD_TRANSFER_ENCODING),
    RV(RQ "Transfer-Encoding: \r\n\r\n", ASX_HTTP_PERR_BAD_TRANSFER_ENCODING),
    RV(RQ "Transfer-Encoding: chunked;q=1\r\n\r\n", ASX_HTTP_PERR_BAD_TRANSFER_ENCODING),
    RV(RQ "Transfer-Encoding: gzip, chunked\r\n\r\n", ASX_HTTP_PERR_UNSUPPORTED_TRANSFER_CODING),
    RV("POST / HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
       ASX_HTTP_PERR_BAD_TRANSFER_ENCODING),
    /* Line folding and line endings. */
    RV("GET / HTTP/1.1\r\nHost: a\r\nX-Folded: a\r\n b\r\n\r\n", ASX_HTTP_PERR_OBS_FOLD),
    RV("GET / HTTP/1.1\r\n Host: a\r\n\r\n", ASX_HTTP_PERR_OBS_FOLD),
    RV("GET / HTTP/1.1\r\nHost: a\r\n\tX: b\r\n\r\n", ASX_HTTP_PERR_OBS_FOLD),
    RV("GET / HTTP/1.1\nHost: a\r\n\r\n", ASX_HTTP_PERR_BAD_LINE_ENDING),
    RV("GET / HTTP/1.1\r\nHost: a\n\r\n", ASX_HTTP_PERR_BAD_LINE_ENDING),
    RV("GET / HTTP/1.1\r\nHost: a\r\n\n", ASX_HTTP_PERR_BAD_LINE_ENDING),
    RV("GET / HTTP/1.1\r\nHost: a\rX: b\r\n\r\n", ASX_HTTP_PERR_BAD_LINE_ENDING),
    RV("GET / HTTP/1.1\r\r\nHost: a\r\n\r\n", ASX_HTTP_PERR_BAD_LINE_ENDING),
    RV("\nGET / HTTP/1.1\r\nHost: a\r\n\r\n", ASX_HTTP_PERR_BAD_LINE_ENDING),
    /* Field syntax. */
    RV("GET / HTTP/1.1\r\nHost : a\r\n\r\n", ASX_HTTP_PERR_BAD_HEADER_NAME),
    RV("GET / HTTP/1.1\r\nHost: a\r\n: b\r\n\r\n", ASX_HTTP_PERR_BAD_HEADER_NAME),
    RV("GET / HTTP/1.1\r\nHost: a\r\nNoColon\r\n\r\n", ASX_HTTP_PERR_BAD_HEADER_NAME),
    RV("GET / HTTP/1.1\r\nHost: a\r\nX(y): b\r\n\r\n", ASX_HTTP_PERR_BAD_HEADER_NAME),
    RV("GET / HTTP/1.1\r\nHost: a\r\nX: a\0b\r\n\r\n", ASX_HTTP_PERR_BAD_HEADER_VALUE),
    RV("GET / HTTP/1.1\r\nHost: a\r\nX: a\x01"
       "b\r\n\r\n",
       ASX_HTTP_PERR_BAD_HEADER_VALUE),
    RV("GET / HTTP/1.1\r\nHost: a\r\nX: a\x7f\r\n\r\n", ASX_HTTP_PERR_BAD_HEADER_VALUE),
    RV("GET / HTTP/1.1\r\n\r\n", ASX_HTTP_PERR_MISSING_HOST),
    RV("GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", ASX_HTTP_PERR_DUPLICATE_HOST),
    /* Request line. */
    RV("BREW /pot HTTP/1.1\r\nHost: a\r\n\r\n", ASX_HTTP_PERR_UNKNOWN_METHOD),
    RV("get / HTTP/1.1\r\nHost: a\r\n\r\n", ASX_HTTP_PERR_UNKNOWN_METHOD),
    RV("GE(T / HTTP/1.1\r\nHost: a\r\n\r\n", ASX_HTTP_PERR_BAD_START_LINE),
    RV("GET  / HTTP/1.1\r\nHost: a\r\n\r\n", ASX_HTTP_PERR_BAD_START_LINE),
    RV("GET /\r\nHost: a\r\n\r\n", ASX_HTTP_PERR_BAD_START_LINE),
    RV("GET / HTTP/1.1 \r\nHost: a\r\n\r\n", ASX_HTTP_PERR_BAD_VERSION),
    RV("GET / http/1.1\r\nHost: a\r\n\r\n", ASX_HTTP_PERR_BAD_VERSION),
    RV("GET / HTTP/1.1x\r\nHost: a\r\n\r\n", ASX_HTTP_PERR_BAD_VERSION),
    RV("GET / HTTP/2.0\r\nHost: a\r\n\r\n", ASX_HTTP_PERR_UNSUPPORTED_VERSION),
    RV("GET / HTTP/1.2\r\nHost: a\r\n\r\n", ASX_HTTP_PERR_UNSUPPORTED_VERSION),
    RV("GET /a\x7f HTTP/1.1\r\nHost: a\r\n\r\n", ASX_HTTP_PERR_BAD_TARGET),
    RV("GET /\xc3\xa9 HTTP/1.1\r\nHost: a\r\n\r\n", ASX_HTTP_PERR_BAD_TARGET),
    RV("GET a HTTP/1.1\r\nHost: a\r\n\r\n", ASX_HTTP_PERR_BAD_TARGET),
    RV("GET * HTTP/1.1\r\nHost: a\r\n\r\n", ASX_HTTP_PERR_BAD_TARGET),
    RV("CONNECT /x HTTP/1.1\r\nHost: a\r\n\r\n", ASX_HTTP_PERR_BAD_TARGET),
    /* Chunk-size line. */
    RV(CH "zz\r\n", ASX_HTTP_PERR_BAD_CHUNK_SIZE),
    RV(CH " 5\r\nhello\r\n0\r\n\r\n", ASX_HTTP_PERR_BAD_CHUNK_SIZE),
    RV(CH "0x5\r\nhello\r\n0\r\n\r\n", ASX_HTTP_PERR_BAD_CHUNK_SIZE),
    RV(CH "+5\r\nhello\r\n0\r\n\r\n", ASX_HTTP_PERR_BAD_CHUNK_SIZE),
    RV(CH "-5\r\n", ASX_HTTP_PERR_BAD_CHUNK_SIZE),
    RV(CH ";a\r\n", ASX_HTTP_PERR_BAD_CHUNK_SIZE),
    RV(CH "\r\n", ASX_HTTP_PERR_BAD_CHUNK_SIZE),
    RV(CH "10000000000000000\r\n", ASX_HTTP_PERR_CHUNK_SIZE_OVERFLOW),
    RV(CH "fffffffffffffffff\r\n", ASX_HTTP_PERR_CHUNK_SIZE_OVERFLOW),
    RV(CH "5 \r\nhello\r\n0\r\n\r\n", ASX_HTTP_PERR_BAD_CHUNK_EXTENSION),
    RV(CH "5;\r\nhello\r\n", ASX_HTTP_PERR_BAD_CHUNK_EXTENSION),
    RV(CH "5;a=\r\nhello\r\n", ASX_HTTP_PERR_BAD_CHUNK_EXTENSION),
    RV(CH "5;a=\"x\r\nhello\r\n", ASX_HTTP_PERR_BAD_CHUNK_EXTENSION),
    RV(CH "5;a\x01\r\nhello\r\n", ASX_HTTP_PERR_BAD_CHUNK_EXTENSION),
    RV(CH "5;a=b c\r\nhello\r\n", ASX_HTTP_PERR_BAD_CHUNK_EXTENSION),
    RV(CH "5\nhello\r\n0\r\n\r\n", ASX_HTTP_PERR_BAD_LINE_ENDING),
    RV(CH "5;a\nhello\r\n", ASX_HTTP_PERR_BAD_LINE_ENDING),
    /* Chunk data terminator. */
    RV(CH "5\r\nhelloX\r\n0\r\n\r\n", ASX_HTTP_PERR_BAD_CHUNK_TERMINATOR),
    RV(CH "5\r\nhello\n0\r\n\r\n", ASX_HTTP_PERR_BAD_CHUNK_TERMINATOR),
    RV(CH "5\r\nhello\r\r\n", ASX_HTTP_PERR_BAD_CHUNK_TERMINATOR),
    /* Trailer section. */
    RV(CH "0\r\nContent-Length: 5\r\n\r\n", ASX_HTTP_PERR_BAD_TRAILER),
    RV(CH "0\r\nTransfer-Encoding: chunked\r\n\r\n", ASX_HTTP_PERR_BAD_TRAILER),
    RV(CH "0\r\nhost: evil\r\n\r\n", ASX_HTTP_PERR_BAD_TRAILER),
    RV(CH "0\r\nX-A: 1\r\n folded\r\n\r\n", ASX_HTTP_PERR_OBS_FOLD),
    RV(CH "0\r\nX-A: 1\n\r\n", ASX_HTTP_PERR_BAD_LINE_ENDING),
};

TEST(smuggling_and_malformed_rejections) {
    uint32_t i;

    for (i = 0u; i < (uint32_t)(sizeof(g_rejects) / sizeof(g_rejects[0])); i++) {
        const reject_vector *v = &g_rejects[i];
        asx_status bulk;
        asx_status bytewise;
        asx_status again;
        asx_http_event ev;
        uint32_t used = 0u;

        bulk = parse_with(ASX_HTTP_PARSE_REQUEST, NULL, v->msg, v->len, 0u);
        if (bulk == ASX_OK || asx_http_parser_error(&g_parser) != v->err) {
            fprintf(stderr, "    reject vector %u: status=%d err=%s (want %s)\n", i, (int)bulk,
                    asx_http_parse_error_str(asx_http_parser_error(&g_parser)),
                    asx_http_parse_error_str(v->err));
        }
        ASSERT_NE(bulk, ASX_OK);
        ASSERT_EQ(asx_http_parser_error(&g_parser), v->err);
        ASSERT_EQ(g_cap.completes, 0u);
        /* Errors are sticky. */
        again = asx_http_parser_feed(&g_parser, "x", 1u, &used, &ev);
        ASSERT_EQ(again, bulk);
        ASSERT_EQ(used, 0u);

        bytewise = parse_with(ASX_HTTP_PARSE_REQUEST, NULL, v->msg, v->len, 1u);
        ASSERT_EQ(bytewise, bulk);
        ASSERT_EQ(asx_http_parser_error(&g_parser), v->err);
    }
}

TEST(rejection_status_mapping_examples) {
    ASSERT_EQ(parse_with(ASX_HTTP_PARSE_REQUEST, NULL,
                         LIT(RQ "Content-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n"), 0u),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_http_parse_error_status(asx_http_parser_error(&g_parser)), 400u);
    ASSERT_EQ(parse_with(ASX_HTTP_PARSE_REQUEST, NULL, LIT("BREW / HTTP/1.1\r\nHost: a\r\n\r\n"),
                         0u),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_http_parse_error_status(asx_http_parser_error(&g_parser)), 501u);
    ASSERT_EQ(parse_with(ASX_HTTP_PARSE_REQUEST, NULL, LIT("GET / HTTP/3.0\r\nHost: a\r\n\r\n"),
                         0u),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_http_parse_error_status(asx_http_parser_error(&g_parser)), 505u);

    /* reset clears the sticky error. */
    asx_http_parser_reset(&g_parser);
    ASSERT_EQ(asx_http_parser_error(&g_parser), ASX_HTTP_PERR_NONE);
    memset(&g_cap, 0, sizeof(g_cap));
    ASSERT_EQ(feed_steps(&g_parser, LIT("GET / HTTP/1.1\r\nHost: a\r\n\r\n"), 0u, &g_cap), ASX_OK);
    ASSERT_EQ(g_cap.completes, 1u);
}

/* ------------------------------------------------------------------ */
/* Limits                                                              */
/* ------------------------------------------------------------------ */

TEST(limit_violations_fail_closed) {
    asx_http_limits limits;
    static uint8_t tiny_head[48];
    uint32_t used = 0u;
    asx_http_event ev;

    /* Start line (414). */
    asx_http_limits_init(&limits);
    limits.max_start_line = 32u;
    ASSERT_EQ(parse_with(ASX_HTTP_PARSE_REQUEST, &limits,
                         LIT("GET /aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa HTTP/1.1\r\n"), 0u),
              ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(asx_http_parser_error(&g_parser), ASX_HTTP_PERR_START_LINE_TOO_LONG);
    ASSERT_EQ(asx_http_parse_error_status(asx_http_parser_error(&g_parser)), 414u);
    /* Exactly at the limit (32 bytes) is accepted. */
    ASSERT_EQ(parse_with(ASX_HTTP_PARSE_REQUEST, &limits,
                         LIT("GET /aaaaaaaaaaaaaaaaaa HTTP/1.1\r\nHost: a\r\n\r\n"), 0u),
              ASX_OK);

    /* Header count (431). */
    asx_http_limits_init(&limits);
    limits.max_header_count = 3u;
    ASSERT_EQ(parse_with(ASX_HTTP_PARSE_REQUEST, &limits,
                         LIT("GET / HTTP/1.1\r\nHost: a\r\nA: 1\r\nB: 2\r\nC: 3\r\n\r\n"), 0u),
              ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(asx_http_parser_error(&g_parser), ASX_HTTP_PERR_TOO_MANY_HEADERS);
    ASSERT_EQ(asx_http_parse_error_status(asx_http_parser_error(&g_parser)), 431u);

    /* Header-section bytes (431). */
    asx_http_limits_init(&limits);
    limits.max_header_bytes = 64u;
    ASSERT_EQ(parse_with(ASX_HTTP_PARSE_REQUEST, &limits,
                         LIT("GET / HTTP/1.1\r\nHost: a\r\nX-Long: "
                             "0123456789012345678901234567890123456789012345678901234567890123"
                             "\r\n\r\n"),
                         0u),
              ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(asx_http_parser_error(&g_parser), ASX_HTTP_PERR_HEADERS_TOO_LARGE);

    /* Caller head buffer exhausted (431 / 414). */
    ASSERT_EQ(asx_http_parser_init(&g_parser, ASX_HTTP_PARSE_REQUEST, NULL, tiny_head,
                                   (uint32_t)sizeof(tiny_head)),
              ASX_OK);
    memset(&g_cap, 0, sizeof(g_cap));
    ASSERT_EQ(feed_steps(&g_parser,
                         LIT("GET / HTTP/1.1\r\nHost: a\r\nX-Pad: "
                             "01234567890123456789012345678901234567890123456789\r\n\r\n"),
                         0u, &g_cap),
              ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(asx_http_parser_error(&g_parser), ASX_HTTP_PERR_HEADERS_TOO_LARGE);
    ASSERT_EQ(asx_http_parser_init(&g_parser, ASX_HTTP_PARSE_REQUEST, NULL, tiny_head,
                                   (uint32_t)sizeof(tiny_head)),
              ASX_OK);
    ASSERT_EQ(feed_steps(&g_parser,
                         LIT("GET /0123456789012345678901234567890123456789012345 HTTP/1.1\r\n"),
                         0u, &g_cap),
              ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(asx_http_parser_error(&g_parser), ASX_HTTP_PERR_START_LINE_TOO_LONG);

    /* Declared body too large: rejected at the head, before any body byte (413). */
    asx_http_limits_init(&limits);
    limits.max_body = 10u;
    ASSERT_EQ(asx_http_parser_init(&g_parser, ASX_HTTP_PARSE_REQUEST, &limits, g_head,
                                   (uint32_t)sizeof(g_head)),
              ASX_OK);
    ASSERT_EQ(asx_http_parser_feed(&g_parser, LIT(RQ "Content-Length: 11\r\n\r\n"), &used, &ev),
              ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(asx_http_parser_error(&g_parser), ASX_HTTP_PERR_BODY_TOO_LARGE);
    ASSERT_EQ(asx_http_parse_error_status(asx_http_parser_error(&g_parser)), 413u);
    ASSERT_EQ(parse_with(ASX_HTTP_PARSE_REQUEST, &limits,
                         LIT(RQ "Content-Length: 10\r\n\r\n0123456789"), 0u),
              ASX_OK);

    /* Chunked body accumulating past the limit (413). */
    ASSERT_EQ(parse_with(ASX_HTTP_PARSE_REQUEST, &limits,
                         LIT(CH "6\r\nabcdef\r\n6\r\nghijkl\r\n0\r\n\r\n"), 0u),
              ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(asx_http_parser_error(&g_parser), ASX_HTTP_PERR_BODY_TOO_LARGE);

    /* Close-delimited response past the limit (413). */
    ASSERT_EQ(parse_with(ASX_HTTP_PARSE_RESPONSE, &limits,
                         LIT("HTTP/1.1 200 OK\r\n\r\n0123456789A"), 0u),
              ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(asx_http_parser_error(&g_parser), ASX_HTTP_PERR_BODY_TOO_LARGE);

    /* Chunk-size line length. */
    asx_http_limits_init(&limits);
    limits.max_chunk_line = 8u;
    ASSERT_EQ(parse_with(ASX_HTTP_PARSE_REQUEST, &limits,
                         LIT(CH "5;aaaaaaaaaa=b\r\nhello\r\n0\r\n\r\n"), 0u),
              ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(asx_http_parser_error(&g_parser), ASX_HTTP_PERR_CHUNK_LINE_TOO_LONG);

    /* Trailer count shares the header-count budget. */
    asx_http_limits_init(&limits);
    limits.max_header_count = 2u;
    ASSERT_EQ(parse_with(ASX_HTTP_PARSE_REQUEST, &limits,
                         LIT(CH "0\r\nX-A: 1\r\nX-B: 2\r\nX-C: 3\r\n\r\n"), 0u),
              ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(asx_http_parser_error(&g_parser), ASX_HTTP_PERR_TOO_MANY_HEADERS);
}

TEST(parser_argument_validation) {
    static uint8_t small[8];
    uint32_t used = 7u;
    asx_http_event ev;

    ASSERT_EQ(asx_http_parser_init(NULL, ASX_HTTP_PARSE_REQUEST, NULL, g_head, 64u),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_http_parser_init(&g_parser, ASX_HTTP_PARSE_REQUEST, NULL, NULL, 64u),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_http_parser_init(&g_parser, ASX_HTTP_PARSE_REQUEST, NULL, small,
                                   (uint32_t)sizeof(small)),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_http_parser_init(&g_parser, ASX_HTTP_PARSE_REQUEST, NULL, g_head,
                                   (uint32_t)sizeof(g_head)),
              ASX_OK);
    ASSERT_EQ(asx_http_parser_feed(&g_parser, NULL, 3u, &used, &ev), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(used, 0u);
    ASSERT_EQ(asx_http_parser_feed(&g_parser, NULL, 0u, &used, &ev), ASX_OK);
    ASSERT_EQ(ev.kind, ASX_HTTP_EVENT_NONE);
    ASSERT_STR_EQ(asx_http_parser_target(&g_parser), "");
    ASSERT_EQ(asx_http_parser_header_count(&g_parser), 0u);
}

/* ------------------------------------------------------------------ */
/* Response parsing                                                    */
/* ------------------------------------------------------------------ */

TEST(parse_response_content_length_and_pipelined_204) {
    ASSERT_EQ(parse_with(ASX_HTTP_PARSE_RESPONSE, NULL,
                         LIT("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nX-A: b\r\n\r\nhello"), 1u),
              ASX_OK);
    ASSERT_EQ(g_cap.completes, 1u);
    ASSERT_EQ(g_cap.status[0], 200u);
    ASSERT_STR_EQ(g_cap.text[0], "OK");
    ASSERT_TRUE(cap_body_is(0u, "hello"));
    ASSERT_EQ(asx_http_parser_keep_alive(&g_parser), 1);

    /* 204 never has a body, even with a (metadata) Content-Length. */
    ASSERT_EQ(parse_with(ASX_HTTP_PARSE_RESPONSE, NULL,
                         LIT("HTTP/1.1 204 No Content\r\nContent-Length: 5\r\n\r\n"
                             "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi"),
                         0u),
              ASX_OK);
    ASSERT_EQ(g_cap.heads, 2u);
    ASSERT_EQ(g_cap.completes, 2u);
    ASSERT_EQ(g_cap.status[0], 204u);
    ASSERT_EQ(g_cap.body_len[0], 0u);
    ASSERT_TRUE(cap_body_is(1u, "hi"));

    /* 304 with Content-Length metadata completes immediately. */
    ASSERT_EQ(parse_with(ASX_HTTP_PARSE_RESPONSE, NULL,
                         LIT("HTTP/1.1 304 Not Modified\r\nContent-Length: 100\r\n\r\n"), 0u),
              ASX_OK);
    ASSERT_EQ(g_cap.completes, 1u);
    ASSERT_EQ(asx_http_parser_content_length(&g_parser), 100u);
    ASSERT_EQ(asx_http_parser_body_framing(&g_parser), ASX_HTTP_BODY_FRAMING_NONE);
}

TEST(parse_response_to_head_has_no_body) {
    memset(&g_cap, 0, sizeof(g_cap));
    ASSERT_EQ(asx_http_parser_init(&g_parser, ASX_HTTP_PARSE_RESPONSE, NULL, g_head,
                                   (uint32_t)sizeof(g_head)),
              ASX_OK);
    asx_http_parser_set_request_method(&g_parser, ASX_HTTP_HEAD);
    ASSERT_EQ(feed_steps(&g_parser,
                         LIT("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n"
                             "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"),
                         0u, &g_cap),
              ASX_OK);
    ASSERT_EQ(g_cap.completes, 2u);
    ASSERT_EQ(g_cap.body_events, 0u);
    ASSERT_EQ(asx_http_parser_keep_alive(&g_parser), 1);
}

TEST(parse_response_close_delimited_and_eof) {
    asx_http_event ev;

    ASSERT_EQ(parse_with(ASX_HTTP_PARSE_RESPONSE, NULL,
                         LIT("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nstreaming "), 1u),
              ASX_OK);
    ASSERT_EQ(feed_steps(&g_parser, LIT("until close"), 0u, &g_cap), ASX_OK);
    ASSERT_EQ(g_cap.completes, 0u);
    ASSERT_EQ(asx_http_parser_body_framing(&g_parser), ASX_HTTP_BODY_FRAMING_CLOSE);
    ASSERT_EQ(asx_http_parser_keep_alive(&g_parser), 0);
    ASSERT_EQ(asx_http_parser_finish(&g_parser, &ev), ASX_OK);
    ASSERT_EQ(ev.kind, ASX_HTTP_EVENT_MESSAGE_COMPLETE);
    ASSERT_TRUE(cap_body_is(0u, "streaming until close"));
    ASSERT_EQ(asx_http_parser_finish(&g_parser, &ev), ASX_OK);
    ASSERT_EQ(ev.kind, ASX_HTTP_EVENT_NONE);
    {
        uint32_t used = 0u;
        ASSERT_EQ(asx_http_parser_feed(&g_parser, "x", 1u, &used, &ev), ASX_E_INVALID_STATE);
    }

    /* A non-chunked final transfer coding is close-delimited too. */
    ASSERT_EQ(parse_with(ASX_HTTP_PARSE_RESPONSE, NULL,
                         LIT("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\nzz"), 0u),
              ASX_OK);
    ASSERT_EQ(asx_http_parser_body_framing(&g_parser), ASX_HTTP_BODY_FRAMING_CLOSE);
    ASSERT_EQ(parse_with(ASX_HTTP_PARSE_RESPONSE, NULL,
                         LIT("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n"
                             "2\r\nzz\r\n0\r\n\r\n"),
                         0u),
              ASX_OK);
    ASSERT_EQ(asx_http_parser_body_framing(&g_parser), ASX_HTTP_BODY_FRAMING_CHUNKED);
    ASSERT_EQ(g_cap.completes, 1u);

    /* Truncation: EOF mid-body and mid-head are errors; between messages it is clean. */
    ASSERT_EQ(parse_with(ASX_HTTP_PARSE_RESPONSE, NULL,
                         LIT("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabcd"), 0u),
              ASX_OK);
    ASSERT_EQ(asx_http_parser_finish(&g_parser, &ev), ASX_E_DISCONNECTED);
    ASSERT_EQ(asx_http_parser_error(&g_parser), ASX_HTTP_PERR_UNEXPECTED_EOF);
    ASSERT_EQ(parse_with(ASX_HTTP_PARSE_RESPONSE, NULL, LIT("HTTP/1.1 200 OK\r\nX-A"), 0u), ASX_OK);
    ASSERT_EQ(asx_http_parser_finish(&g_parser, &ev), ASX_E_DISCONNECTED);
    ASSERT_EQ(parse_with(ASX_HTTP_PARSE_RESPONSE, NULL,
                         LIT("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n"), 0u),
              ASX_OK);
    ASSERT_EQ(asx_http_parser_finish(&g_parser, &ev), ASX_OK);
    ASSERT_EQ(ev.kind, ASX_HTTP_EVENT_NONE);
}

TEST(parse_response_versions_and_status_lines) {
    ASSERT_EQ(parse_with(ASX_HTTP_PARSE_RESPONSE, NULL,
                         LIT("HTTP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n"), 0u),
              ASX_OK);
    ASSERT_EQ(asx_http_parser_keep_alive(&g_parser), 0);
    ASSERT_EQ(parse_with(ASX_HTTP_PARSE_RESPONSE, NULL,
                         LIT("HTTP/1.0 200 OK\r\nConnection: keep-alive\r\nContent-Length: 0\r\n"
                             "\r\n"),
                         0u),
              ASX_OK);
    ASSERT_EQ(asx_http_parser_keep_alive(&g_parser), 1);
    ASSERT_EQ(parse_with(ASX_HTTP_PARSE_RESPONSE, NULL,
                         LIT("HTTP/1.1 200\r\nContent-Length: 0\r\n\r\n"), 0u),
              ASX_OK);
    ASSERT_STR_EQ(asx_http_parser_reason(&g_parser), "");
    ASSERT_EQ(parse_with(ASX_HTTP_PARSE_RESPONSE, NULL,
                         LIT("HTTP/1.1 599 Caf\xc3\xa9 \t Odd\r\nContent-Length: 0\r\n\r\n"), 0u),
              ASX_OK);
    ASSERT_EQ(asx_http_parser_status(&g_parser), 599u);

    ASSERT_EQ(parse_with(ASX_HTTP_PARSE_RESPONSE, NULL, LIT("HTTP/1.1 20 OK\r\n\r\n"), 0u),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_http_parser_error(&g_parser), ASX_HTTP_PERR_BAD_STATUS);
    ASSERT_EQ(parse_with(ASX_HTTP_PARSE_RESPONSE, NULL, LIT("HTTP/1.1 600 X\r\n\r\n"), 0u),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_http_parser_error(&g_parser), ASX_HTTP_PERR_BAD_STATUS);
    ASSERT_EQ(parse_with(ASX_HTTP_PARSE_RESPONSE, NULL, LIT("HTTP/1.1 200OK\r\n\r\n"), 0u),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(parse_with(ASX_HTTP_PARSE_RESPONSE, NULL, LIT("HTTP/2 200 OK\r\n\r\n"), 0u),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(parse_with(ASX_HTTP_PARSE_RESPONSE, NULL, LIT("HTTP/1.1 200 O\x01K\r\n\r\n"), 0u),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(parse_with(ASX_HTTP_PARSE_RESPONSE, NULL,
                         LIT("HTTP/1.0 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"), 0u),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_http_parser_error(&g_parser), ASX_HTTP_PERR_BAD_TRANSFER_ENCODING);
    ASSERT_EQ(parse_with(ASX_HTTP_PARSE_RESPONSE, NULL,
                         LIT("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
                             "Transfer-Encoding: chunked\r\n\r\n"),
                         0u),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_http_parser_error(&g_parser), ASX_HTTP_PERR_CONTENT_LENGTH_AND_CHUNKED);
}

TEST(parse_response_informational_and_upgrade) {
    uint32_t used = 0u;
    asx_http_event ev;

    ASSERT_EQ(parse_with(ASX_HTTP_PARSE_RESPONSE, NULL,
                         LIT("HTTP/1.1 100 Continue\r\n\r\n"
                             "HTTP/1.1 103 Early Hints\r\nLink: </s.css>\r\n\r\n"
                             "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"),
                         1u),
              ASX_OK);
    ASSERT_EQ(g_cap.heads, 3u);
    ASSERT_EQ(g_cap.completes, 3u);
    ASSERT_EQ(g_cap.status[0], 100u);
    ASSERT_EQ(g_cap.status[1], 103u);
    ASSERT_EQ(g_cap.status[2], 200u);
    ASSERT_TRUE(cap_body_is(2u, "ok"));

    /* 101: bytes after the head belong to the new protocol. */
    ASSERT_EQ(parse_with(ASX_HTTP_PARSE_RESPONSE, NULL,
                         LIT("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                             "Connection: Upgrade\r\n\r\n"),
                         0u),
              ASX_OK);
    ASSERT_EQ(g_cap.completes, 1u);
    ASSERT_TRUE(asx_http_parser_is_upgraded(&g_parser));
    ASSERT_EQ(asx_http_parser_keep_alive(&g_parser), 0);
    ASSERT_EQ(asx_http_parser_feed(&g_parser, "\x81\x05", 2u, &used, &ev), ASX_E_INVALID_STATE);
    ASSERT_EQ(used, 0u);

    /* 2xx to CONNECT switches to a tunnel. */
    memset(&g_cap, 0, sizeof(g_cap));
    ASSERT_EQ(asx_http_parser_init(&g_parser, ASX_HTTP_PARSE_RESPONSE, NULL, g_head,
                                   (uint32_t)sizeof(g_head)),
              ASX_OK);
    asx_http_parser_set_request_method(&g_parser, ASX_HTTP_CONNECT);
    ASSERT_EQ(feed_steps(&g_parser, LIT("HTTP/1.1 200 Connection Established\r\n\r\n"), 0u, &g_cap),
              ASX_OK);
    ASSERT_TRUE(asx_http_parser_is_upgraded(&g_parser));
}

/* ------------------------------------------------------------------ */
/* Convenience collection path                                         */
/* ------------------------------------------------------------------ */

static asx_http_request g_req;
static asx_http_request g_req2;
static asx_http_response g_resp;
static uint8_t g_wire[16384];

TEST(collect_request_pipelined_and_partial) {
    static const char msg[] = "POST /one HTTP/1.1\r\nHost: a\r\nContent-Length: 3\r\n\r\nabc"
                              "GET /two?q=1 HTTP/1.1\r\nHost: a\r\nX-K: v\r\n\r\n";
    uint32_t consumed = 0u;
    uint32_t first_len = 0u;
    int complete = 0;

    ASSERT_EQ(asx_http_parser_init(&g_parser, ASX_HTTP_PARSE_REQUEST, NULL, g_head,
                                   (uint32_t)sizeof(g_head)),
              ASX_OK);
    /* Partial: half the first request. */
    ASSERT_EQ(asx_http_parser_collect_request(&g_parser, msg, 20u, &consumed, &g_req, &complete),
              ASX_OK);
    ASSERT_EQ(complete, 0);
    ASSERT_EQ(consumed, 20u);
    ASSERT_EQ(asx_http_parser_collect_request(&g_parser, msg + 20, (uint32_t)sizeof(msg) - 21u,
                                              &consumed, &g_req, &complete),
              ASX_OK);
    ASSERT_EQ(complete, 1);
    first_len = 20u + consumed;
    ASSERT_EQ(first_len, (uint32_t)strlen("POST /one HTTP/1.1\r\nHost: a\r\nContent-Length: "
                                          "3\r\n\r\nabc"));
    ASSERT_EQ(g_req.method, ASX_HTTP_POST);
    ASSERT_STR_EQ(g_req.uri, "/one");
    ASSERT_EQ(g_req.body.len, 3u);
    ASSERT_TRUE(memcmp(g_req.body.data, "abc", 3u) == 0);

    ASSERT_EQ(asx_http_parser_collect_request(&g_parser, msg + first_len,
                                              (uint32_t)sizeof(msg) - 1u - first_len, &consumed,
                                              &g_req2, &complete),
              ASX_OK);
    ASSERT_EQ(complete, 1);
    ASSERT_EQ(g_req2.method, ASX_HTTP_GET);
    ASSERT_STR_EQ(g_req2.uri, "/two?q=1");
    ASSERT_STR_EQ(asx_http_headers_get(&g_req2.headers, "x-k"), "v");
    ASSERT_TRUE(asx_http_body_is_empty(&g_req2.body));
}

TEST(collect_request_struct_capacity_fails_closed) {
    uint32_t consumed = 0u;
    uint32_t len;
    uint32_t i;
    int complete = 0;
    int n;

    /* Target longer than ASX_HTTP_URI_MAX -> 414. */
    n = snprintf((char *)g_wire, sizeof(g_wire), "GET /");
    for (i = 0u; i < ASX_HTTP_URI_MAX + 10u; i++) g_wire[(uint32_t)n + i] = 'a';
    len = (uint32_t)n + i;
    n = snprintf((char *)g_wire + len, sizeof(g_wire) - len, " HTTP/1.1\r\nHost: a\r\n\r\n");
    len += (uint32_t)n;
    ASSERT_EQ(asx_http_parser_init(&g_parser, ASX_HTTP_PARSE_REQUEST, NULL, g_head,
                                   (uint32_t)sizeof(g_head)),
              ASX_OK);
    ASSERT_EQ(asx_http_parser_collect_request(&g_parser, g_wire, len, &consumed, &g_req, &complete),
              ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(asx_http_parser_error(&g_parser), ASX_HTTP_PERR_START_LINE_TOO_LONG);

    /* More fields than asx_http_headers holds -> 431. */
    len = (uint32_t)snprintf((char *)g_wire, sizeof(g_wire), "GET / HTTP/1.1\r\nHost: a\r\n");
    for (i = 0u; i < ASX_HTTP_MAX_HEADERS + 2u; i++) {
        len += (uint32_t)snprintf((char *)g_wire + len, sizeof(g_wire) - len, "X-%u: v\r\n",
                                  (unsigned)i);
    }
    len += (uint32_t)snprintf((char *)g_wire + len, sizeof(g_wire) - len, "\r\n");
    ASSERT_EQ(asx_http_parser_init(&g_parser, ASX_HTTP_PARSE_REQUEST, NULL, g_head,
                                   (uint32_t)sizeof(g_head)),
              ASX_OK);
    ASSERT_EQ(asx_http_parser_collect_request(&g_parser, g_wire, len, &consumed, &g_req, &complete),
              ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(asx_http_parser_error(&g_parser), ASX_HTTP_PERR_HEADERS_TOO_LARGE);

    /* Inline body larger than ASX_HTTP_BODY_MAX -> 413. */
    len = (uint32_t)snprintf((char *)g_wire, sizeof(g_wire),
                             "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: %u\r\n\r\n",
                             ASX_HTTP_BODY_MAX + 1u);
    memset(g_wire + len, 'b', ASX_HTTP_BODY_MAX + 1u);
    len += ASX_HTTP_BODY_MAX + 1u;
    ASSERT_EQ(asx_http_parser_init(&g_parser, ASX_HTTP_PARSE_REQUEST, NULL, g_head,
                                   (uint32_t)sizeof(g_head)),
              ASX_OK);
    ASSERT_EQ(asx_http_parser_collect_request(&g_parser, g_wire, len, &consumed, &g_req, &complete),
              ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(asx_http_parser_error(&g_parser), ASX_HTTP_PERR_BODY_TOO_LARGE);
    ASSERT_EQ(complete, 0);
}

TEST(collect_response_skips_interim) {
    static const char msg[] =
        "HTTP/1.1 100 Continue\r\n\r\n"
        "HTTP/1.1 201 Created\r\nLocation: /x/1\r\nContent-Length: 2\r\n\r\nok";
    uint32_t consumed = 0u;
    int complete = 0;

    ASSERT_EQ(asx_http_parser_init(&g_parser, ASX_HTTP_PARSE_RESPONSE, NULL, g_head,
                                   (uint32_t)sizeof(g_head)),
              ASX_OK);
    ASSERT_EQ(asx_http_parser_collect_response(&g_parser, LIT(msg), &consumed, &g_resp, &complete),
              ASX_OK);
    ASSERT_EQ(complete, 1);
    ASSERT_EQ(consumed, (uint32_t)sizeof(msg) - 1u);
    ASSERT_EQ(g_resp.status, 201u);
    ASSERT_STR_EQ(asx_http_headers_get(&g_resp.headers, "location"), "/x/1");
    ASSERT_EQ(g_resp.body.len, 2u);
}

/* ------------------------------------------------------------------ */
/* Serializer                                                          */
/* ------------------------------------------------------------------ */

static int wire_contains(const uint8_t *buf, uint32_t len, const char *needle) {
    size_t n = strlen(needle);
    uint32_t i;

    if (n > len) return 0;
    for (i = 0u; i + n <= len; i++) {
        if (memcmp(buf + i, needle, n) == 0) return 1;
    }
    return 0;
}

static asx_status reparse_request(const uint8_t *wire, uint32_t len, asx_http_request *out) {
    uint32_t consumed = 0u;
    int complete = 0;
    asx_status st;

    st = asx_http_parser_init(&g_parser, ASX_HTTP_PARSE_REQUEST, NULL, g_head,
                              (uint32_t)sizeof(g_head));
    if (st != ASX_OK) return st;
    st = asx_http_parser_collect_request(&g_parser, wire, len, &consumed, out, &complete);
    if (st != ASX_OK) return st;
    return (complete && consumed == len) ? ASX_OK : ASX_E_INVALID_STATE;
}

static asx_status reparse_response(const uint8_t *wire, uint32_t len, asx_http_method method,
                                   asx_http_response *out) {
    uint32_t consumed = 0u;
    int complete = 0;
    asx_status st;

    st = asx_http_parser_init(&g_parser, ASX_HTTP_PARSE_RESPONSE, NULL, g_head,
                              (uint32_t)sizeof(g_head));
    if (st != ASX_OK) return st;
    asx_http_parser_set_request_method(&g_parser, method);
    st = asx_http_parser_collect_response(&g_parser, wire, len, &consumed, out, &complete);
    if (st != ASX_OK) return st;
    return (complete && consumed == len) ? ASX_OK : ASX_E_INVALID_STATE;
}

TEST(serialize_request_round_trip_content_length) {
    asx_http_serialize_opts opts;
    uint32_t len = 0u;
    uint32_t i;

    asx_http_request_init(&g_req, ASX_HTTP_PUT, "/api/items?id=7&x=%20");
    ASSERT_EQ(asx_http_headers_add(&g_req.headers, "Host", "example.test:8080"), ASX_OK);
    ASSERT_EQ(asx_http_headers_add(&g_req.headers, "Content-Type", "application/json"), ASX_OK);
    ASSERT_EQ(asx_http_headers_add(&g_req.headers, "X-Trace", "a\tb  c"), ASX_OK);
    /* Caller framing fields are dropped and regenerated (smuggling-safe). */
    ASSERT_EQ(asx_http_headers_add(&g_req.headers, "Content-Length", "999"), ASX_OK);
    ASSERT_EQ(asx_http_headers_add(&g_req.headers, "Transfer-Encoding", "chunked"), ASX_OK);
    ASSERT_EQ(asx_http_body_set_bytes(&g_req.body, "{\"a\":1}", 7u), ASX_OK);
    asx_http_serialize_opts_init(&opts);
    opts.connection = ASX_HTTP_CONNECTION_CLOSE;
    ASSERT_EQ(asx_http_serialize_request(&g_req, &opts, g_wire, sizeof(g_wire), &len), ASX_OK);
    ASSERT_TRUE(wire_contains(g_wire, len, "PUT /api/items?id=7&x=%20 HTTP/1.1\r\n"));
    ASSERT_TRUE(wire_contains(g_wire, len, "\r\nContent-Length: 7\r\n"));
    ASSERT_TRUE(wire_contains(g_wire, len, "\r\nConnection: close\r\n"));
    ASSERT_FALSE(wire_contains(g_wire, len, "999"));
    ASSERT_FALSE(wire_contains(g_wire, len, "Transfer-Encoding"));

    ASSERT_EQ(reparse_request(g_wire, len, &g_req2), ASX_OK);
    ASSERT_EQ(g_req2.method, g_req.method);
    ASSERT_STR_EQ(g_req2.uri, g_req.uri);
    ASSERT_EQ(g_req2.version, g_req.version);
    for (i = 0u; i < 3u; i++) {
        ASSERT_STR_EQ(asx_http_headers_get(&g_req2.headers, g_req.headers.entries[i].name),
                      g_req.headers.entries[i].value);
    }
    ASSERT_EQ(g_req2.body.len, 7u);
    ASSERT_TRUE(memcmp(g_req2.body.data, "{\"a\":1}", 7u) == 0);
    ASSERT_EQ(asx_http_parser_keep_alive(&g_parser), 0);

    /* GET without a body carries no Content-Length; POST with an empty body sends 0. */
    asx_http_request_init(&g_req, ASX_HTTP_GET, "/");
    ASSERT_EQ(asx_http_headers_add(&g_req.headers, "Host", "h"), ASX_OK);
    ASSERT_EQ(asx_http_serialize_request(&g_req, NULL, g_wire, sizeof(g_wire), &len), ASX_OK);
    ASSERT_EQ(len, (uint32_t)strlen("GET / HTTP/1.1\r\nHost: h\r\n\r\n"));
    ASSERT_TRUE(memcmp(g_wire, "GET / HTTP/1.1\r\nHost: h\r\n\r\n", len) == 0);
    g_req.method = ASX_HTTP_POST;
    ASSERT_EQ(asx_http_serialize_request(&g_req, NULL, g_wire, sizeof(g_wire), &len), ASX_OK);
    ASSERT_TRUE(wire_contains(g_wire, len, "Content-Length: 0\r\n"));
}

TEST(serialize_request_round_trip_chunked_with_trailers) {
    asx_http_serialize_opts opts;
    asx_http_headers trailers;
    const char *name = NULL;
    const char *value = NULL;
    uint32_t len = 0u;

    asx_http_request_init(&g_req, ASX_HTTP_POST, "/upload");
    ASSERT_EQ(asx_http_body_set_bytes(&g_req.body, "chunked payload", 15u), ASX_OK);
    asx_http_headers_init(&trailers);
    ASSERT_EQ(asx_http_headers_add(&trailers, "X-Checksum", "abc123"), ASX_OK);
    asx_http_serialize_opts_init(&opts);
    opts.framing = ASX_HTTP_FRAMING_CHUNKED;
    opts.trailers = &trailers;
    opts.host = "upload.test";
    ASSERT_EQ(asx_http_serialize_request(&g_req, &opts, g_wire, sizeof(g_wire), &len), ASX_OK);
    ASSERT_TRUE(wire_contains(g_wire, len, "Host: upload.test\r\n"));
    ASSERT_TRUE(wire_contains(g_wire, len, "Transfer-Encoding: chunked\r\n"));
    ASSERT_TRUE(wire_contains(g_wire, len,
                              "\r\nf\r\nchunked payload\r\n0\r\n"
                              "X-Checksum: abc123\r\n\r\n"));
    ASSERT_FALSE(wire_contains(g_wire, len, "Content-Length"));

    ASSERT_EQ(reparse_request(g_wire, len, &g_req2), ASX_OK);
    ASSERT_EQ(g_req2.body.len, 15u);
    ASSERT_TRUE(memcmp(g_req2.body.data, "chunked payload", 15u) == 0);
    ASSERT_EQ(asx_http_parser_trailer_count(&g_parser), 1u);
    ASSERT_EQ(asx_http_parser_trailer_at(&g_parser, 0u, &name, &value), ASX_OK);
    ASSERT_STR_EQ(value, "abc123");

    /* Chunked framing is HTTP/1.1-only; forbidden trailers are rejected. */
    g_req.version = ASX_HTTP_VERSION_1_0;
    ASSERT_EQ(asx_http_serialize_request(&g_req, &opts, g_wire, sizeof(g_wire), &len),
              ASX_E_INVALID_ARGUMENT);
    g_req.version = ASX_HTTP_VERSION_1_1;
    ASSERT_EQ(asx_http_headers_add(&trailers, "Content-Length", "3"), ASX_OK);
    ASSERT_EQ(asx_http_serialize_request(&g_req, &opts, g_wire, sizeof(g_wire), &len),
              ASX_E_INVALID_ARGUMENT);
}

TEST(serialize_response_round_trip) {
    asx_http_serialize_opts opts;
    uint32_t len = 0u;

    asx_http_response_init(&g_resp, ASX_HTTP_201_CREATED);
    ASSERT_EQ(asx_http_headers_add(&g_resp.headers, "Location", "/items/9"), ASX_OK);
    ASSERT_EQ(asx_http_headers_add(&g_resp.headers, "Connection", "keep-alive"), ASX_OK);
    ASSERT_EQ(asx_http_body_set_bytes(&g_resp.body, "created", 7u), ASX_OK);
    asx_http_serialize_opts_init(&opts);
    opts.connection = ASX_HTTP_CONNECTION_CLOSE;
    ASSERT_EQ(asx_http_serialize_response(&g_resp, &opts, g_wire, sizeof(g_wire), &len), ASX_OK);
    ASSERT_TRUE(memcmp(g_wire, "HTTP/1.1 201 Created\r\n", 22u) == 0);
    ASSERT_FALSE(wire_contains(g_wire, len, "keep-alive"));
    ASSERT_EQ(reparse_response(g_wire, len, ASX_HTTP_POST, &g_resp), ASX_OK);
    ASSERT_EQ(g_resp.status, 201u);
    ASSERT_STR_EQ(asx_http_parser_reason(&g_parser), "Created");
    ASSERT_STR_EQ(asx_http_headers_get(&g_resp.headers, "Location"), "/items/9");
    ASSERT_STR_EQ(asx_http_headers_get(&g_resp.headers, "Connection"), "close");
    ASSERT_EQ(asx_http_parser_keep_alive(&g_parser), 0);
    ASSERT_EQ(g_resp.body.len, 7u);

    /* Chunked response with trailers, close-delimited response. */
    asx_http_response_init(&g_resp, ASX_HTTP_200_OK);
    ASSERT_EQ(asx_http_body_set_bytes(&g_resp.body, "streamed", 8u), ASX_OK);
    opts.connection = ASX_HTTP_CONNECTION_DEFAULT;
    opts.framing = ASX_HTTP_FRAMING_CHUNKED;
    ASSERT_EQ(asx_http_serialize_response(&g_resp, &opts, g_wire, sizeof(g_wire), &len), ASX_OK);
    ASSERT_EQ(reparse_response(g_wire, len, ASX_HTTP_GET, &g_resp), ASX_OK);
    ASSERT_EQ(g_resp.body.len, 8u);
    ASSERT_EQ(asx_http_parser_keep_alive(&g_parser), 1);

    asx_http_response_init(&g_resp, ASX_HTTP_200_OK);
    ASSERT_EQ(asx_http_body_set_bytes(&g_resp.body, "until-eof", 9u), ASX_OK);
    opts.framing = ASX_HTTP_FRAMING_CLOSE;
    ASSERT_EQ(asx_http_serialize_response(&g_resp, &opts, g_wire, sizeof(g_wire), &len), ASX_OK);
    ASSERT_TRUE(wire_contains(g_wire, len, "Connection: close\r\n"));
    ASSERT_FALSE(wire_contains(g_wire, len, "Content-Length"));
    opts.connection = ASX_HTTP_CONNECTION_KEEP_ALIVE;
    ASSERT_EQ(asx_http_serialize_response(&g_resp, &opts, g_wire, sizeof(g_wire), &len),
              ASX_E_INVALID_ARGUMENT);

    /* HTTP/1.0 keep-alive response and an unregistered status code. */
    asx_http_response_init(&g_resp, 599u);
    g_resp.version = ASX_HTTP_VERSION_1_0;
    asx_http_serialize_opts_init(&opts);
    opts.connection = ASX_HTTP_CONNECTION_KEEP_ALIVE;
    ASSERT_EQ(asx_http_serialize_response(&g_resp, &opts, g_wire, sizeof(g_wire), &len), ASX_OK);
    ASSERT_TRUE(memcmp(g_wire, "HTTP/1.0 599 \r\n", 15u) == 0);
    ASSERT_TRUE(wire_contains(g_wire, len, "Connection: keep-alive\r\n"));
    ASSERT_TRUE(wire_contains(g_wire, len, "Content-Length: 0\r\n"));
}

TEST(serialize_head_and_bodiless_statuses) {
    asx_http_serialize_opts opts;
    uint32_t len = 0u;

    /* HEAD: same Content-Length as GET, no body bytes. */
    asx_http_response_init(&g_resp, ASX_HTTP_200_OK);
    ASSERT_EQ(asx_http_body_set_bytes(&g_resp.body, "hello", 5u), ASX_OK);
    asx_http_serialize_opts_init(&opts);
    opts.head_response = 1u;
    ASSERT_EQ(asx_http_serialize_response(&g_resp, &opts, g_wire, sizeof(g_wire), &len), ASX_OK);
    ASSERT_TRUE(wire_contains(g_wire, len, "Content-Length: 5\r\n\r\n"));
    ASSERT_FALSE(wire_contains(g_wire, len, "hello"));
    ASSERT_EQ(reparse_response(g_wire, len, ASX_HTTP_HEAD, &g_resp), ASX_OK);
    ASSERT_EQ(g_resp.body.len, 0u);

    /* A HEAD handler may declare the GET length itself. */
    asx_http_response_init(&g_resp, ASX_HTTP_200_OK);
    ASSERT_EQ(asx_http_headers_add(&g_resp.headers, "Content-Length", "1234"), ASX_OK);
    ASSERT_EQ(asx_http_serialize_response(&g_resp, &opts, g_wire, sizeof(g_wire), &len), ASX_OK);
    ASSERT_TRUE(wire_contains(g_wire, len, "Content-Length: 1234\r\n"));

    /* 204 / 304 / 1xx: no framing headers; bodies are rejected. */
    asx_http_serialize_opts_init(&opts);
    asx_http_response_init(&g_resp, ASX_HTTP_204_NO_CONTENT);
    ASSERT_EQ(asx_http_serialize_response(&g_resp, &opts, g_wire, sizeof(g_wire), &len), ASX_OK);
    ASSERT_EQ(len, (uint32_t)strlen("HTTP/1.1 204 No Content\r\n\r\n"));
    ASSERT_EQ(asx_http_body_set_bytes(&g_resp.body, "x", 1u), ASX_OK);
    ASSERT_EQ(asx_http_serialize_response(&g_resp, &opts, g_wire, sizeof(g_wire), &len),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(len, 0u);
    asx_http_response_init(&g_resp, ASX_HTTP_304_NOT_MODIFIED);
    ASSERT_EQ(asx_http_serialize_response(&g_resp, &opts, g_wire, sizeof(g_wire), &len), ASX_OK);
    ASSERT_FALSE(wire_contains(g_wire, len, "Content-Length"));
    asx_http_response_init(&g_resp, ASX_HTTP_100_CONTINUE);
    ASSERT_EQ(asx_http_serialize_response(&g_resp, &opts, g_wire, sizeof(g_wire), &len), ASX_OK);
    ASSERT_EQ(len, (uint32_t)strlen("HTTP/1.1 100 Continue\r\n\r\n"));

    /* Head-only LENGTH framing for a streamed body. */
    asx_http_response_init(&g_resp, ASX_HTTP_200_OK);
    opts.framing = ASX_HTTP_FRAMING_LENGTH;
    opts.content_length = 1048576u;
    ASSERT_EQ(asx_http_serialize_response_head(&g_resp, &opts, g_wire, sizeof(g_wire), &len),
              ASX_OK);
    ASSERT_TRUE(wire_contains(g_wire, len, "Content-Length: 1048576\r\n\r\n"));
}

TEST(serialize_rejects_injection_and_overflow) {
    uint32_t len = 77u;
    uint8_t small[16];

    /* Response splitting via CR/LF in a value. */
    asx_http_response_init(&g_resp, ASX_HTTP_200_OK);
    ASSERT_EQ(asx_http_headers_add(&g_resp.headers, "X-Evil", "a\r\nSet-Cookie: x=1"), ASX_OK);
    ASSERT_EQ(asx_http_serialize_response(&g_resp, NULL, g_wire, sizeof(g_wire), &len),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(len, 0u);
    asx_http_response_init(&g_resp, ASX_HTTP_200_OK);
    ASSERT_EQ(asx_http_headers_add(&g_resp.headers, "Bad Name", "v"), ASX_OK);
    ASSERT_EQ(asx_http_serialize_response(&g_resp, NULL, g_wire, sizeof(g_wire), &len),
              ASX_E_INVALID_ARGUMENT);
    asx_http_response_init(&g_resp, 99u);
    ASSERT_EQ(asx_http_serialize_response(&g_resp, NULL, g_wire, sizeof(g_wire), &len),
              ASX_E_INVALID_ARGUMENT);

    /* Request target / Host validation. */
    asx_http_request_init(&g_req, ASX_HTTP_GET, "/a b");
    ASSERT_EQ(asx_http_headers_add(&g_req.headers, "Host", "h"), ASX_OK);
    ASSERT_EQ(asx_http_serialize_request(&g_req, NULL, g_wire, sizeof(g_wire), &len),
              ASX_E_INVALID_ARGUMENT);
    asx_http_request_init(&g_req, ASX_HTTP_GET, "/");
    ASSERT_EQ(asx_http_serialize_request(&g_req, NULL, g_wire, sizeof(g_wire), &len),
              ASX_E_INVALID_ARGUMENT); /* HTTP/1.1 without Host */
    g_req.version = ASX_HTTP_VERSION_1_0;
    ASSERT_EQ(asx_http_serialize_request(&g_req, NULL, g_wire, sizeof(g_wire), &len), ASX_OK);
    ASSERT_EQ(len, (uint32_t)strlen("GET / HTTP/1.0\r\n\r\n"));

    /* Fail closed on short output buffers. */
    ASSERT_EQ(asx_http_serialize_request(&g_req, NULL, small, sizeof(small), &len),
              ASX_E_BUFFER_TOO_SMALL);
    ASSERT_EQ(len, 0u);
}

TEST(serialize_chunk_encoders) {
    asx_http_headers trailers;
    uint32_t len = 0u;
    uint8_t small[6];

    ASSERT_EQ(asx_http_encode_chunk("hello", 5u, g_wire, sizeof(g_wire), &len), ASX_OK);
    ASSERT_EQ(len, 10u);
    ASSERT_TRUE(memcmp(g_wire, "5\r\nhello\r\n", 10u) == 0);
    memset(g_wire + 100, 'z', 300u);
    ASSERT_EQ(asx_http_encode_chunk(g_wire + 100, 300u, g_wire, 100u, &len),
              ASX_E_BUFFER_TOO_SMALL);
    ASSERT_EQ(asx_http_encode_chunk(g_wire + 100, 300u, g_wire + 1000, 400u, &len), ASX_OK);
    ASSERT_TRUE(memcmp(g_wire + 1000, "12c\r\n", 5u) == 0);
    ASSERT_EQ(asx_http_encode_chunk("x", 0u, g_wire, sizeof(g_wire), &len), ASX_E_INVALID_ARGUMENT);

    ASSERT_EQ(asx_http_encode_last_chunk(NULL, g_wire, sizeof(g_wire), &len), ASX_OK);
    ASSERT_EQ(len, 5u);
    ASSERT_TRUE(memcmp(g_wire, "0\r\n\r\n", 5u) == 0);
    asx_http_headers_init(&trailers);
    ASSERT_EQ(asx_http_headers_add(&trailers, "X-A", "1"), ASX_OK);
    ASSERT_EQ(asx_http_encode_last_chunk(&trailers, g_wire, sizeof(g_wire), &len), ASX_OK);
    ASSERT_EQ(len, 13u);
    ASSERT_TRUE(memcmp(g_wire, "0\r\nX-A: 1\r\n\r\n", 13u) == 0);
    ASSERT_EQ(asx_http_encode_last_chunk(&trailers, small, sizeof(small), &len),
              ASX_E_BUFFER_TOO_SMALL);
    ASSERT_EQ(asx_http_headers_add(&trailers, "Authorization", "secret"), ASX_OK);
    ASSERT_EQ(asx_http_encode_last_chunk(&trailers, g_wire, sizeof(g_wire), &len),
              ASX_E_INVALID_ARGUMENT);
}

static asx_time fixed_clock(void *ctx) {
    (void)ctx;
    return (asx_time)784111777u * 1000000000u;
}

TEST(serialize_date_header_from_clock_hook) {
    asx_runtime_hooks hooks;
    asx_http_serialize_opts opts;
    uint32_t len = 0u;

    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    hooks.clock.now_ns_fn = fixed_clock;
    hooks.clock.logical_now_ns_fn = fixed_clock;
    ASSERT_EQ(asx_runtime_set_hooks(&hooks), ASX_OK);

    asx_http_response_init(&g_resp, ASX_HTTP_200_OK);
    ASSERT_EQ(asx_http_headers_add(&g_resp.headers, "Date", "stale"), ASX_OK);
    asx_http_serialize_opts_init(&opts);
    ASSERT_EQ(asx_http_serialize_response(&g_resp, &opts, g_wire, sizeof(g_wire), &len), ASX_OK);
    ASSERT_TRUE(wire_contains(g_wire, len, "Date: stale\r\n"));
    opts.emit_date = 1u;
    ASSERT_EQ(asx_http_serialize_response(&g_resp, &opts, g_wire, sizeof(g_wire), &len), ASX_OK);
    ASSERT_TRUE(wire_contains(g_wire, len, "Date: Sun, 06 Nov 1994 08:49:37 GMT\r\n"));
    ASSERT_FALSE(wire_contains(g_wire, len, "stale"));

    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    ASSERT_EQ(asx_runtime_set_hooks(&hooks), ASX_OK);
}

/* ------------------------------------------------------------------ */
/* End-to-end over the in-memory TCP loopback                          */
/* ------------------------------------------------------------------ */

#if ASX_HAS_SERVER_SURFACE
static asx_http_router g_router;
static asx_server g_srv;
static asx_http_server g_hs;
static asx_http_client_conn g_client;
static asx_http_response g_resps[4];
static uint8_t g_rx[16384];
static uint32_t g_sink_bytes;

static asx_status h_user(asx_http_request_context *ctx, asx_http_response *resp, void *ud) {
    char id[32];
    char page[32];
    char body[96];
    int n;
    (void)ud;

    if (asx_http_request_path_param(ctx, "id", id, sizeof(id)) != ASX_OK) return ASX_E_NOT_FOUND;
    if (asx_http_request_query_param(ctx->request, "page", page, sizeof(page)) != ASX_OK) {
        (void)snprintf(page, sizeof(page), "none");
    }
    asx_http_response_init(resp, ASX_HTTP_200_OK);
    n = snprintf(body, sizeof(body), "id=%s page=%s", id, page);
    if (n <= 0) return ASX_E_INVALID_ARGUMENT;
    if (asx_http_headers_add(&resp->headers, "Content-Type", "text/plain") != ASX_OK) {
        return ASX_E_INVALID_STATE;
    }
    return asx_http_body_set_bytes(&resp->body, body, (uint32_t)n);
}

static asx_status h_echo(asx_http_request_context *ctx, asx_http_response *resp, void *ud) {
    char len_text[24];
    (void)ud;

    asx_http_response_init(resp, ASX_HTTP_200_OK);
    if (ctx->request->body.kind == ASX_HTTP_BODY_STREAM) {
        char body[48];
        int n = snprintf(body, sizeof(body), "streamed=%u",
                         (unsigned)ctx->request->body.content_length);
        return asx_http_body_set_bytes(&resp->body, body, (uint32_t)n);
    }
    (void)snprintf(len_text, sizeof(len_text), "%u", (unsigned)ctx->request->body.len);
    if (asx_http_headers_add(&resp->headers, "X-Body-Len", len_text) != ASX_OK) {
        return ASX_E_INVALID_STATE;
    }
    return asx_http_body_set_bytes(&resp->body, ctx->request->body.data, ctx->request->body.len);
}

static asx_status h_empty(asx_http_request_context *ctx, asx_http_response *resp, void *ud) {
    (void)ctx;
    (void)ud;
    asx_http_response_init(resp, ASX_HTTP_204_NO_CONTENT);
    return ASX_OK;
}

static asx_status count_sink(void *ctx, const uint8_t *data, uint32_t len) {
    (void)ctx;
    (void)data;
    g_sink_bytes += len;
    return ASX_OK;
}

static int server_available(void) { return asx_surface_available_active(ASX_SURFACE_SERVER); }

/* Bind a listening asx_server on `port` and wrap it in an HTTP server. */
static int e2e_setup(uint16_t port, const asx_http_server_config *override) {
    asx_server_config scfg;
    asx_http_server_config hcfg;

    asx_net_reset();
    g_sink_bytes = 0u;
    asx_http_router_init(&g_router);
    if (asx_http_router_add_route(&g_router, ASX_HTTP_GET, "/users/:id", h_user, NULL, NULL,
                                  NULL) != ASX_OK ||
        asx_http_router_add_route(&g_router, ASX_HTTP_POST, "/echo", h_echo, NULL, NULL, NULL) !=
            ASX_OK ||
        asx_http_router_add_route(&g_router, ASX_HTTP_GET, "/empty", h_empty, NULL, NULL, NULL) !=
            ASX_OK) {
        return 0;
    }
    asx_server_config_init(&scfg);
    scfg.listen_addr = asx_socket_addr_loopback(port);
    asx_server_init(&g_srv, &scfg);
    if (asx_server_listen(&g_srv) != ASX_OK) return 0;
    if (override != NULL) {
        hcfg = *override;
        hcfg.router = &g_router;
    } else {
        asx_http_server_config_init(&hcfg, &g_router);
    }
    return asx_http_server_init(&g_hs, &g_srv, &hcfg) == ASX_OK;
}

static void pump(uint32_t rounds) {
    uint32_t i;
    for (i = 0u; i < rounds; i++) {
        asx_status st = asx_http_server_poll(&g_hs);
        if (st != ASX_OK && st != ASX_E_PENDING) return;
    }
}

/* Drive client and server until the client reports a final response. */
static asx_status exchange(asx_http_client_conn *c, asx_http_response *resp) {
    uint32_t i;

    for (i = 0u; i < 256u; i++) {
        asx_status st = asx_http_client_conn_poll(c, resp);
        if (st != ASX_E_PENDING) return st;
        st = asx_http_server_poll(&g_hs);
        if (st != ASX_OK && st != ASX_E_PENDING) return st;
    }
    return ASX_E_TIMED_OUT;
}

static int raw_write(asx_tcp_stream s, const void *data, uint32_t len) {
    asx_buf src = asx_buf_from(data, len);
    uint32_t written = 0u;
    return asx_tcp_stream_poll_write(s, &src, &written) == ASX_OK && written == len;
}

static uint32_t raw_read_all(asx_tcp_stream s) {
    static asx_buf_mut rx;
    uint32_t total = 0u;
    uint32_t guard;

    for (guard = 0u; guard < 64u; guard++) {
        uint32_t n = 0u;
        asx_buf readable;

        asx_buf_mut_init(&rx);
        if (asx_tcp_stream_poll_read(s, &rx, &n) != ASX_OK || n == 0u) break;
        readable = asx_buf_mut_readable(&rx);
        if (total + readable.len > sizeof(g_rx)) break;
        memcpy(g_rx + total, readable.ptr, readable.len);
        total += readable.len;
    }
    return total;
}

/* Parse up to `max` consecutive responses from g_rx[0..len). */
static uint32_t parse_responses(uint32_t len, const asx_http_method *methods, uint32_t max) {
    uint32_t pos = 0u;
    uint32_t count = 0u;

    if (asx_http_parser_init(&g_parser, ASX_HTTP_PARSE_RESPONSE, NULL, g_head,
                             (uint32_t)sizeof(g_head)) != ASX_OK) {
        return 0u;
    }
    while (count < max && pos < len) {
        uint32_t used = 0u;
        int complete = 0;
        asx_http_parser_set_request_method(&g_parser,
                                           methods != NULL ? methods[count] : ASX_HTTP_GET);
        if (asx_http_parser_collect_response(&g_parser, g_rx + pos, len - pos, &used,
                                             &g_resps[count], &complete) != ASX_OK ||
            !complete) {
            break;
        }
        pos += used;
        count++;
    }
    return count;
}

static int resp_body_is(const asx_http_response *r, const char *expected) {
    size_t n = strlen(expected);
    return r->body.len == (uint32_t)n && memcmp(r->body.data, expected, n) == 0;
}

TEST(e2e_get_and_chunked_post_through_router) {
    asx_socket_addr addr;
    asx_tcp_stream stream;
    asx_http_serialize_opts opts;
    asx_http_headers trailers;

    if (!server_available()) return;
    ASSERT_TRUE(e2e_setup(18080u, NULL));
    addr = asx_socket_addr_loopback(18080u);
    ASSERT_EQ(asx_tcp_connect(&stream, &addr), ASX_OK);
    ASSERT_EQ(asx_http_client_conn_init(&g_client, stream, NULL), ASX_OK);

    /* 1. GET with path + query params through the router. */
    asx_http_request_init(&g_req, ASX_HTTP_GET, "/users/42?page=7");
    ASSERT_EQ(asx_http_headers_add(&g_req.headers, "Host", "asx.test"), ASX_OK);
    ASSERT_EQ(asx_http_client_conn_send(&g_client, &g_req, NULL), ASX_OK);
    ASSERT_EQ(asx_http_client_conn_send(&g_client, &g_req, NULL), ASX_E_INVALID_STATE);
    ASSERT_EQ(exchange(&g_client, &g_resp), ASX_OK);
    ASSERT_EQ(g_resp.status, ASX_HTTP_200_OK);
    ASSERT_TRUE(resp_body_is(&g_resp, "id=42 page=7"));
    ASSERT_STR_EQ(asx_http_headers_get(&g_resp.headers, "Content-Length"), "12");
    ASSERT_STR_EQ(asx_http_headers_get(&g_resp.headers, "Content-Type"), "text/plain");
    ASSERT_TRUE(asx_http_client_conn_reusable(&g_client));
    ASSERT_EQ(asx_http_server_active_conns(&g_hs), 1u);

    /* 2. POST with a streamed chunked body and trailers (same connection). */
    asx_http_request_init(&g_req, ASX_HTTP_POST, "/echo");
    asx_http_serialize_opts_init(&opts);
    opts.framing = ASX_HTTP_FRAMING_CHUNKED;
    opts.host = "asx.test";
    asx_http_headers_init(&trailers);
    ASSERT_EQ(asx_http_headers_add(&trailers, "X-Checksum", "abc"), ASX_OK);
    ASSERT_EQ(asx_http_client_conn_begin(&g_client, &g_req, &opts), ASX_OK);
    ASSERT_EQ(asx_http_client_conn_write_body(&g_client, "hello ", 6u), ASX_OK);
    ASSERT_EQ(asx_http_client_conn_poll(&g_client, &g_resp), ASX_E_PENDING); /* flush */
    pump(2u);
    ASSERT_EQ(asx_http_client_conn_write_body(&g_client, "chunked ", 8u), ASX_OK);
    ASSERT_EQ(asx_http_client_conn_write_body(&g_client, "world", 5u), ASX_OK);
    ASSERT_EQ(asx_http_client_conn_finish(&g_client, &trailers), ASX_OK);
    ASSERT_EQ(asx_http_client_conn_write_body(&g_client, "late", 4u), ASX_E_INVALID_STATE);
    ASSERT_EQ(exchange(&g_client, &g_resp), ASX_OK);
    ASSERT_EQ(g_resp.status, ASX_HTTP_200_OK);
    ASSERT_TRUE(resp_body_is(&g_resp, "hello chunked world"));
    ASSERT_STR_EQ(asx_http_headers_get(&g_resp.headers, "X-Body-Len"), "19");

    /* 3. POST with a streamed Content-Length body. */
    asx_http_request_init(&g_req, ASX_HTTP_POST, "/echo");
    asx_http_serialize_opts_init(&opts);
    opts.framing = ASX_HTTP_FRAMING_LENGTH;
    opts.content_length = 10u;
    opts.host = "asx.test";
    ASSERT_EQ(asx_http_client_conn_begin(&g_client, &g_req, &opts), ASX_OK);
    ASSERT_EQ(asx_http_client_conn_write_body(&g_client, "0123", 4u), ASX_OK);
    ASSERT_EQ(asx_http_client_conn_finish(&g_client, NULL), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_http_client_conn_write_body(&g_client, "4567890", 7u), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_http_client_conn_write_body(&g_client, "456789", 6u), ASX_OK);
    ASSERT_EQ(asx_http_client_conn_finish(&g_client, NULL), ASX_OK);
    ASSERT_EQ(exchange(&g_client, &g_resp), ASX_OK);
    ASSERT_TRUE(resp_body_is(&g_resp, "0123456789"));

    /* 4. 204 from the router, unknown route 404. */
    asx_http_request_init(&g_req, ASX_HTTP_GET, "/empty");
    ASSERT_EQ(asx_http_headers_add(&g_req.headers, "Host", "asx.test"), ASX_OK);
    ASSERT_EQ(asx_http_client_conn_send(&g_client, &g_req, NULL), ASX_OK);
    ASSERT_EQ(exchange(&g_client, &g_resp), ASX_OK);
    ASSERT_EQ(g_resp.status, ASX_HTTP_204_NO_CONTENT);
    ASSERT_TRUE(asx_http_headers_get(&g_resp.headers, "Content-Length") == NULL);
    asx_http_request_init(&g_req, ASX_HTTP_GET, "/nope");
    ASSERT_EQ(asx_http_headers_add(&g_req.headers, "Host", "asx.test"), ASX_OK);
    ASSERT_EQ(asx_http_client_conn_send(&g_client, &g_req, NULL), ASX_OK);
    ASSERT_EQ(exchange(&g_client, &g_resp), ASX_OK);
    ASSERT_EQ(g_resp.status, ASX_HTTP_404_NOT_FOUND);
    ASSERT_TRUE(asx_http_client_conn_reusable(&g_client));

    /* 5. Connection: close ends the exchange on both sides. */
    asx_http_request_init(&g_req, ASX_HTTP_GET, "/users/7");
    ASSERT_EQ(asx_http_headers_add(&g_req.headers, "Host", "asx.test"), ASX_OK);
    asx_http_serialize_opts_init(&opts);
    opts.connection = ASX_HTTP_CONNECTION_CLOSE;
    ASSERT_EQ(asx_http_client_conn_send(&g_client, &g_req, &opts), ASX_OK);
    ASSERT_EQ(exchange(&g_client, &g_resp), ASX_OK);
    ASSERT_TRUE(resp_body_is(&g_resp, "id=7 page=none"));
    ASSERT_STR_EQ(asx_http_headers_get(&g_resp.headers, "Connection"), "close");
    ASSERT_FALSE(asx_http_client_conn_reusable(&g_client));
    ASSERT_EQ(g_client.state, ASX_HTTP_CONN_CLOSED);
    ASSERT_EQ(asx_http_client_conn_send(&g_client, &g_req, NULL), ASX_E_INVALID_STATE);
    pump(2u);
    ASSERT_EQ(asx_http_server_active_conns(&g_hs), 0u);
    ASSERT_EQ(asx_server_active_count(&g_srv), 0u);
    ASSERT_EQ(asx_http_server_requests_served(&g_hs), 6u);

    ASSERT_EQ(asx_server_shutdown(&g_srv), ASX_OK);
    ASSERT_EQ(asx_http_server_poll(&g_hs), ASX_OK);
    ASSERT_EQ(asx_server_get_state(&g_srv), ASX_SERVER_STATE_STOPPED);
}

TEST(e2e_pipelined_requests_answered_in_order) {
    static const char reqs[] = "GET /users/1?page=1 HTTP/1.1\r\nHost: a\r\n\r\n"
                               "GET /users/2?page=2 HTTP/1.1\r\nHost: a\r\n\r\n"
                               "POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 4\r\n"
                               "Connection: close\r\n\r\nping";
    asx_socket_addr addr;
    asx_tcp_stream s;
    uint32_t len;

    if (!server_available()) return;
    ASSERT_TRUE(e2e_setup(18081u, NULL));
    addr = asx_socket_addr_loopback(18081u);
    ASSERT_EQ(asx_tcp_connect(&s, &addr), ASX_OK);
    ASSERT_TRUE(raw_write(s, reqs, (uint32_t)sizeof(reqs) - 1u)); /* one write, three requests */
    pump(8u);
    len = raw_read_all(s);
    ASSERT_EQ(parse_responses(len, NULL, 3u), 3u);
    ASSERT_TRUE(resp_body_is(&g_resps[0], "id=1 page=1"));
    ASSERT_TRUE(resp_body_is(&g_resps[1], "id=2 page=2"));
    ASSERT_TRUE(resp_body_is(&g_resps[2], "ping"));
    ASSERT_TRUE(asx_http_headers_get(&g_resps[1].headers, "Connection") == NULL);
    ASSERT_STR_EQ(asx_http_headers_get(&g_resps[2].headers, "Connection"), "close");
    ASSERT_EQ(asx_http_server_active_conns(&g_hs), 0u);
    ASSERT_EQ(asx_http_server_requests_served(&g_hs), 3u);
    (void)asx_tcp_stream_close(s);
}

TEST(e2e_http10_close_and_keep_alive) {
    asx_socket_addr addr;
    asx_tcp_stream a;
    asx_tcp_stream b;
    uint32_t len;

    if (!server_available()) return;
    ASSERT_TRUE(e2e_setup(18082u, NULL));
    addr = asx_socket_addr_loopback(18082u);

    /* HTTP/1.0 without keep-alive: answered as 1.0 and closed. */
    ASSERT_EQ(asx_tcp_connect(&a, &addr), ASX_OK);
    ASSERT_TRUE(raw_write(a, LIT("GET /users/3?page=3 HTTP/1.0\r\n\r\n")));
    pump(4u);
    len = raw_read_all(a);
    ASSERT_TRUE(len > 17u && memcmp(g_rx, "HTTP/1.0 200 OK\r\n", 17u) == 0);
    ASSERT_EQ(parse_responses(len, NULL, 1u), 1u);
    ASSERT_STR_EQ(asx_http_headers_get(&g_resps[0].headers, "Connection"), "close");
    ASSERT_TRUE(resp_body_is(&g_resps[0], "id=3 page=3"));
    ASSERT_EQ(asx_http_server_active_conns(&g_hs), 0u);

    /* HTTP/1.0 with keep-alive: connection persists for a second request. */
    ASSERT_EQ(asx_tcp_connect(&b, &addr), ASX_OK);
    ASSERT_TRUE(
        raw_write(b, LIT("GET /users/4?page=4 HTTP/1.0\r\nConnection: keep-alive\r\n\r\n")));
    pump(4u);
    len = raw_read_all(b);
    ASSERT_EQ(parse_responses(len, NULL, 1u), 1u);
    ASSERT_STR_EQ(asx_http_headers_get(&g_resps[0].headers, "Connection"), "keep-alive");
    ASSERT_EQ(asx_http_server_active_conns(&g_hs), 1u);
    ASSERT_TRUE(raw_write(b, LIT("GET /users/5?page=5 HTTP/1.0\r\n\r\n")));
    pump(4u);
    len = raw_read_all(b);
    ASSERT_EQ(parse_responses(len, NULL, 1u), 1u);
    ASSERT_TRUE(resp_body_is(&g_resps[0], "id=5 page=5"));
    ASSERT_EQ(asx_http_server_active_conns(&g_hs), 0u);
    (void)asx_tcp_stream_close(a);
    (void)asx_tcp_stream_close(b);
}

TEST(e2e_protocol_errors_answer_and_close) {
    static const struct {
        const char *req;
        asx_http_status status;
    } rows[] = {
        {"POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 4\r\n"
         "Transfer-Encoding: chunked\r\n\r\n",
         400u},
        {"GET /users/1 HTTP/1.1\r\n\r\n", 400u},
        {"GET /users/1 HTTP/1.1\r\nHost: a\r\nX: a\r\n b\r\n\r\n", 400u},
        {"GET /users/1 HTTP/1.1\nHost: a\r\n\r\n", 400u},
        {"BREW /pot HTTP/1.1\r\nHost: a\r\n\r\n", 501u},
        {"POST /echo HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: gzip, chunked\r\n\r\n", 501u},
        {"GET / HTTP/2.0\r\nHost: a\r\n\r\n", 505u},
        {"POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 99999999999\r\n\r\n", 413u},
    };
    asx_socket_addr addr;
    uint32_t i;

    if (!server_available()) return;
    ASSERT_TRUE(e2e_setup(18083u, NULL));
    addr = asx_socket_addr_loopback(18083u);
    for (i = 0u; i < (uint32_t)(sizeof(rows) / sizeof(rows[0])); i++) {
        asx_tcp_stream s;
        uint32_t len;

        ASSERT_EQ(asx_tcp_connect(&s, &addr), ASX_OK);
        ASSERT_TRUE(raw_write(s, rows[i].req, (uint32_t)strlen(rows[i].req)));
        pump(4u);
        len = raw_read_all(s);
        ASSERT_EQ(parse_responses(len, NULL, 1u), 1u);
        if (g_resps[0].status != rows[i].status) {
            fprintf(stderr, "    error row %u: status %u\n", i, (unsigned)g_resps[0].status);
        }
        ASSERT_EQ(g_resps[0].status, rows[i].status);
        ASSERT_STR_EQ(asx_http_headers_get(&g_resps[0].headers, "Connection"), "close");
        ASSERT_EQ(asx_http_server_active_conns(&g_hs), 0u);
        (void)asx_tcp_stream_close(s);
    }
}

TEST(e2e_capacity_errors_431_414_413) {
    asx_socket_addr addr;
    asx_tcp_stream s;
    uint32_t len;
    uint32_t i;
    asx_http_server_config cfg;

    if (!server_available()) return;
    asx_http_server_config_init(&cfg, NULL);
    cfg.limits.max_body = 64u;
    ASSERT_TRUE(e2e_setup(18084u, &cfg));
    addr = asx_socket_addr_loopback(18084u);

    /* More header fields than the request struct holds -> 431. */
    len =
        (uint32_t)snprintf((char *)g_wire, sizeof(g_wire), "GET /users/1 HTTP/1.1\r\nHost: a\r\n");
    for (i = 0u; i < ASX_HTTP_MAX_HEADERS + 4u; i++) {
        len += (uint32_t)snprintf((char *)g_wire + len, sizeof(g_wire) - len, "X-%u: v\r\n",
                                  (unsigned)i);
    }
    len += (uint32_t)snprintf((char *)g_wire + len, sizeof(g_wire) - len, "\r\n");
    ASSERT_EQ(asx_tcp_connect(&s, &addr), ASX_OK);
    ASSERT_TRUE(raw_write(s, g_wire, len));
    pump(4u);
    ASSERT_EQ(parse_responses(raw_read_all(s), NULL, 1u), 1u);
    ASSERT_EQ(g_resps[0].status, 431u);
    (void)asx_tcp_stream_close(s);

    /* Request target longer than the request struct's URI capacity -> 414. */
    len = (uint32_t)snprintf((char *)g_wire, sizeof(g_wire), "GET /");
    for (i = 0u; i < ASX_HTTP_URI_MAX; i++) g_wire[len + i] = 'u';
    len += ASX_HTTP_URI_MAX;
    len += (uint32_t)snprintf((char *)g_wire + len, sizeof(g_wire) - len,
                              " HTTP/1.1\r\nHost: a\r\n\r\n");
    ASSERT_EQ(asx_tcp_connect(&s, &addr), ASX_OK);
    ASSERT_TRUE(raw_write(s, g_wire, len));
    pump(4u);
    ASSERT_EQ(parse_responses(raw_read_all(s), NULL, 1u), 1u);
    ASSERT_EQ(g_resps[0].status, 414u);
    (void)asx_tcp_stream_close(s);

    /* Body beyond the configured limit -> 413 before the body is read. */
    ASSERT_EQ(asx_tcp_connect(&s, &addr), ASX_OK);
    ASSERT_TRUE(raw_write(s, LIT("POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 65\r\n\r\n")));
    pump(4u);
    ASSERT_EQ(parse_responses(raw_read_all(s), NULL, 1u), 1u);
    ASSERT_EQ(g_resps[0].status, 413u);
    ASSERT_EQ(asx_http_server_active_conns(&g_hs), 0u);
    (void)asx_tcp_stream_close(s);
}

TEST(e2e_expect_100_continue) {
    static const char interim[] = "HTTP/1.1 100 Continue\r\n\r\n";
    asx_socket_addr addr;
    asx_tcp_stream s;
    uint32_t len;

    if (!server_available()) return;
    ASSERT_TRUE(e2e_setup(18085u, NULL));
    addr = asx_socket_addr_loopback(18085u);
    ASSERT_EQ(asx_tcp_connect(&s, &addr), ASX_OK);
    ASSERT_TRUE(raw_write(s, LIT("POST /echo HTTP/1.1\r\nHost: a\r\nExpect: 100-continue\r\n"
                                 "Content-Length: 5\r\n\r\n")));
    pump(4u);
    len = raw_read_all(s);
    ASSERT_EQ(len, (uint32_t)sizeof(interim) - 1u);
    ASSERT_TRUE(memcmp(g_rx, interim, len) == 0);
    ASSERT_TRUE(raw_write(s, LIT("hello")));
    pump(4u);
    len = raw_read_all(s);
    ASSERT_EQ(parse_responses(len, NULL, 1u), 1u);
    ASSERT_EQ(g_resps[0].status, 200u);
    ASSERT_TRUE(resp_body_is(&g_resps[0], "hello"));

    /* Body sent eagerly with the head: no interim response is generated. */
    ASSERT_TRUE(raw_write(s, LIT("POST /echo HTTP/1.1\r\nHost: a\r\nExpect: 100-continue\r\n"
                                 "Content-Length: 2\r\n\r\nhi")));
    pump(4u);
    len = raw_read_all(s);
    ASSERT_TRUE(len > 12u && memcmp(g_rx, "HTTP/1.1 200", 12u) == 0);
    (void)asx_tcp_stream_close(s);
}

TEST(e2e_head_mirrors_get_without_body) {
    static const asx_http_method methods[] = {ASX_HTTP_HEAD, ASX_HTTP_GET};
    asx_socket_addr addr;
    asx_tcp_stream s;
    uint32_t len;

    if (!server_available()) return;
    ASSERT_TRUE(e2e_setup(18086u, NULL));
    addr = asx_socket_addr_loopback(18086u);
    ASSERT_EQ(asx_tcp_connect(&s, &addr), ASX_OK);
    ASSERT_TRUE(raw_write(s, LIT("HEAD /users/9?page=1 HTTP/1.1\r\nHost: a\r\n\r\n"
                                 "GET /users/10?page=2 HTTP/1.1\r\nHost: a\r\n\r\n")));
    pump(6u);
    len = raw_read_all(s);
    ASSERT_EQ(parse_responses(len, methods, 2u), 2u);
    ASSERT_EQ(g_resps[0].status, 200u);
    ASSERT_STR_EQ(asx_http_headers_get(&g_resps[0].headers, "Content-Length"), "11");
    ASSERT_EQ(g_resps[0].body.len, 0u);
    ASSERT_TRUE(resp_body_is(&g_resps[1], "id=10 page=2"));
    (void)asx_tcp_stream_close(s);
}

TEST(e2e_streaming_request_body_sink) {
    static uint8_t chunk[2000];
    asx_http_server_config cfg;
    asx_socket_addr addr;
    asx_tcp_stream s;
    uint32_t i;
    uint32_t len;

    if (!server_available()) return;
    asx_http_server_config_init(&cfg, NULL);
    cfg.body_sink = count_sink;
    ASSERT_TRUE(e2e_setup(18087u, &cfg));
    addr = asx_socket_addr_loopback(18087u);
    ASSERT_EQ(asx_tcp_connect(&s, &addr), ASX_OK);
    ASSERT_TRUE(
        raw_write(s, LIT("POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 10000\r\n\r\n")));
    memset(chunk, 'q', sizeof(chunk));
    for (i = 0u; i < 5u; i++) {
        pump(2u);
        ASSERT_TRUE(raw_write(s, chunk, (uint32_t)sizeof(chunk)));
    }
    pump(4u);
    len = raw_read_all(s);
    ASSERT_EQ(parse_responses(len, NULL, 1u), 1u);
    ASSERT_TRUE(resp_body_is(&g_resps[0], "streamed=10000"));
    ASSERT_EQ(g_sink_bytes, 10000u); /* far beyond the 4 KiB inline body */
    (void)asx_tcp_stream_close(s);
}

TEST(e2e_max_requests_and_drain) {
    asx_http_server_config cfg;
    asx_socket_addr addr;
    asx_tcp_stream stream;
    asx_tcp_stream partial;
    uint32_t len;

    if (!server_available()) return;
    asx_http_server_config_init(&cfg, NULL);
    cfg.max_requests = 2u;
    ASSERT_TRUE(e2e_setup(18088u, &cfg));
    addr = asx_socket_addr_loopback(18088u);

    /* max_requests: the second response announces close. */
    ASSERT_EQ(asx_tcp_connect(&stream, &addr), ASX_OK);
    ASSERT_EQ(asx_http_client_conn_init(&g_client, stream, NULL), ASX_OK);
    asx_http_request_init(&g_req, ASX_HTTP_GET, "/users/1");
    ASSERT_EQ(asx_http_headers_add(&g_req.headers, "Host", "a"), ASX_OK);
    ASSERT_EQ(asx_http_client_conn_send(&g_client, &g_req, NULL), ASX_OK);
    ASSERT_EQ(exchange(&g_client, &g_resp), ASX_OK);
    ASSERT_TRUE(asx_http_client_conn_reusable(&g_client));
    ASSERT_EQ(asx_http_client_conn_send(&g_client, &g_req, NULL), ASX_OK);
    ASSERT_EQ(exchange(&g_client, &g_resp), ASX_OK);
    ASSERT_STR_EQ(asx_http_headers_get(&g_resp.headers, "Connection"), "close");
    ASSERT_FALSE(asx_http_client_conn_reusable(&g_client));
    pump(2u);
    ASSERT_EQ(asx_http_server_active_conns(&g_hs), 0u);

    /* Drain: an idle keep-alive connection is closed, an in-flight request
     * is still answered (with Connection: close), then the server stops. */
    ASSERT_EQ(asx_tcp_connect(&stream, &addr), ASX_OK);
    ASSERT_EQ(asx_http_client_conn_init(&g_client, stream, NULL), ASX_OK);
    ASSERT_EQ(asx_http_client_conn_send(&g_client, &g_req, NULL), ASX_OK);
    ASSERT_EQ(exchange(&g_client, &g_resp), ASX_OK);
    ASSERT_EQ(asx_tcp_connect(&partial, &addr), ASX_OK);
    ASSERT_TRUE(raw_write(partial, LIT("GET /users/5?page=5 HTTP/1.1\r\nHost: a\r\n")));
    pump(3u);
    ASSERT_EQ(asx_http_server_active_conns(&g_hs), 2u);

    ASSERT_EQ(asx_server_shutdown(&g_srv), ASX_OK);
    ASSERT_EQ(asx_server_get_state(&g_srv), ASX_SERVER_STATE_DRAINING);
    ASSERT_EQ(asx_http_server_poll(&g_hs), ASX_E_PENDING);
    ASSERT_EQ(asx_http_server_active_conns(&g_hs), 1u); /* idle keep-alive closed */
    ASSERT_EQ(asx_server_get_state(&g_srv), ASX_SERVER_STATE_DRAINING);

    ASSERT_TRUE(raw_write(partial, LIT("\r\n")));
    pump(1u);
    len = raw_read_all(partial);
    ASSERT_EQ(parse_responses(len, NULL, 1u), 1u);
    ASSERT_TRUE(resp_body_is(&g_resps[0], "id=5 page=5"));
    ASSERT_STR_EQ(asx_http_headers_get(&g_resps[0].headers, "Connection"), "close");
    ASSERT_EQ(asx_http_server_active_conns(&g_hs), 0u);
    ASSERT_EQ(asx_server_get_state(&g_srv), ASX_SERVER_STATE_STOPPED);
    ASSERT_EQ(asx_http_server_poll(&g_hs), ASX_OK);
    (void)asx_tcp_stream_close(partial);
    ASSERT_EQ(asx_http_client_conn_close(&g_client), ASX_OK);
}

/* Timeouts and limits (bd-9kll.10.3) run on a clock the test moves, in
 * deterministic and live builds alike. */
static asx_time g_test_now;

static asx_time test_clock(void *ctx) {
    (void)ctx;
    return g_test_now;
}

static int use_test_clock(void) {
    asx_runtime_hooks hooks;
    g_test_now = 0u;
    if (asx_runtime_hooks_init(&hooks) != ASX_OK) return 0;
    hooks.clock.now_ns_fn = test_clock;
    hooks.clock.logical_now_ns_fn = test_clock;
    return asx_runtime_set_hooks(&hooks) == ASX_OK;
}

static void restore_default_clock(void) {
    asx_runtime_hooks hooks;
    if (asx_runtime_hooks_init(&hooks) == ASX_OK) (void)asx_runtime_set_hooks(&hooks);
}

static void set_ms(uint32_t ms) { g_test_now = (asx_time)ms * 1000000u; }

/* The server closed `s`: a read finds end of stream. */
static int peer_closed(asx_tcp_stream s) {
    static asx_buf_mut rx;
    uint32_t n = 1u;
    asx_buf_mut_init(&rx);
    return asx_tcp_stream_poll_read(s, &rx, &n) == ASX_OK && n == 0u;
}

/* An idle keep-alive connection is closed once idle_timeout_ms passes
 * after its last response (Rust's Http1Config::idle_timeout). */
TEST(e2e_idle_keepalive_connection_times_out) {
    asx_http_server_config cfg;
    asx_socket_addr addr;
    asx_tcp_stream stream;

    if (!server_available()) return;
    ASSERT_TRUE(use_test_clock());
    asx_http_server_config_init(&cfg, NULL);
    ASSERT_EQ(cfg.idle_timeout_ms, 60000u);
    ASSERT_EQ(cfg.max_requests, 1000u);
    cfg.idle_timeout_ms = 1000u;
    ASSERT_TRUE(e2e_setup(18090u, &cfg));
    addr = asx_socket_addr_loopback(18090u);

    set_ms(100u);
    ASSERT_EQ(asx_tcp_connect(&stream, &addr), ASX_OK);
    ASSERT_EQ(asx_http_client_conn_init(&g_client, stream, NULL), ASX_OK);
    asx_http_request_init(&g_req, ASX_HTTP_GET, "/users/1");
    ASSERT_EQ(asx_http_headers_add(&g_req.headers, "Host", "a"), ASX_OK);
    ASSERT_EQ(asx_http_client_conn_send(&g_client, &g_req, NULL), ASX_OK);
    ASSERT_EQ(exchange(&g_client, &g_resp), ASX_OK);
    ASSERT_TRUE(asx_http_client_conn_reusable(&g_client));

    set_ms(1099u); /* response sent at 100 ms: 999 ms idle */
    pump(2u);
    ASSERT_EQ(asx_http_server_active_conns(&g_hs), 1u);
    set_ms(1100u);
    pump(1u);
    ASSERT_EQ(asx_http_server_active_conns(&g_hs), 0u);
    ASSERT_EQ(g_hs.connections_timed_out, 1u);
    ASSERT_TRUE(peer_closed(stream));
    ASSERT_EQ(asx_http_client_conn_close(&g_client), ASX_OK);
    restore_default_clock();
}

/* Slowloris: a client trickling a request head keeps no slot past the
 * idle timeout (the whole request reads under one window), and clients
 * that arrive meanwhile are served. */
TEST(e2e_slow_request_times_out_while_others_are_served) {
    asx_http_server_config cfg;
    asx_socket_addr addr;
    asx_tcp_stream slow;
    asx_tcp_stream stream;

    if (!server_available()) return;
    ASSERT_TRUE(use_test_clock());
    asx_http_server_config_init(&cfg, NULL);
    cfg.idle_timeout_ms = 1000u;
    ASSERT_TRUE(e2e_setup(18091u, &cfg));
    addr = asx_socket_addr_loopback(18091u);

    ASSERT_EQ(asx_tcp_connect(&slow, &addr), ASX_OK);
    ASSERT_TRUE(raw_write(slow, LIT("GET /users/1 HTTP/1.1\r\nHo")));
    pump(2u);
    set_ms(600u);
    ASSERT_TRUE(raw_write(slow, LIT("st: a\r\n"))); /* progress does not extend it */
    pump(2u);
    ASSERT_EQ(asx_http_server_active_conns(&g_hs), 1u);

    ASSERT_EQ(asx_tcp_connect(&stream, &addr), ASX_OK);
    ASSERT_EQ(asx_http_client_conn_init(&g_client, stream, NULL), ASX_OK);
    asx_http_request_init(&g_req, ASX_HTTP_GET, "/users/2");
    ASSERT_EQ(asx_http_headers_add(&g_req.headers, "Host", "a"), ASX_OK);
    ASSERT_EQ(asx_http_client_conn_send(&g_client, &g_req, NULL), ASX_OK);
    ASSERT_EQ(exchange(&g_client, &g_resp), ASX_OK);
    ASSERT_TRUE(resp_body_is(&g_resp, "id=2 page=none"));

    set_ms(1000u);
    pump(1u);
    ASSERT_EQ(g_hs.connections_timed_out, 1u);
    ASSERT_EQ(asx_http_server_active_conns(&g_hs), 1u); /* the fresh one stays */
    ASSERT_EQ(raw_read_all(slow), 0u);                  /* no response, just closed */
    ASSERT_TRUE(peer_closed(slow));
    (void)asx_tcp_stream_close(slow);
    ASSERT_EQ(asx_http_client_conn_close(&g_client), ASX_OK);
    restore_default_clock();
}

/* At its connection limit the server accepts and closes extra clients at
 * once (Rust's listener drops them), so they fail fast instead of waiting
 * in the backlog behind idle connections; the held connection still works. */
TEST(e2e_full_server_rejects_extra_clients_fast) {
    asx_socket_addr addr;
    asx_tcp_stream stream;
    asx_tcp_stream extra;

    if (!server_available()) return;
    ASSERT_TRUE(e2e_setup(18092u, NULL));
    g_srv.config.max_connections = 1u;
    addr = asx_socket_addr_loopback(18092u);

    ASSERT_EQ(asx_tcp_connect(&stream, &addr), ASX_OK);
    ASSERT_EQ(asx_http_client_conn_init(&g_client, stream, NULL), ASX_OK);
    asx_http_request_init(&g_req, ASX_HTTP_GET, "/users/1");
    ASSERT_EQ(asx_http_headers_add(&g_req.headers, "Host", "a"), ASX_OK);
    ASSERT_EQ(asx_http_client_conn_send(&g_client, &g_req, NULL), ASX_OK);
    ASSERT_EQ(exchange(&g_client, &g_resp), ASX_OK);

    ASSERT_EQ(asx_tcp_connect(&extra, &addr), ASX_OK);
    pump(1u);
    ASSERT_EQ(asx_server_total_rejected(&g_srv), 1u);
    ASSERT_TRUE(peer_closed(extra));
    ASSERT_EQ(asx_http_server_active_conns(&g_hs), 1u);

    ASSERT_EQ(asx_http_client_conn_send(&g_client, &g_req, NULL), ASX_OK);
    ASSERT_EQ(exchange(&g_client, &g_resp), ASX_OK);
    ASSERT_EQ(g_resp.status, ASX_HTTP_200_OK);
    (void)asx_tcp_stream_close(extra);
    ASSERT_EQ(asx_http_client_conn_close(&g_client), ASX_OK);
}

/* drain_timeout_ms is enforced: a connection stuck mid-request is closed
 * at the drain deadline and the server stops. */
TEST(e2e_drain_deadline_closes_stuck_connections) {
    asx_http_server_config cfg;
    asx_socket_addr addr;
    asx_tcp_stream stuck;
    asx_time deadline = 0u;

    if (!server_available()) return;
    ASSERT_TRUE(use_test_clock());
    asx_http_server_config_init(&cfg, NULL);
    cfg.idle_timeout_ms = 0u; /* only the drain deadline applies */
    ASSERT_TRUE(e2e_setup(18093u, &cfg));
    g_srv.config.drain_timeout_ms = 500u;
    addr = asx_socket_addr_loopback(18093u);

    ASSERT_EQ(asx_tcp_connect(&stuck, &addr), ASX_OK);
    ASSERT_TRUE(
        raw_write(stuck, LIT("POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 9\r\n\r\nab")));
    pump(2u);
    ASSERT_EQ(asx_http_server_active_conns(&g_hs), 1u);

    set_ms(200u);
    ASSERT_EQ(asx_server_shutdown(&g_srv), ASX_OK);
    ASSERT_EQ(asx_server_drain_deadline(&g_srv, &deadline), ASX_OK);
    ASSERT_EQ(deadline, (asx_time)700u * 1000000u);
    ASSERT_EQ(asx_http_server_poll(&g_hs), ASX_E_PENDING);
    set_ms(699u);
    ASSERT_EQ(asx_http_server_poll(&g_hs), ASX_E_PENDING);
    ASSERT_EQ(asx_http_server_active_conns(&g_hs), 1u);

    set_ms(700u);
    ASSERT_EQ(asx_http_server_poll(&g_hs), ASX_OK);
    ASSERT_EQ(asx_http_server_active_conns(&g_hs), 0u);
    ASSERT_EQ(g_hs.connections_drain_closed, 1u);
    ASSERT_EQ(asx_server_get_state(&g_srv), ASX_SERVER_STATE_STOPPED);
    ASSERT_TRUE(peer_closed(stuck));
    (void)asx_tcp_stream_close(stuck);
    restore_default_clock();
}

#if ASX_DETERMINISTIC
static asx_status http_server_task(void *ud, asx_task_id self) {
    (void)ud;
    (void)self;
    return asx_http_server_poll(&g_hs);
}

/* Under the scheduler the server task parks on I/O and its timer: with
 * nothing else to do, the virtual clock jumps to the idle deadline, the
 * task wakes and closes the connection, then waits for clients again. */
TEST(e2e_idle_timeout_wakes_the_server_task) {
    asx_http_server_config cfg;
    asx_socket_addr addr;
    asx_tcp_stream stream;
    asx_region_id r;
    asx_task_id t;
    asx_budget b;
    asx_time start;

    if (!server_available()) return;
    asx_runtime_reset();
    restore_default_clock();
    asx_http_server_config_init(&cfg, NULL);
    cfg.idle_timeout_ms = 5000u;
    ASSERT_TRUE(e2e_setup(18094u, &cfg));
    addr = asx_socket_addr_loopback(18094u);
    ASSERT_EQ(asx_tcp_connect(&stream, &addr), ASX_OK);
    start = asx_runtime_virtual_now();

    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, http_server_task, NULL, &t), ASX_OK);
    b = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(r, &b), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(g_hs.connections_timed_out, 1u);
    ASSERT_EQ(asx_http_server_active_conns(&g_hs), 0u);
    ASSERT_TRUE(asx_runtime_virtual_now() >= start + (asx_time)5000u * 1000000u);
    ASSERT_TRUE(peer_closed(stream));
    (void)asx_tcp_stream_close(stream);
    asx_runtime_reset();
}
#endif

TEST(e2e_client_parses_chunked_and_close_delimited) {
    static const char chunked[] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                                  "4\r\nWiki\r\n5;x=y\r\npedia\r\n0\r\nX-T: 1\r\n\r\n";
    static const char closing[] = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\n"
                                  "partial-close-body";
    asx_tcp_listener listener;
    asx_tcp_stream client;
    asx_tcp_stream server_side;
    asx_socket_addr addr;
    uint32_t len;

    asx_net_reset();
    addr = asx_socket_addr_loopback(18089u);
    ASSERT_EQ(asx_tcp_listener_bind(&listener, &addr), ASX_OK);
    ASSERT_EQ(asx_tcp_connect(&client, &addr), ASX_OK);
    ASSERT_EQ(asx_tcp_listener_poll_accept(listener, &server_side, NULL), ASX_OK);
    ASSERT_EQ(asx_http_client_conn_init(&g_client, client, NULL), ASX_OK);

    asx_http_request_init(&g_req, ASX_HTTP_GET, "/stream");
    ASSERT_EQ(asx_http_headers_add(&g_req.headers, "Host", "origin"), ASX_OK);
    ASSERT_EQ(asx_http_client_conn_send(&g_client, &g_req, NULL), ASX_OK);
    ASSERT_EQ(asx_http_client_conn_poll(&g_client, &g_resp), ASX_E_PENDING);
    len = raw_read_all(server_side);
    ASSERT_EQ(len, (uint32_t)strlen("GET /stream HTTP/1.1\r\nHost: origin\r\n\r\n"));
    ASSERT_TRUE(memcmp(g_rx, "GET /stream HTTP/1.1\r\nHost: origin\r\n\r\n", len) == 0);

    ASSERT_TRUE(raw_write(server_side, LIT(chunked)));
    ASSERT_EQ(asx_http_client_conn_poll(&g_client, &g_resp), ASX_OK);
    ASSERT_TRUE(resp_body_is(&g_resp, "Wikipedia"));
    ASSERT_TRUE(asx_http_client_conn_reusable(&g_client));

    /* Close-delimited: completes on EOF (reported explicitly here because
     * the in-memory transport cannot signal it through poll_read). */
    ASSERT_EQ(asx_http_client_conn_send(&g_client, &g_req, NULL), ASX_OK);
    ASSERT_EQ(asx_http_client_conn_poll(&g_client, &g_resp), ASX_E_PENDING);
    (void)raw_read_all(server_side);
    ASSERT_TRUE(raw_write(server_side, LIT(closing)));
    ASSERT_EQ(asx_http_client_conn_poll(&g_client, &g_resp), ASX_E_PENDING);
    asx_http_client_conn_notify_eof(&g_client);
    ASSERT_EQ(asx_http_client_conn_poll(&g_client, &g_resp), ASX_OK);
    ASSERT_TRUE(resp_body_is(&g_resp, "partial-close-body"));
    ASSERT_FALSE(asx_http_client_conn_reusable(&g_client));
    ASSERT_EQ(asx_http_client_conn_poll(&g_client, &g_resp), ASX_E_INVALID_STATE);

    (void)asx_tcp_stream_close(server_side);
    (void)asx_tcp_listener_close(listener);
}

TEST(e2e_client_rejects_malformed_response) {
    asx_tcp_listener listener;
    asx_tcp_stream client;
    asx_tcp_stream server_side;
    asx_socket_addr addr;

    asx_net_reset();
    addr = asx_socket_addr_loopback(18090u);
    ASSERT_EQ(asx_tcp_listener_bind(&listener, &addr), ASX_OK);
    ASSERT_EQ(asx_tcp_connect(&client, &addr), ASX_OK);
    ASSERT_EQ(asx_tcp_listener_poll_accept(listener, &server_side, NULL), ASX_OK);
    ASSERT_EQ(asx_http_client_conn_init(&g_client, client, NULL), ASX_OK);
    asx_http_request_init(&g_req, ASX_HTTP_GET, "/");
    ASSERT_EQ(asx_http_headers_add(&g_req.headers, "Host", "origin"), ASX_OK);
    ASSERT_EQ(asx_http_client_conn_send(&g_client, &g_req, NULL), ASX_OK);
    ASSERT_EQ(asx_http_client_conn_poll(&g_client, &g_resp), ASX_E_PENDING);
    ASSERT_TRUE(raw_write(server_side, LIT("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n"
                                           "Content-Length: 4\r\n\r\nabcd")));
    ASSERT_EQ(asx_http_client_conn_poll(&g_client, &g_resp), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_http_parser_error(&g_client.parser), ASX_HTTP_PERR_DUPLICATE_CONTENT_LENGTH);
    ASSERT_EQ(g_client.state, ASX_HTTP_CONN_CLOSED);
    (void)asx_tcp_stream_close(server_side);
    (void)asx_tcp_listener_close(listener);
}
#endif /* ASX_HAS_SERVER_SURFACE */

int main(void) {
    fprintf(stderr, "=== http wire tests ===\n");

    /* Request parsing */
    RUN_TEST(parse_simple_get);
    RUN_TEST(parse_request_forms_and_versions);
    RUN_TEST(parse_keep_alive_decisions_from_wire);
    RUN_TEST(parse_chunked_at_every_feed_granularity);
    RUN_TEST(parse_chunked_at_every_split_point);
    RUN_TEST(parse_content_length_body_byte_at_a_time);
    RUN_TEST(parse_pipelined_requests);
    RUN_TEST(parse_leading_empty_lines_bounded);
    RUN_TEST(parse_chunk_extensions_and_trailers);

    /* Rejections and limits */
    RUN_TEST(smuggling_and_malformed_rejections);
    RUN_TEST(rejection_status_mapping_examples);
    RUN_TEST(limit_violations_fail_closed);
    RUN_TEST(parser_argument_validation);

    /* Response parsing */
    RUN_TEST(parse_response_content_length_and_pipelined_204);
    RUN_TEST(parse_response_to_head_has_no_body);
    RUN_TEST(parse_response_close_delimited_and_eof);
    RUN_TEST(parse_response_versions_and_status_lines);
    RUN_TEST(parse_response_informational_and_upgrade);

    /* Collection path */
    RUN_TEST(collect_request_pipelined_and_partial);
    RUN_TEST(collect_request_struct_capacity_fails_closed);
    RUN_TEST(collect_response_skips_interim);

    /* Serializer */
    RUN_TEST(serialize_request_round_trip_content_length);
    RUN_TEST(serialize_request_round_trip_chunked_with_trailers);
    RUN_TEST(serialize_response_round_trip);
    RUN_TEST(serialize_head_and_bodiless_statuses);
    RUN_TEST(serialize_rejects_injection_and_overflow);
    RUN_TEST(serialize_chunk_encoders);
    RUN_TEST(serialize_date_header_from_clock_hook);

#if ASX_HAS_SERVER_SURFACE
    /* End-to-end over loopback */
    RUN_TEST(e2e_get_and_chunked_post_through_router);
    RUN_TEST(e2e_pipelined_requests_answered_in_order);
    RUN_TEST(e2e_http10_close_and_keep_alive);
    RUN_TEST(e2e_protocol_errors_answer_and_close);
    RUN_TEST(e2e_capacity_errors_431_414_413);
    RUN_TEST(e2e_expect_100_continue);
    RUN_TEST(e2e_head_mirrors_get_without_body);
    RUN_TEST(e2e_streaming_request_body_sink);
    RUN_TEST(e2e_max_requests_and_drain);
    RUN_TEST(e2e_idle_keepalive_connection_times_out);
    RUN_TEST(e2e_slow_request_times_out_while_others_are_served);
    RUN_TEST(e2e_full_server_rejects_extra_clients_fast);
    RUN_TEST(e2e_drain_deadline_closes_stuck_connections);
#if ASX_DETERMINISTIC
    RUN_TEST(e2e_idle_timeout_wakes_the_server_task);
#endif
    RUN_TEST(e2e_client_parses_chunked_and_close_delimited);
    RUN_TEST(e2e_client_rejects_malformed_response);
#endif

    TEST_REPORT();
    return test_failures;
}
