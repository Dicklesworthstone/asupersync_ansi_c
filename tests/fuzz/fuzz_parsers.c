/*
 * fuzz_parsers.c — fuzz harnesses for the parsers of untrusted input
 * (bd-9kll.16.1, SEC1).
 *
 * One source, one binary per parser: build with -DASX_FUZZ_<TARGET> (make
 * fuzz-parsers builds them all with clang -fsanitize=fuzzer,address,
 * undefined against a library instrumented the same way). Each target hands
 * the input to one parser the way a peer or a file would, in chunks of
 * varying size, and reads back every result the parser produces, so the
 * sanitizers see each byte it hands out. Where a parser has an exact
 * inverse, the round trip is checked too, and a mismatch aborts.
 *
 * Built with -DASX_FUZZ_STANDALONE the file has its own main, which runs
 * each file named on the command line once: committed regression inputs run
 * under any compiler, without libFuzzer.
 *
 * Targets:
 *   ASX_FUZZ_HTTP_REQUEST   HTTP/1.1 request parser (incremental feed,
 *                           pipelining, chunked bodies, trailers), then the
 *                           collect path and the multipart form parser
 *   ASX_FUZZ_HTTP_RESPONSE  HTTP/1.1 response parser, for GET, HEAD and
 *                           CONNECT requests
 *   ASX_FUZZ_WEBSOCKET      frame header decoder, and the WebSocket engine
 *                           (handshake or open, client or server role)
 *   ASX_FUZZ_BASE64         strict Base64 decoder, all three variants;
 *                           decode then encode gives the input back
 *   ASX_FUZZ_CODEC_JSON     canonical fixture JSON decoder; a decoded
 *                           fixture re-encodes and decodes again
 *   ASX_FUZZ_CODEC_BIN      canonical fixture binary view decoder
 *   ASX_FUZZ_WEB_QUERY      URI query string parser
 *   ASX_FUZZ_RAPTORQ_OTI    RaptorQ OTI decoder and block layout; a decoded
 *                           OTI re-serializes to the same 12 bytes
 *   ASX_FUZZ_RAPTORQ_DECODE RaptorQ block decoder fed received symbols
 *                           (any ESI, any bytes); with the encoder's
 *                           symbols a decode gives the source back
 *   ASX_FUZZ_FRAME_CODECS   length-delimited (1, 2, 4-byte length) and
 *                           line frame decoders
 *   ASX_FUZZ_RESOLVE_HOST   host resolution of literals: IPv4 dotted quads,
 *                           IPv6 text form (brackets, "::", IPv4 tail)
 *   ASX_FUZZ_JSON_READER    the conformance JSON reader (scenario and
 *                           fixture files)
 *
 * Differential mode (bd-9kll.16.2), not a fuzz target: -DASX_DIFF_<TARGET>
 * with -DASX_FUZZ_STANDALONE prints, for each file named on the command
 * line, one JSON line {"input": <name>, "outcome": {...}} describing what the
 * C parser made of the whole file, in the shape twin_run parse-diff prints
 * for asupersync's parser (tools/twin_run/src/parsediff.rs), so
 * tools/ci/parser_differential.py can compare the two:
 *   ASX_DIFF_HTTP_REQUEST   the first HTTP/1.1 request in the file, fed in
 *                           one piece and then end of input. ASX_DIFF_
 *                           REQUIRE_HOST=0 in the environment turns off
 *                           the parser's Host requirement, so a divergence
 *                           it hides can be reached.
 *   ASX_DIFF_HTTP_RESPONSE  the first HTTP/1.1 response in the file, once as
 *                           the answer to a GET, once to a HEAD and once to
 *                           a CONNECT (what frames its body): the outcome is
 *                           an object keyed by those method names.
 *   ASX_DIFF_WS_FRAME       the first WebSocket frame in the file, once as
 *                           received by a client and once by a server (the
 *                           masking rule differs): header decode and check,
 *                           unmasked payload, close-payload rules; a
 *                           rejected frame carries the close code the
 *                           receiver fails with, which is compared.
 *   ASX_DIFF_RAPTORQ        the file is a case `K T SEED R N MASK` (ASCII):
 *                           the source block of the test_raptorq.c
 *                           generator, encoded, the digest of every repair
 *                           symbol from ESI K, and the decode of K+N
 *                           encoding symbols chosen by MASK: twin_run
 *                           prints the same for the crates.io raptorq crate
 *                           and for asupersync's encoder and decoder.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <asx/asx.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/* The helpers below are shared; each target uses some of them. */
#if defined(__GNUC__) || defined(__clang__)
#define FUZZ_HELPER static __attribute__((unused))
#else
#define FUZZ_HELPER static
#endif

/* Read every byte of [p, p+n): ASan reports a byte that is not readable. */
static volatile uint8_t g_sink;
FUZZ_HELPER void touch(const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    size_t i;
    uint8_t acc = 0;
    for (i = 0; i < n; i++) acc = (uint8_t)(acc ^ b[i]);
    g_sink = acc;
}

FUZZ_HELPER void touch_str(const char *s) {
    if (s != NULL) touch(s, strlen(s) + 1u);
}

/* The file being run, for the differential mode (set by the standalone main). */
FUZZ_HELPER const char *g_input_name = "";

/* A violated property of a parser: abort, which the fuzzer reports. */
FUZZ_HELPER void fuzz_fail(const char *what) {
    fprintf(stderr, "fuzz_parsers: property violated: %s\n", what);
    abort();
}

/* Differential mode output (ASX_DIFF_*). The JSON string whose code points
 * are the n bytes of s (Latin-1), so equal bytes print equal on both sides
 * whatever they hold. */
FUZZ_HELPER void diff_put_string(const char *s, size_t n) {
    size_t i;
    putchar('"');
    for (i = 0; i < n; i++) {
        unsigned b = (unsigned char)s[i];
        if (b == (unsigned)'"' || b == (unsigned)'\\') {
            putchar('\\');
            putchar((int)b);
        } else if (b < 0x20u) {
            printf("\\u%04x", b);
        } else if (b < 0x80u) {
            putchar((int)b);
        } else {
            putchar((int)(0xC0u | (b >> 6)));
            putchar((int)(0x80u | (b & 0x3Fu)));
        }
    }
    putchar('"');
}

FUZZ_HELPER void diff_put_cstr(const char *s) { diff_put_string(s, strlen(s)); }

/* One line: {"input": <file>, "outcome": <outcome>} */
FUZZ_HELPER void diff_begin(void) {
    fputs("{\"input\":", stdout);
    diff_put_cstr(g_input_name);
    fputs(",\"outcome\":", stdout);
}

FUZZ_HELPER void diff_end(void) { fputs("}\n", stdout); }

/* The outcome objects carry no newline: a response, or a frame, prints one
 * per request method or receiver role inside one line. */
FUZZ_HELPER void diff_error(const char *detail) {
    fputs("{\"kind\":\"error\",\"detail\":", stdout);
    diff_put_cstr(detail);
    putchar('}');
}

/* Chunk sizes vary with the input's first byte: 1..8 bytes, or the rest. */
FUZZ_HELPER uint32_t next_chunk(uint8_t mode, uint32_t pos, uint32_t left) {
    uint32_t n;
    if ((mode & 0x80u) != 0u) return left;
    n = ((uint32_t)(mode & 0x07u) + pos) % 8u + 1u;
    return n < left ? n : left;
}

/* NUL-terminated copy of the input, for text parsers. */
FUZZ_HELPER char *dup_text(const uint8_t *data, size_t size) {
    char *s = (char *)malloc(size + 1u);
    if (s == NULL) return NULL;
    if (size > 0u) memcpy(s, data, size);
    s[size] = '\0';
    return s;
}

#if defined(ASX_FUZZ_HTTP_REQUEST) || defined(ASX_FUZZ_HTTP_RESPONSE)

#include <asx/net/http.h>

static uint8_t g_head[8192];

static void touch_head(const asx_http_parser *p) {
    uint32_t i;
    const char *name;
    const char *value;
    touch_str(asx_http_parser_target(p));
    touch_str(asx_http_parser_reason(p));
    for (i = 0; i < asx_http_parser_header_count(p); i++) {
        if (asx_http_parser_header_at(p, i, &name, &value) == ASX_OK) {
            touch_str(name);
            touch_str(value);
        }
    }
    touch_str(asx_http_parser_header(p, "content-length"));
    g_sink = (uint8_t)(asx_http_parser_content_length(p) + (uint64_t)asx_http_parser_keep_alive(p) +
                       (uint64_t)asx_http_parser_body_framing(p));
}

static void touch_trailers(const asx_http_parser *p) {
    uint32_t i;
    const char *name;
    const char *value;
    for (i = 0; i < asx_http_parser_trailer_count(p); i++) {
        if (asx_http_parser_trailer_at(p, i, &name, &value) == ASX_OK) {
            touch_str(name);
            touch_str(value);
        }
    }
}

/* Feed the input in chunks, draining events after each one; a parser that
 * keeps returning events without consuming input is a livelock. */
static void fuzz_http_parser(asx_http_parse_kind kind, uint8_t mode, const uint8_t *data,
                             uint32_t size) {
    asx_http_parser p;
    asx_http_event ev;
    uint32_t pos = 0;
    uint32_t guard = 4u * size + 64u;
    static const asx_http_method answered[3] = {ASX_HTTP_GET, ASX_HTTP_HEAD, ASX_HTTP_CONNECT};

    if (asx_http_parser_init(&p, kind, NULL, g_head, (uint32_t)sizeof(g_head)) != ASX_OK) {
        fuzz_fail("http parser init");
    }
    if (kind == ASX_HTTP_PARSE_RESPONSE) {
        asx_http_parser_set_request_method(&p, answered[(mode >> 3) % 3u]);
    }
    while (pos < size) {
        uint32_t n = next_chunk(mode, pos, size - pos);
        const uint8_t *chunk = data + pos;
        uint32_t left = n;
        for (;;) {
            uint32_t consumed = 0;
            if (guard-- == 0u) fuzz_fail("http parser makes no progress");
            if (asx_http_parser_feed(&p, chunk, left, &consumed, &ev) != ASX_OK) return;
            if (consumed > left) fuzz_fail("http parser consumed more than it was given");
            if (ev.kind == ASX_HTTP_EVENT_BODY) touch(ev.data, ev.len);
            if (ev.kind == ASX_HTTP_EVENT_HEAD) touch_head(&p);
            if (ev.kind == ASX_HTTP_EVENT_MESSAGE_COMPLETE) touch_trailers(&p);
            chunk += consumed;
            left -= consumed;
            if (ev.kind == ASX_HTTP_EVENT_NONE) break;
            if (asx_http_parser_is_upgraded(&p)) return;
        }
        pos += n;
    }
    if (asx_http_parser_finish(&p, &ev) == ASX_OK && ev.kind == ASX_HTTP_EVENT_MESSAGE_COMPLETE) {
        touch_trailers(&p);
    }
}

#if defined(ASX_FUZZ_HTTP_REQUEST)

static asx_http_request g_req;
static asx_http_multipart_form g_form;

/* The convenience path: collect whole requests, then parse each body as a
 * multipart form. */
static void fuzz_http_collect(const uint8_t *data, uint32_t size) {
    asx_http_parser p;
    uint32_t pos = 0;
    uint32_t guard = size + 8u;
    if (asx_http_parser_init(&p, ASX_HTTP_PARSE_REQUEST, NULL, g_head, (uint32_t)sizeof(g_head)) !=
        ASX_OK) {
        fuzz_fail("http parser init");
    }
    while (pos <= size) {
        uint32_t consumed = 0;
        int complete = 0;
        uint32_t i;
        if (guard-- == 0u) fuzz_fail("http collect makes no progress");
        memset(&g_req, 0, sizeof(g_req));
        if (asx_http_parser_collect_request(&p, data + pos, size - pos, &consumed, &g_req,
                                            &complete) != ASX_OK) {
            return;
        }
        if (consumed > size - pos) fuzz_fail("http collect consumed more than it was given");
        pos += consumed;
        if (!complete) return;
        touch(&g_req, sizeof(g_req));
        if (asx_http_parse_multipart(&g_req, &g_form) == ASX_OK) {
            if (g_form.count > ASX_HTTP_MULTIPART_PARTS_MAX) fuzz_fail("multipart part count");
            for (i = 0; i < g_form.count; i++) {
                if (g_form.parts[i].data_len > ASX_HTTP_MULTIPART_DATA_MAX) {
                    fuzz_fail("multipart part length");
                }
                touch(g_form.parts[i].data, g_form.parts[i].data_len);
                touch_str(g_form.parts[i].name);
                touch_str(g_form.parts[i].filename);
            }
        }
        if (consumed == 0u) return;
    }
}

static void fuzz_one(const uint8_t *data, size_t size) {
    if (size < 1u || size > (1u << 20)) return;
    fuzz_http_parser(ASX_HTTP_PARSE_REQUEST, data[0], data + 1, (uint32_t)(size - 1u));
    fuzz_http_collect(data + 1, (uint32_t)(size - 1u));
}

#else /* ASX_FUZZ_HTTP_RESPONSE */

static void fuzz_one(const uint8_t *data, size_t size) {
    if (size < 1u || size > (1u << 20)) return;
    fuzz_http_parser(ASX_HTTP_PARSE_RESPONSE, data[0], data + 1, (uint32_t)(size - 1u));
}

#endif

#elif defined(ASX_FUZZ_WEBSOCKET)

#include <asx/net/websocket.h>

static asx_ws_engine g_ws;
static uint8_t g_msg[ASX_WS_MAX_MESSAGE];

/* A fixed random source: the fuzzer drives the input, not the masks. */
static asx_status fixed_random(void *ctx, uint8_t *out, uint32_t len) {
    (void)ctx;
    memset(out, 0x5a, len);
    return ASX_OK;
}

static void fuzz_one(const uint8_t *data, size_t size) {
    asx_ws_config cfg;
    asx_ws_frame_header h;
    uint8_t mode;
    uint32_t pos = 0;
    uint32_t guard;

    if (size < 1u || size > (1u << 20)) return;
    mode = data[0];
    data++;
    size--;

    /* The header decoder alone, on the raw bytes. */
    if (asx_ws_header_decode(data, (uint32_t)size, &h) == ASX_OK) {
        if (h.header_len > size) fuzz_fail("ws header longer than the input");
        g_sink = (uint8_t)(asx_ws_header_check(&h, ASX_WS_ROLE_SERVER) +
                           asx_ws_header_check(&h, ASX_WS_ROLE_CLIENT));
    }

    /* The engine: client or server, with or without the opening handshake. */
    asx_ws_config_init(&cfg, (mode & 1u) ? ASX_WS_ROLE_SERVER : ASX_WS_ROLE_CLIENT);
    cfg.random_fn = fixed_random;
    cfg.host = "fuzz.example";
    cfg.path = "/chat";
    cfg.protocols = (mode & 4u) ? "chat, superchat" : NULL;
    cfg.skip_handshake = (mode & 2u) ? 1 : 0;
    if (asx_ws_engine_init(&g_ws, &cfg) != ASX_OK) return;
    guard = 4u * (uint32_t)size + 64u;
    while (pos < (uint32_t)size) {
        uint32_t n = next_chunk(mode, pos, (uint32_t)size - pos);
        uint32_t consumed = 0;
        asx_ws_opcode op;
        uint32_t len = 0;
        if (guard-- == 0u) fuzz_fail("ws engine makes no progress");
        if (asx_ws_engine_feed(&g_ws, data + pos, n, &consumed) != ASX_OK) return;
        if (consumed > n) fuzz_fail("ws engine consumed more than it was given");
        while (asx_ws_engine_recv(&g_ws, &op, g_msg, (uint32_t)sizeof(g_msg), &len) == ASX_OK) {
            if (len > sizeof(g_msg)) fuzz_fail("ws message longer than the buffer");
            touch(g_msg, len);
            if (guard-- == 0u) fuzz_fail("ws engine returns messages forever");
        }
        /* Unconsumed bytes wait for the messages above to be drained. */
        pos += consumed;
        if (consumed == 0u && n > 0u) {
            uint32_t retry = 0;
            if (asx_ws_engine_feed(&g_ws, data + pos, n, &retry) != ASX_OK || retry == 0u) return;
            pos += retry;
        }
    }
}

#elif defined(ASX_FUZZ_BASE64)

#include <asx/security/crypto.h>

static void fuzz_one(const uint8_t *data, size_t size) {
    asx_base64_variant variant;
    uint8_t *out;
    char *again;
    size_t n = 0;
    size_t m = 0;
    const char *text;
    size_t text_len;

    if (size < 1u || size > (1u << 16)) return;
    variant = (asx_base64_variant)(data[0] % 3u);
    text = (const char *)(data + 1);
    text_len = size - 1u;
    out = (uint8_t *)malloc(text_len + 1u);
    again = (char *)malloc(text_len + 2u);
    if (out == NULL || again == NULL) {
        free(out);
        free(again);
        return;
    }
    if (asx_base64_decode(variant, text, text_len, out, text_len + 1u, &n) == ASX_OK) {
        /* Strict decoding accepts only the canonical encoding: encoding the
         * result gives the input back. */
        if (n > text_len) fuzz_fail("base64 decoded more bytes than characters");
        if (asx_base64_encode(variant, out, n, again, text_len + 2u, &m) != ASX_OK) {
            fuzz_fail("base64 re-encode of a decoded value");
        }
        if (m != text_len || memcmp(again, text, text_len) != 0) {
            fuzz_fail("base64 decode accepted a non-canonical encoding");
        }
    }
    free(out);
    free(again);
}

#elif defined(ASX_FUZZ_CODEC_JSON)

#include <asx/codec/codec.h>

static asx_canonical_fixture g_fixture;
static asx_canonical_fixture g_fixture2;

static void fuzz_one(const uint8_t *data, size_t size) {
    char *json;
    asx_codec_buffer buf;
    if (size > (1u << 20)) return;
    json = dup_text(data, size);
    if (json == NULL) return;
    asx_canonical_fixture_init(&g_fixture);
    if (asx_codec_decode_fixture_json(json, &g_fixture) == ASX_OK) {
        /* What decodes, re-encodes; and the encoding decodes again. */
        asx_codec_buffer_init(&buf);
        if (asx_codec_encode_fixture_json(&g_fixture, &buf) != ASX_OK) {
            fuzz_fail("codec json re-encode of a decoded fixture");
        }
        asx_canonical_fixture_init(&g_fixture2);
        if (asx_codec_decode_fixture_json(buf.data, &g_fixture2) != ASX_OK) {
            fuzz_fail("codec json decode of its own encoding");
        }
        asx_canonical_fixture_reset(&g_fixture2);
        asx_codec_buffer_reset(&buf);
    }
    asx_canonical_fixture_reset(&g_fixture);
    free(json);
}

#elif defined(ASX_FUZZ_CODEC_BIN)

#include <asx/codec/codec.h>

static void touch_slice(asx_codec_slice s) {
    if (s.ptr != NULL) touch(s.ptr, s.len);
}

static void fuzz_one(const uint8_t *data, size_t size) {
    asx_codec_bin_fixture_view view;
    if (size > (1u << 20)) return;
    asx_codec_bin_fixture_view_init(&view);
    if (asx_codec_decode_fixture_bin_view(data, size, &view) == ASX_OK) {
        const uint8_t *lo = data;
        const uint8_t *hi = data + size;
        const asx_codec_slice *s[3];
        size_t i;
        s[0] = &view.input_json;
        s[1] = &view.expected_events_json;
        s[2] = &view.semantic_digest;
        for (i = 0; i < 3u; i++) {
            const uint8_t *p = (const uint8_t *)s[i]->ptr;
            if (p != NULL && (p < lo || p + s[i]->len > hi)) {
                fuzz_fail("codec bin view slice outside the payload");
            }
        }
        touch_slice(view.profile);
        touch_slice(view.input_json);
        touch_slice(view.expected_events_json);
        touch_slice(view.expected_final_snapshot_json);
        touch_slice(view.expected_error_codes_json);
        touch_slice(view.semantic_digest);
        touch_slice(view.rust_baseline_commit);
        touch_slice(view.capture_run_id);
    }
}

#elif defined(ASX_FUZZ_WEB_QUERY)

#include <asx/net/web.h>

static asx_web_query g_query;

static void fuzz_one(const uint8_t *data, size_t size) {
    char *uri;
    uint32_t i;
    if (size > (1u << 16)) return;
    uri = dup_text(data, size);
    if (uri == NULL) return;
    if (asx_web_query_parse(&g_query, uri) == ASX_OK) {
        if (g_query.count > ASX_WEB_MAX_QUERY_PARAMS) fuzz_fail("query param count");
        for (i = 0; i < g_query.count; i++) {
            if (memchr(g_query.params[i].key, '\0', sizeof(g_query.params[i].key)) == NULL ||
                memchr(g_query.params[i].value, '\0', sizeof(g_query.params[i].value)) == NULL) {
                fuzz_fail("query param not NUL-terminated");
            }
            touch_str(g_query.params[i].key);
            touch_str(g_query.params[i].value);
        }
    }
    free(uri);
}

#elif defined(ASX_FUZZ_RAPTORQ_OTI)

#include <asx/raptorq/raptorq.h>

static void fuzz_one(const uint8_t *data, size_t size) {
    asx_raptorq_oti oti;
    uint8_t again[ASX_RAPTORQ_OTI_SIZE];
    uint32_t sbn;
    if (size < ASX_RAPTORQ_OTI_SIZE) return;
    if (asx_raptorq_oti_deserialize(data, &oti) != ASX_OK) return;
    if (asx_raptorq_oti_validate(&oti) != ASX_OK) fuzz_fail("raptorq decoded OTI is invalid");
    if (asx_raptorq_oti_serialize(&oti, again) != ASX_OK ||
        memcmp(again, data, ASX_RAPTORQ_OTI_SIZE) != 0) {
        fuzz_fail("raptorq OTI does not re-serialize to its input");
    }
    /* Every source block of the object has a layout inside it. */
    for (sbn = 0; sbn < 256u; sbn++) {
        asx_raptorq_block_layout layout;
        if (asx_raptorq_oti_block_layout(&oti, sbn, &layout) != ASX_OK) break;
        if (layout.length > (uint64_t)layout.source_symbols * layout.symbol_size) {
            fuzz_fail("raptorq block covers more than K * T bytes");
        }
    }
}

#elif defined(ASX_FUZZ_RAPTORQ_DECODE)

#include <asx/raptorq/raptorq.h>

/* Small blocks keep the solver fast. */
#define RQ_MAX_K 24u
#define RQ_MAX_T 16u
#define RQ_MAX_RECEIVED 64u

/* Input: K, symbol size T, mode, then 3-byte little-endian ESIs. With an
 * odd mode each ESI is followed by up to T symbol bytes, as a peer may
 * send anything. With an even mode the symbols are the encoder's for a
 * source block derived from the header: a decode that succeeds must give
 * that block back, and one with every source symbol must succeed (the
 * code is systematic, RFC 6330 section 5.3.3.4). */
static void fuzz_one(const uint8_t *data, size_t size) {
    uint8_t source[RQ_MAX_K * RQ_MAX_T];
    uint8_t out[RQ_MAX_K * RQ_MAX_T];
    uint8_t again[RQ_MAX_K * RQ_MAX_T];
    uint8_t sym[RQ_MAX_T];
    asx_raptorq_decoder dec;
    asx_raptorq_encoder enc;
    size_t ws_size = 0;
    size_t enc_size = 0;
    size_t pos = 3u;
    size_t block;
    size_t i;
    void *ws;
    void *enc_ws = NULL;
    uint32_t k;
    uint32_t t;
    uint32_t received = 0;
    uint32_t sources = 0;
    int encoded;
    asx_status st;

    if (size < 3u) return;
    k = 1u + data[0] % RQ_MAX_K;
    t = 1u + data[1] % RQ_MAX_T;
    encoded = (data[2] & 1u) == 0u;
    block = (size_t)k * t;
    if (asx_raptorq_decoder_workspace_size(k, t, RQ_MAX_RECEIVED, &ws_size) != ASX_OK) {
        fuzz_fail("raptorq decoder workspace size refused");
    }
    ws = malloc(ws_size);
    if (ws == NULL) return;
    if (asx_raptorq_decoder_init(&dec, k, t, RQ_MAX_RECEIVED, ws, ws_size) != ASX_OK) {
        fuzz_fail("raptorq decoder refused its queried workspace");
    }
    if (encoded) {
        for (i = 0; i < block; i++) source[i] = (uint8_t)(data[i % 3u] * 31u + i * 7u + 1u);
        if (asx_raptorq_encoder_workspace_size(k, t, &enc_size) != ASX_OK) {
            fuzz_fail("raptorq encoder workspace size refused");
        }
        enc_ws = malloc(enc_size);
        if (enc_ws == NULL) {
            free(ws);
            return;
        }
        if (asx_raptorq_encoder_init(&enc, k, t, source, block, enc_ws, enc_size) != ASX_OK) {
            fuzz_fail("raptorq encoder refused a valid block");
        }
    }
    while (received < RQ_MAX_RECEIVED && pos + 3u <= size) {
        uint32_t esi = (uint32_t)data[pos] | ((uint32_t)data[pos + 1u] << 8) |
                       ((uint32_t)data[pos + 2u] << 16);
        pos += 3u;
        if (encoded) {
            /* The source symbols and the first repair symbols. */
            esi %= k + 40u;
            if (asx_raptorq_encoder_symbol(&enc, esi, sym, t) != ASX_OK) {
                fuzz_fail("raptorq encoder refused an ESI");
            }
        } else {
            size_t n = size - pos < t ? size - pos : t;
            memset(sym, 0, sizeof(sym));
            memcpy(sym, data + pos, n);
            pos += n;
        }
        st = asx_raptorq_decoder_add_symbol(&dec, esi, sym, t);
        if (st == ASX_OK) {
            received++;
            if (esi < k) sources++;
        } else if (st != ASX_E_DUPLICATE_SYMBOL) {
            fuzz_fail("raptorq decoder refused a valid symbol");
        }
    }
    st = asx_raptorq_decoder_decode(&dec, out, block);
    if (st == ASX_OK) {
        touch(out, block);
        if (encoded && memcmp(out, source, block) != 0) {
            fuzz_fail("raptorq decoded block is not the source");
        }
        /* The stored symbols are kept: decoding again gives the same block. */
        if (asx_raptorq_decoder_decode(&dec, again, block) != ASX_OK ||
            memcmp(again, out, block) != 0) {
            fuzz_fail("raptorq second decode differs");
        }
    } else if (st != ASX_E_RESOURCE_EXHAUSTED) {
        fuzz_fail("raptorq decode failed other than for too few symbols");
    } else if (encoded && sources == k) {
        fuzz_fail("raptorq decode failed with every source symbol received");
    }
    free(enc_ws);
    free(ws);
}

#elif defined(ASX_FUZZ_FRAME_CODECS)

#include <asx/bytes/buf.h>
#include <asx/bytes/codec.h>

/* The buffer is a block of its own on the heap, on purpose. asx_buf_mut keeps
 * its bytes in an inline array, so a decoder that reads the byte before
 * data[0] (the planted off-by-one of ASX_MUTANT_FRAME_CODECS: an empty first
 * line) reads whatever precedes the object. Behind a static object that is a
 * neighbouring global or padding, which AddressSanitizer does not poison, and
 * the read goes unseen (batch #19: the replay control found nothing with the
 * seed that reaches it). A heap block has a redzone on its left. */
static asx_buf_mut *g_buf;

static void drain_frames(void *state,
                         asx_decode_result (*decode)(void *, asx_buf_mut *, asx_frame *)) {
    uint32_t guard = ASX_BUF_CAPACITY + 8u;
    for (;;) {
        asx_frame f;
        asx_decode_result r = decode(state, g_buf, &f);
        if (guard-- == 0u) fuzz_fail("frame decoder makes no progress");
        if (r != ASX_DECODE_FRAME) return;
        if (f.len > ASX_BUF_CAPACITY) fuzz_fail("frame longer than the buffer");
        touch(f.data, f.len);
    }
}

static void fuzz_one(const uint8_t *data, size_t size) {
    static const uint8_t field_sizes[3] = {1u, 2u, 4u};
    asx_length_delimited_codec ld;
    asx_lines_codec lines;
    uint8_t mode;
    uint32_t pos = 0;
    if (size < 1u) return;
    mode = data[0];
    data++;
    size--;
    if (size > ASX_BUF_CAPACITY) size = ASX_BUF_CAPACITY;

    if (asx_length_delimited_codec_init_with(&ld, field_sizes[mode % 3u],
                                             (mode & 0x40u) ? 64u : 0u) != ASX_OK) {
        return;
    }
    g_buf = (asx_buf_mut *)calloc(1u, sizeof(*g_buf));
    if (g_buf == NULL) return;
    asx_buf_mut_init(g_buf);
    while (pos < (uint32_t)size) {
        uint32_t n = next_chunk(mode, pos, (uint32_t)size - pos);
        if (asx_buf_mut_put(g_buf, data + pos, n) != ASX_OK) break;
        drain_frames(&ld, asx_length_delimited_decode);
        pos += n;
    }

    asx_lines_codec_init_with(&lines, (mode & 0x20u) ? 16u : 0u);
    asx_buf_mut_init(g_buf);
    pos = 0;
    while (pos < (uint32_t)size) {
        uint32_t n = next_chunk(mode, pos, (uint32_t)size - pos);
        if (asx_buf_mut_put(g_buf, data + pos, n) != ASX_OK) break;
        drain_frames(&lines, asx_lines_decode);
        pos += n;
    }
    free(g_buf);
    g_buf = NULL;
}

#elif defined(ASX_FUZZ_RESOLVE_HOST)

#include <asx/net/net.h>

static void fuzz_one(const uint8_t *data, size_t size) {
    asx_resolve_options opts;
    asx_resolve_result res;
    char *host;
    uint8_t mode;
    if (size < 1u || size > 1024u) return;
    mode = data[0];
    host = dup_text(data + 1, size - 1u);
    if (host == NULL) return;
    /* Literals and "localhost" resolve in every build; anything else is
     * ASX_E_NOT_FOUND without the native backend (no network here). */
    asx_resolve_options_init(&opts, (uint16_t)(mode * 257u));
    opts.allow_ipv4 = (uint8_t)((mode & 1u) || !(mode & 2u));
    opts.allow_ipv6 = (uint8_t)((mode & 2u) || !(mode & 1u));
    opts.preferred_family = (mode & 4u) ? ASX_AF_INET6 : ASX_AF_INET4;
    if (asx_resolve_host(&res, host, &opts) == ASX_OK) {
        if (res.count == 0u || res.count > ASX_RESOLVE_MAX_RESULTS) {
            fuzz_fail("resolve result count");
        }
        touch(res.addrs, res.count * sizeof(res.addrs[0]));
    }
    free(host);
}

#elif defined(ASX_FUZZ_JSON_READER)

#include "../../src/conformance/json.h"

static asx_json_doc g_doc;

static void fuzz_one(const uint8_t *data, size_t size) {
    uint32_t root = 0;
    if (size > (1u << 20)) return;
    if (asx_json_parse(&g_doc, (const char *)data, size, &root) == ASX_OK) {
        uint32_t i;
        uint32_t n = asx_json_count(&g_doc, root);
        for (i = 0; i < n; i++) touch_str(asx_json_string(&g_doc, asx_json_item(&g_doc, root, i)));
    }
}

#elif defined(ASX_DIFF_HTTP_REQUEST) || defined(ASX_DIFF_HTTP_RESPONSE)

#include <asx/net/http.h>

#ifndef ASX_FUZZ_STANDALONE
#error "the ASX_DIFF_* modes need ASX_FUZZ_STANDALONE (one outcome line per file)"
#endif

static uint8_t g_head[8192];

static void diff_put_fields(const asx_http_parser *p, int trailers) {
    uint32_t n = trailers ? asx_http_parser_trailer_count(p) : asx_http_parser_header_count(p);
    uint32_t i;
    putchar('[');
    for (i = 0; i < n; i++) {
        const char *name = "";
        const char *value = "";
        asx_status st = trailers ? asx_http_parser_trailer_at(p, i, &name, &value)
                                 : asx_http_parser_header_at(p, i, &name, &value);
        if (st != ASX_OK) fuzz_fail("http field index below its count");
        if (i > 0u) putchar(',');
        putchar('[');
        diff_put_cstr(name);
        putchar(',');
        diff_put_cstr(value);
        putchar(']');
    }
    putchar(']');
}

static void diff_complete(const asx_http_parser *p, asx_http_parse_kind kind, uint64_t body_len,
                          uint64_t body_fnv, uint32_t consumed) {
    fputs("{\"kind\":\"complete\",", stdout);
    if (kind == ASX_HTTP_PARSE_REQUEST) {
        asx_http_method method = asx_http_parser_method(p);
        const char *token = asx_http_parser_method_token(p);

        /* an extension method has no name of its own: its text is the token */
        if ((method == ASX_HTTP_EXTENSION) != (token != NULL)) {
            fuzz_fail("http method token exactly for an extension method");
        }
        fputs("\"method\":", stdout);
        diff_put_cstr(token != NULL ? token : asx_http_method_str(method));
        fputs(",\"target\":", stdout);
        diff_put_cstr(asx_http_parser_target(p));
    } else {
        printf("\"status\":%u,\"reason\":", (unsigned)asx_http_parser_status(p));
        diff_put_cstr(asx_http_parser_reason(p));
    }
    fputs(",\"version\":", stdout);
    diff_put_cstr(asx_http_version_str(asx_http_parser_version(p)));
    fputs(",\"headers\":", stdout);
    diff_put_fields(p, 0);
    fputs(",\"trailers\":", stdout);
    diff_put_fields(p, 1);
    printf(",\"body_len\":%llu,\"body_fnv\":\"%016llx\",\"consumed\":%lu}",
           (unsigned long long)body_len, (unsigned long long)body_fnv, (unsigned long)consumed);
}

/* The first message in the input, fed in one piece and then end of input; a
 * response is decoded as the answer to a request of method `answers`.
 * Accepted means the parser reported MESSAGE_COMPLETE; anything else (a
 * protocol error, a limit, input cut short, input that holds no message)
 * is an error with the parser's own reason in "detail". */
static void diff_message(asx_http_parse_kind kind, asx_http_method answers, const uint8_t *data,
                         size_t size) {
    asx_http_limits limits;
    asx_http_parser p;
    asx_http_event ev;
    const char *env = getenv("ASX_DIFF_REQUIRE_HOST");
    uint32_t pos = 0;
    uint32_t guard;
    uint64_t body_len = 0;
    uint64_t fnv = UINT64_C(0xcbf29ce484222325);
    asx_status st;

    if (size == 0u) {
        fputs("{\"kind\":\"empty\"}", stdout);
        return;
    }
    if (size > (1u << 20)) {
        diff_error("input too large");
        return;
    }
    asx_http_limits_init(&limits);
    if (env != NULL && env[0] == '0') limits.require_host = 0u;
    if (asx_http_parser_init(&p, kind, &limits, g_head, (uint32_t)sizeof(g_head)) != ASX_OK) {
        fuzz_fail("http parser init");
    }
    if (kind == ASX_HTTP_PARSE_RESPONSE) asx_http_parser_set_request_method(&p, answers);
    guard = 4u * (uint32_t)size + 64u;
    for (;;) {
        uint32_t used = 0;
        if (guard-- == 0u) fuzz_fail("http parser makes no progress");
        st = asx_http_parser_feed(&p, data + pos, (uint32_t)size - pos, &used, &ev);
        if (st != ASX_OK) {
            diff_error(asx_http_parse_error_str(asx_http_parser_error(&p)));
            return;
        }
        pos += used;
        if (ev.kind == ASX_HTTP_EVENT_BODY) {
            uint32_t i;
            for (i = 0; i < ev.len; i++) {
                fnv = (fnv ^ (uint64_t)ev.data[i]) * UINT64_C(0x00000100000001b3);
            }
            body_len += ev.len;
        } else if (ev.kind == ASX_HTTP_EVENT_MESSAGE_COMPLETE) {
            diff_complete(&p, kind, body_len, fnv, pos);
            return;
        } else if (ev.kind == ASX_HTTP_EVENT_NONE) {
            break;
        }
    }
    st = asx_http_parser_finish(&p, &ev);
    if (st != ASX_OK) {
        diff_error(asx_http_parse_error_str(asx_http_parser_error(&p)));
    } else if (ev.kind == ASX_HTTP_EVENT_MESSAGE_COMPLETE) {
        diff_complete(&p, kind, body_len, fnv, pos);
    } else {
        diff_error("no message in the input");
    }
}

static void fuzz_one(const uint8_t *data, size_t size) {
    diff_begin();
#if defined(ASX_DIFF_HTTP_REQUEST)
    diff_message(ASX_HTTP_PARSE_REQUEST, ASX_HTTP_GET, data, size);
#else
    {
        static const asx_http_method answered[3] = {ASX_HTTP_GET, ASX_HTTP_HEAD, ASX_HTTP_CONNECT};
        uint32_t i;
        putchar('{');
        for (i = 0; i < 3u; i++) {
            if (i > 0u) putchar(',');
            diff_put_cstr(asx_http_method_str(answered[i]));
            putchar(':');
            diff_message(ASX_HTTP_PARSE_RESPONSE, answered[i], data, size);
        }
        putchar('}');
    }
#endif
    diff_end();
}

#elif defined(ASX_DIFF_WS_FRAME)

#include <asx/net/websocket.h>

#ifndef ASX_FUZZ_STANDALONE
#error "the ASX_DIFF_* modes need ASX_FUZZ_STANDALONE (one outcome line per file)"
#endif

/* The payload limit of twin_run's ws-frame target (WS_MAX_PAYLOAD in
 * tools/twin_run/src/parsediff.rs): RFC 6455 leaves it to the endpoint, and
 * both sides apply this one so that a size policy hides nothing. */
#define DIFF_WS_MAX_PAYLOAD 65536u

static uint8_t g_ws_payload[DIFF_WS_MAX_PAYLOAD];

/* An error outcome. close_code is what the receiver fails the connection
 * with (0: nothing was violated yet, the frame is only cut short). */
static void diff_ws_error(const char *detail, unsigned close_code) {
    fputs("{\"kind\":\"error\",\"detail\":", stdout);
    diff_put_cstr(detail);
    if (close_code != 0u) printf(",\"close_code\":%u", close_code);
    putchar('}');
}

/* The first frame in the input as the receiver `role` decodes it, end of
 * input signalled. The header functions are the ones the engine runs
 * (ws_on_header), the payload is unmasked with asx_ws_mask, and a close
 * frame's payload is held to what ws_on_close_frame holds it to: the engine
 * also assembles messages and checks text UTF-8, which is above a frame. */
static void diff_ws_frame(asx_ws_role role, const uint8_t *data, size_t size) {
    asx_ws_frame_header h;
    asx_status st;
    uint16_t code;
    uint32_t plen;
    uint32_t i;
    uint64_t fnv = UINT64_C(0xcbf29ce484222325);

    if (size == 0u) {
        fputs("{\"kind\":\"empty\"}", stdout);
        return;
    }
    if (size > (1u << 20)) {
        diff_ws_error("input too large", 0u);
        return;
    }
    /* The engine is fed a header a byte at a time and judges its first two
     * bytes as they arrive, before the rest of the header can make it
     * malformed or incomplete, as asupersync's codec does. */
    if (size >= 2u) {
        st = asx_ws_header_decode(data, 2u, &h);
        if (st == ASX_OK || st == ASX_E_PENDING) {
            code = asx_ws_header_check(&h, role);
            if (code != 0u) {
                diff_ws_error("header-check", code);
                return;
            }
        }
    }
    st = asx_ws_header_decode(data, (uint32_t)size, &h);
    if (st == ASX_E_PENDING) {
        diff_ws_error("incomplete", 0u);
        return;
    }
    if (st != ASX_OK) {
        diff_ws_error("header-malformed", ASX_WS_CLOSE_PROTOCOL_ERROR);
        return;
    }
    /* A decoded header is entirely within the input (ASX_E_PENDING says
     * otherwise); a longer one has read its mask key from past the end. */
    if (h.header_len > size) fuzz_fail("ws header decode succeeded past the end of its input");
    code = asx_ws_header_check(&h, role);
    if (code != 0u) {
        diff_ws_error("header-check", code);
        return;
    }
    if (h.payload_len > DIFF_WS_MAX_PAYLOAD) {
        diff_ws_error("payload-too-large", ASX_WS_CLOSE_MESSAGE_TOO_BIG);
        return;
    }
    plen = (uint32_t)h.payload_len;
    if ((uint64_t)size - h.header_len < plen) {
        diff_ws_error("incomplete", 0u);
        return;
    }
    memcpy(g_ws_payload, data + h.header_len, plen);
    if (h.masked) asx_ws_mask(g_ws_payload, plen, h.mask_key, 0u);
    if (h.opcode == ASX_WS_OPCODE_CLOSE && plen > 0u) {
        if (plen == 1u) {
            diff_ws_error("close-payload-length", ASX_WS_CLOSE_PROTOCOL_ERROR);
            return;
        }
        if (!asx_ws_close_code_receivable(
                (uint16_t)(((unsigned)g_ws_payload[0] << 8) | (unsigned)g_ws_payload[1]))) {
            diff_ws_error("close-code", ASX_WS_CLOSE_PROTOCOL_ERROR);
            return;
        }
        if (!asx_ws_utf8_valid(g_ws_payload + 2, plen - 2u)) {
            diff_ws_error("close-reason-utf8", ASX_WS_CLOSE_PROTOCOL_ERROR);
            return;
        }
    }
    for (i = 0; i < plen; i++)
        fnv = (fnv ^ (uint64_t)g_ws_payload[i]) * UINT64_C(0x00000100000001b3);
    printf(
        "{\"kind\":\"complete\",\"fin\":%s,\"rsv\":%u,\"opcode\":%u,\"masked\":%s,\"mask_key\":\"",
        h.fin ? "true" : "false", (unsigned)h.rsv, (unsigned)h.opcode, h.masked ? "true" : "false");
    if (h.masked) {
        printf("%02x%02x%02x%02x", (unsigned)h.mask_key[0], (unsigned)h.mask_key[1],
               (unsigned)h.mask_key[2], (unsigned)h.mask_key[3]);
    }
    printf("\",\"payload_len\":%lu,\"payload_fnv\":\"%016llx\",\"consumed\":%lu}",
           (unsigned long)plen, (unsigned long long)fnv, (unsigned long)(h.header_len + plen));
}

static void fuzz_one(const uint8_t *data, size_t size) {
    diff_begin();
    fputs("{\"client\":", stdout);
    diff_ws_frame(ASX_WS_ROLE_CLIENT, data, size);
    fputs(",\"server\":", stdout);
    diff_ws_frame(ASX_WS_ROLE_SERVER, data, size);
    putchar('}');
    diff_end();
}

#elif defined(ASX_DIFF_RAPTORQ)

#include <asx/raptorq/raptorq.h>

#ifndef ASX_FUZZ_STANDALONE
#error "the ASX_DIFF_* modes need ASX_FUZZ_STANDALONE (one outcome line per file)"
#endif

/* The limits of twin_run's raptorq target (MAX_K, MAX_T, MAX_R in
 * tools/twin_run/src/rqoracle.rs): the harness's, not the library's. */
#define DIFF_RQ_MAX_K 2000u
#define DIFF_RQ_MAX_T 256u
#define DIFF_RQ_MAX_R 64u

/* The case `K T SEED R N MASK`: ASCII decimal, single spaces, one optional
 * final newline, parsed exactly as rqoracle.rs does. Returns 1 and the six
 * values, or 0 for anything else. */
static int diff_rq_parse(const uint8_t *data, size_t size, uint32_t out[6]) {
    size_t end = size;
    size_t pos = 0;
    uint32_t count = 0;

    if (end > 0u && data[end - 1u] == (uint8_t)'\n') end--;
    for (;;) {
        uint64_t value = 0;
        size_t digits = 0;
        while (pos < end && data[pos] != (uint8_t)' ') {
            if (data[pos] < (uint8_t)'0' || data[pos] > (uint8_t)'9') return 0;
            value = value * 10u + (uint64_t)(data[pos] - (uint8_t)'0');
            digits++;
            pos++;
            if (digits > 10u) return 0;
        }
        if (digits == 0u || value > UINT64_C(0xFFFFFFFF)) return 0;
        if (count == 6u) return 0;
        out[count++] = (uint32_t)value;
        if (pos >= end) break;
        pos++;                    /* the single space */
        if (pos >= end) return 0; /* a trailing space: an empty last token */
    }
    if (count != 6u) return 0;
    if (out[0] < 1u || out[0] > DIFF_RQ_MAX_K || out[1] < 1u || out[1] > DIFF_RQ_MAX_T ||
        out[3] > DIFF_RQ_MAX_R || out[4] > out[3]) {
        return 0;
    }
    /* The pool the decoder's symbols are drawn from holds K+N of them: the
     * repair symbols alone when MASK is odd, else all K+R. */
    return out[0] + out[4] <= ((out[5] & 1u) != 0u ? out[3] : out[0] + out[3]);
}

static uint64_t diff_rq_fnv(const uint8_t *p, size_t n) {
    uint64_t h = UINT64_C(0xcbf29ce484222325);
    size_t i;
    for (i = 0; i < n; i++) h = (h ^ (uint64_t)p[i]) * UINT64_C(0x00000100000001b3);
    return h;
}

/* The decode half of a case: a decoder is given the first K+N of the ESIs
 * 0..K+R-1 after the shuffle of rqoracle.rs (ESI data from the encoder) and
 * asked for the block. Prints "decode_fnv": the digest of what it recovered,
 * or "fail" when it reports the set insufficient. */
static void diff_rq_decode(const asx_raptorq_encoder *enc, uint32_t k, uint32_t t, uint32_t repairs,
                           uint32_t extra, uint32_t mask) {
    uint32_t first = (mask & 1u) != 0u ? k : 0u; /* an odd mask: repair symbols only */
    uint32_t total = k + repairs - first;
    uint32_t received = k + extra;
    uint32_t *esis = (uint32_t *)malloc((size_t)total * sizeof(uint32_t));
    uint8_t *sym = (uint8_t *)malloc(t);
    uint8_t *block = (uint8_t *)malloc((size_t)k * t);
    asx_raptorq_decoder dec;
    size_t ws_size = 0;
    void *ws = NULL;
    uint32_t s = mask;
    uint32_t i;
    asx_status st;

    if (esis == NULL || sym == NULL || block == NULL) fuzz_fail("raptorq decode allocation");
    for (i = 0; i < total; i++) esis[i] = first + i;
    for (i = total - 1u; i >= 1u; i--) {
        uint32_t j;
        uint32_t swap;
        s = s * 1664525u + 1013904223u;
        j = (s >> 8) % (i + 1u);
        swap = esis[i];
        esis[i] = esis[j];
        esis[j] = swap;
    }
    st = asx_raptorq_decoder_workspace_size(k, t, received, &ws_size);
    if (st == ASX_OK) ws = malloc(ws_size);
    if (st == ASX_OK && ws == NULL) fuzz_fail("raptorq decoder workspace allocation");
    if (st == ASX_OK) st = asx_raptorq_decoder_init(&dec, k, t, received, ws, ws_size);
    for (i = 0; st == ASX_OK && i < received; i++) {
        if (asx_raptorq_encoder_symbol(enc, esis[i], sym, t) != ASX_OK) {
            fuzz_fail("raptorq symbol of an initialized encoder");
        }
        st = asx_raptorq_decoder_add_symbol(&dec, esis[i], sym, t);
    }
    if (st == ASX_OK) st = asx_raptorq_decoder_decode(&dec, block, (size_t)k * t);
    if (st == ASX_OK) {
        printf(",\"decode_fnv\":\"%016llx\"",
               (unsigned long long)diff_rq_fnv(block, (size_t)k * t));
    } else {
        fputs(",\"decode_fnv\":\"fail\"", stdout);
    }
    free(ws);
    free(block);
    free(sym);
    free(esis);
}

/* One case: the K source symbols of the generator of test_raptorq.c, the
 * encoder, the digests of repair ESIs K .. K+R-1, and the decode. */
static void diff_rq_case(uint32_t k, uint32_t t, uint32_t seed, uint32_t repairs, uint32_t extra,
                         uint32_t mask) {
    asx_raptorq_encoder enc;
    size_t bytes = (size_t)k * t;
    size_t ws_size = 0;
    uint8_t *src = (uint8_t *)malloc(bytes);
    uint8_t *sym = (uint8_t *)malloc(t);
    void *ws = NULL;
    uint32_t s = seed;
    uint32_t i;
    int systematic_ok = 1;
    asx_status st;

    if (src == NULL || sym == NULL) fuzz_fail("raptorq case allocation");
    for (i = 0; i < bytes; i++) {
        s = s * 1664525u + 1013904223u;
        src[i] = (uint8_t)(s >> 24);
    }
    st = asx_raptorq_encoder_workspace_size(k, t, &ws_size);
    if (st == ASX_OK) ws = malloc(ws_size);
    if (st == ASX_OK && ws == NULL) fuzz_fail("raptorq workspace allocation");
    if (st == ASX_OK) st = asx_raptorq_encoder_init(&enc, k, t, src, bytes, ws, ws_size);
    if (st != ASX_OK) {
        fputs("{\"kind\":\"error\",\"detail\":", stdout);
        diff_put_cstr(asx_status_name(st));
        putchar('}');
        free(ws);
        free(sym);
        free(src);
        return;
    }
    for (i = 0; i < k; i++) {
        if (asx_raptorq_encoder_symbol(&enc, i, sym, t) != ASX_OK ||
            memcmp(sym, src + (size_t)i * t, t) != 0) {
            systematic_ok = 0;
        }
    }
    printf("{\"kind\":\"complete\",\"k\":%lu,\"t\":%lu,\"seed\":%lu,\"repairs\":%lu,"
           "\"extra\":%lu,\"mask\":%lu,\"source_fnv\":\"%016llx\",\"systematic_ok\":%s,"
           "\"repair_fnv\":[",
           (unsigned long)k, (unsigned long)t, (unsigned long)seed, (unsigned long)repairs,
           (unsigned long)extra, (unsigned long)mask, (unsigned long long)diff_rq_fnv(src, bytes),
           systematic_ok ? "true" : "false");
    for (i = 0; i < repairs; i++) {
        if (asx_raptorq_encoder_symbol(&enc, k + i, sym, t) != ASX_OK) {
            fuzz_fail("raptorq repair symbol of an initialized encoder");
        }
        printf("%s\"%016llx\"", i > 0u ? "," : "", (unsigned long long)diff_rq_fnv(sym, t));
    }
    fputs("]", stdout);
    diff_rq_decode(&enc, k, t, repairs, extra, mask);
    fputs("}", stdout);
    free(ws);
    free(sym);
    free(src);
}

static void fuzz_one(const uint8_t *data, size_t size) {
    uint32_t c[6];
    diff_begin();
    if (size == 0u) {
        fputs("{\"kind\":\"empty\"}", stdout);
    } else if (!diff_rq_parse(data, size, c)) {
        diff_error("not a case");
    } else {
        diff_rq_case(c[0], c[1], c[2], c[3], c[4], c[5]);
    }
    diff_end();
}

#else
#error "define one ASX_FUZZ_<TARGET> or ASX_DIFF_<TARGET> (see the header comment)"
#endif

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    fuzz_one(data, size);
    return 0;
}

#ifdef ASX_FUZZ_STANDALONE
/* Run each file named on the command line once (regression inputs). */
int main(int argc, char **argv) {
    int i;
    for (i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        uint8_t *buf;
        long len;
        if (f == NULL) {
            fprintf(stderr, "fuzz_parsers: cannot open %s\n", argv[i]);
            return 2;
        }
        if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
            fclose(f);
            return 2;
        }
        buf = (uint8_t *)malloc((size_t)len + 1u);
        if (buf == NULL || fread(buf, 1, (size_t)len, f) != (size_t)len) {
            free(buf);
            fclose(f);
            return 2;
        }
        fclose(f);
        g_input_name = argv[i];
        (void)LLVMFuzzerTestOneInput(buf, (size_t)len);
        free(buf);
    }
    return 0;
}
#endif
