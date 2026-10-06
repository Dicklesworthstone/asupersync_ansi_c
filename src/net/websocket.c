/*
 * websocket.c — RFC 6455 WebSocket codec, handshake, and sans-IO engine,
 *               plus a thin adapter over asx_tcp_stream
 *
 * The engine is a pure state machine over byte buffers: received bytes are
 * fed in, frames are parsed incrementally (payloads stream straight into the
 * reassembly or control buffer and are unmasked in place), and every byte to
 * transmit is appended to an output buffer the caller drains. No I/O, no
 * allocation, no globals. The adapter at the bottom owns a small slot table
 * and pumps engines over the in-memory/real TCP stream poll API.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx_config.h>
#include <asx/net/websocket.h>
#include <asx/portable.h>
#include <asx/security/crypto.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static uint32_t ws_min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }

static int ws_opcode_known(uint32_t op) {
    return op == ASX_WS_OPCODE_CONTINUATION || op == ASX_WS_OPCODE_TEXT ||
           op == ASX_WS_OPCODE_BINARY || op == ASX_WS_OPCODE_CLOSE || op == ASX_WS_OPCODE_PING ||
           op == ASX_WS_OPCODE_PONG;
}

int asx_ws_opcode_is_control(uint8_t opcode) { return (opcode & 0x08u) != 0u; }

int asx_ws_close_code_sendable(uint16_t code) {
    return (code >= 1000u && code <= 1003u) || (code >= 1007u && code <= 1014u) ||
           (code >= 3000u && code <= 4999u);
}

int asx_ws_close_code_receivable(uint16_t code) {
    return (code >= 1000u && code <= 1003u) || (code >= 1007u && code <= 1014u) ||
           (code >= 1016u && code <= 4999u);
}

/* ------------------------------------------------------------------ */
/* Frame codec                                                         */
/* ------------------------------------------------------------------ */

static uint32_t ws_header_size(uint64_t payload_len, int masked) {
    uint32_t n = 2u;
    if (payload_len > 0xFFFFu) {
        n += 8u;
    } else if (payload_len > 125u) {
        n += 2u;
    }
    return masked ? n + 4u : n;
}

asx_status asx_ws_header_encode(const asx_ws_frame_header *h, uint8_t *out, uint32_t cap,
                                uint32_t *out_len) {
    uint8_t tmp[ASX_WS_MAX_FRAME_HEADER];
    uint32_t n = 0u;
    uint32_t mask_bit;

    if (h == NULL || out == NULL || out_len == NULL) return ASX_E_INVALID_ARGUMENT;
    if (h->opcode > 0x0Fu || h->rsv > 7u) return ASX_E_INVALID_ARGUMENT;
    if (h->payload_len > UINT64_C(0x7FFFFFFFFFFFFFFF)) return ASX_E_INVALID_ARGUMENT;

    tmp[n++] = (uint8_t)((h->fin ? 0x80u : 0u) | ((uint32_t)h->rsv << 4) | (uint32_t)h->opcode);
    mask_bit = h->masked ? 0x80u : 0u;
    if (h->payload_len <= 125u) {
        tmp[n++] = (uint8_t)(mask_bit | (uint32_t)h->payload_len);
    } else if (h->payload_len <= 0xFFFFu) {
        tmp[n++] = (uint8_t)(mask_bit | 126u);
        asx_store_be_u16(tmp + n, (uint16_t)h->payload_len);
        n += 2u;
    } else {
        tmp[n++] = (uint8_t)(mask_bit | 127u);
        asx_store_be_u64(tmp + n, h->payload_len);
        n += 8u;
    }
    if (h->masked) {
        memcpy(tmp + n, h->mask_key, 4u);
        n += 4u;
    }
    if (n > cap) return ASX_E_BUFFER_TOO_SMALL;
    memcpy(out, tmp, n);
    *out_len = n;
    return ASX_OK;
}

asx_status asx_ws_header_decode(const uint8_t *in, uint32_t len, asx_ws_frame_header *h) {
    uint32_t len7;
    uint32_t need;
    uint32_t pos;

    if (h == NULL || (in == NULL && len > 0u)) return ASX_E_INVALID_ARGUMENT;
    if (len < 2u) return ASX_E_PENDING;

    len7 = (uint32_t)in[1] & 0x7Fu;
    need = 2u + (len7 == 126u ? 2u : (len7 == 127u ? 8u : 0u)) + ((in[1] & 0x80u) ? 4u : 0u);
    if (len < need) return ASX_E_PENDING;

    memset(h, 0, sizeof(*h));
    h->fin = (uint8_t)((in[0] >> 7) & 1u);
    h->rsv = (uint8_t)((in[0] >> 4) & 7u);
    h->opcode = (uint8_t)(in[0] & 0x0Fu);
    h->masked = (uint8_t)((in[1] >> 7) & 1u);
    pos = 2u;
    if (len7 <= 125u) {
        h->payload_len = len7;
    } else if (len7 == 126u) {
        uint16_t v = asx_load_be_u16(in + 2);
        if (v <= 125u) return ASX_E_INVALID_ARGUMENT; /* non-minimal */
        h->payload_len = v;
        pos += 2u;
    } else {
        uint64_t v = asx_load_be_u64(in + 2);
        if ((v >> 63) != 0u) return ASX_E_INVALID_ARGUMENT; /* MSB must be 0 */
        if (v <= 0xFFFFu) return ASX_E_INVALID_ARGUMENT;    /* non-minimal */
        h->payload_len = v;
        pos += 8u;
    }
    if (h->masked) {
        memcpy(h->mask_key, in + pos, 4u);
        pos += 4u;
    }
    h->header_len = pos;
    return ASX_OK;
}

uint16_t asx_ws_header_check(const asx_ws_frame_header *h, asx_ws_role receiver_role) {
    if (h == NULL) return ASX_WS_CLOSE_PROTOCOL_ERROR;
    if (h->rsv != 0u) return ASX_WS_CLOSE_PROTOCOL_ERROR;
    if (!ws_opcode_known(h->opcode)) return ASX_WS_CLOSE_PROTOCOL_ERROR;
    if (asx_ws_opcode_is_control(h->opcode)) {
        if (!h->fin) return ASX_WS_CLOSE_PROTOCOL_ERROR;
        if (h->payload_len > ASX_WS_CONTROL_MAX) return ASX_WS_CLOSE_PROTOCOL_ERROR;
    }
    if (receiver_role == ASX_WS_ROLE_SERVER && !h->masked) return ASX_WS_CLOSE_PROTOCOL_ERROR;
    if (receiver_role == ASX_WS_ROLE_CLIENT && h->masked) return ASX_WS_CLOSE_PROTOCOL_ERROR;
    return 0u;
}

void asx_ws_mask(uint8_t *data, uint32_t len, const uint8_t key[4], uint64_t offset) {
    uint32_t i;

    if (data == NULL || key == NULL) return;
    for (i = 0u; i < len; i++) {
        data[i] = (uint8_t)(data[i] ^ key[(uint32_t)((offset + i) & 3u)]);
    }
}

asx_status asx_ws_frame_encode(asx_ws_opcode opcode, int fin, const uint8_t *mask_key,
                               const uint8_t *payload, uint32_t payload_len, uint8_t *out,
                               uint32_t cap, uint32_t *out_len) {
    asx_ws_frame_header h;
    uint32_t hlen = 0u;
    uint32_t op = (uint32_t)opcode;
    asx_status st;

    if (out == NULL || out_len == NULL) return ASX_E_INVALID_ARGUMENT;
    if (payload == NULL && payload_len > 0u) return ASX_E_INVALID_ARGUMENT;
    if (!ws_opcode_known(op)) return ASX_E_INVALID_ARGUMENT;
    if (asx_ws_opcode_is_control((uint8_t)op) && (!fin || payload_len > ASX_WS_CONTROL_MAX)) {
        return ASX_E_INVALID_ARGUMENT;
    }

    memset(&h, 0, sizeof(h));
    h.fin = (uint8_t)(fin ? 1u : 0u);
    h.opcode = (uint8_t)op;
    h.payload_len = payload_len;
    if (mask_key != NULL) {
        h.masked = 1u;
        memcpy(h.mask_key, mask_key, 4u);
    }
    st = asx_ws_header_encode(&h, out, cap, &hlen);
    if (st != ASX_OK) return st;
    if (payload_len > cap - hlen) return ASX_E_BUFFER_TOO_SMALL;
    if (payload_len > 0u) {
        memcpy(out + hlen, payload, payload_len);
        if (mask_key != NULL) asx_ws_mask(out + hlen, payload_len, mask_key, 0u);
    }
    *out_len = hlen + payload_len;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Strict UTF-8 validation (RFC 3629)                                  */
/* ------------------------------------------------------------------ */

void asx_ws_utf8_init(asx_ws_utf8 *v) {
    if (v == NULL) return;
    v->need = 0u;
    v->lo = 0x80u;
    v->hi = 0xBFu;
    v->failed = 0u;
}

int asx_ws_utf8_feed(asx_ws_utf8 *v, const uint8_t *data, size_t len) {
    size_t i;

    if (v == NULL) return 0;
    if (data == NULL && len > 0u) {
        v->failed = 1u;
        return 0;
    }
    for (i = 0u; i < len && !v->failed; i++) {
        uint8_t b = data[i];
        if (v->need == 0u) {
            if (b <= 0x7Fu) continue;
            v->lo = 0x80u;
            v->hi = 0xBFu;
            if (b >= 0xC2u && b <= 0xDFu) {
                v->need = 1u;
            } else if (b == 0xE0u) {
                v->need = 2u;
                v->lo = 0xA0u; /* no overlongs */
            } else if ((b >= 0xE1u && b <= 0xECu) || b == 0xEEu || b == 0xEFu) {
                v->need = 2u;
            } else if (b == 0xEDu) {
                v->need = 2u;
                v->hi = 0x9Fu; /* no UTF-16 surrogates */
            } else if (b == 0xF0u) {
                v->need = 3u;
                v->lo = 0x90u; /* no overlongs */
            } else if (b >= 0xF1u && b <= 0xF3u) {
                v->need = 3u;
            } else if (b == 0xF4u) {
                v->need = 3u;
                v->hi = 0x8Fu; /* <= U+10FFFF */
            } else {
                v->failed = 1u;
            }
        } else {
            if (b < v->lo || b > v->hi) {
                v->failed = 1u;
            } else {
                v->need--;
                v->lo = 0x80u;
                v->hi = 0xBFu;
            }
        }
    }
    return v->failed ? 0 : 1;
}

int asx_ws_utf8_complete(const asx_ws_utf8 *v) {
    if (v == NULL) return 0;
    return !v->failed && v->need == 0u;
}

int asx_ws_utf8_valid(const uint8_t *data, size_t len) {
    asx_ws_utf8 v;
    asx_ws_utf8_init(&v);
    if (!asx_ws_utf8_feed(&v, data, len)) return 0;
    return asx_ws_utf8_complete(&v);
}

/* ------------------------------------------------------------------ */
/* HTTP/1.1 text helpers for the opening handshake                     */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *p;
    size_t n;
} ws_span;

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    int overflow;
} ws_sb;

static void ws_sb_putn(ws_sb *sb, const char *s, size_t n) {
    if (sb->overflow) return;
    if (n >= sb->cap - sb->len) {
        sb->overflow = 1;
        return;
    }
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
}

static void ws_sb_put(ws_sb *sb, const char *s) { ws_sb_putn(sb, s, strlen(s)); }

static unsigned char ws_lower(char c) {
    unsigned char u = (unsigned char)c;
    return (u >= (unsigned char)'A' && u <= (unsigned char)'Z') ? (unsigned char)(u + 32u) : u;
}

static int ws_span_eq(ws_span s, const char *lit, int case_insensitive) {
    size_t i;
    size_t n = strlen(lit);

    if (s.n != n) return 0;
    for (i = 0u; i < n; i++) {
        if (case_insensitive) {
            if (ws_lower(s.p[i]) != ws_lower(lit[i])) return 0;
        } else if (s.p[i] != lit[i]) {
            return 0;
        }
    }
    return 1;
}

static int ws_span_eq_span(ws_span a, ws_span b) {
    return a.n == b.n && (a.n == 0u || memcmp(a.p, b.p, a.n) == 0);
}

static ws_span ws_trim(ws_span s) {
    while (s.n > 0u && (s.p[0] == ' ' || s.p[0] == '\t')) {
        s.p++;
        s.n--;
    }
    while (s.n > 0u && (s.p[s.n - 1u] == ' ' || s.p[s.n - 1u] == '\t')) s.n--;
    return s;
}

/* Split the next comma-separated element off *list (trimmed, may be empty).
 * Returns 0 once the list is exhausted. */
static int ws_list_next(ws_span *list, ws_span *item) {
    size_t i;

    if (list->p == NULL) return 0;
    for (i = 0u; i < list->n && list->p[i] != ','; i++) {}
    item->p = list->p;
    item->n = i;
    *item = ws_trim(*item);
    if (i < list->n) {
        list->p += i + 1u;
        list->n -= i + 1u;
    } else {
        list->p = NULL;
        list->n = 0u;
    }
    return 1;
}

static int ws_list_contains(ws_span list, ws_span token, int case_insensitive) {
    ws_span item;

    while (ws_list_next(&list, &item)) {
        if (item.n == 0u) continue;
        if (case_insensitive) {
            size_t i;
            int same = (item.n == token.n);
            for (i = 0u; same && i < item.n; i++) {
                if (ws_lower(item.p[i]) != ws_lower(token.p[i])) same = 0;
            }
            if (same) return 1;
        } else if (ws_span_eq_span(item, token)) {
            return 1;
        }
    }
    return 0;
}

static ws_span ws_cstr_span(const char *s) {
    ws_span sp;
    sp.p = s;
    sp.n = (s != NULL) ? strlen(s) : 0u;
    return sp;
}

/* Field values we emit must not smuggle CR/LF (header injection). */
static int ws_field_safe(const char *s) {
    if (s == NULL) return 1;
    for (; *s != '\0'; s++) {
        if (*s == '\r' || *s == '\n') return 0;
    }
    return 1;
}

static int ws_is_tchar(char c) {
    unsigned char u = (unsigned char)c;
    if (u >= (unsigned char)'0' && u <= (unsigned char)'9') return 1;
    if (u >= (unsigned char)'a' && u <= (unsigned char)'z') return 1;
    if (u >= (unsigned char)'A' && u <= (unsigned char)'Z') return 1;
    return strchr("!#$%&'*+-.^_`|~", (int)u) != NULL && u != 0u;
}

/* Locate the end of the header block ("\r\n\r\n"); *end is the index just
 * past it. */
static int ws_find_header_end(const uint8_t *buf, uint32_t len, uint32_t *end) {
    uint32_t i;

    if (len < 4u) return 0;
    for (i = 0u; i + 3u < len; i++) {
        if (buf[i] == '\r' && buf[i + 1u] == '\n' && buf[i + 2u] == '\r' && buf[i + 3u] == '\n') {
            *end = i + 4u;
            return 1;
        }
    }
    return 0;
}

typedef struct {
    ws_span start_line;
    const char *fields;     /* first header field line */
    const char *fields_end; /* final empty line */
} ws_http_head;

/* Split a complete head (ending in CRLFCRLF) into its start line and field
 * block, and syntax-check every field line. */
static int ws_http_head_parse(const char *text, uint32_t len, ws_http_head *head) {
    const char *end = text + len - 2u; /* points at the final CRLF */
    const char *p = text;
    const char *line_end;

    line_end = p;
    while (line_end < end && !(line_end[0] == '\r' && line_end[1] == '\n')) line_end++;
    head->start_line.p = p;
    head->start_line.n = (size_t)(line_end - p);
    head->fields = line_end + 2;
    head->fields_end = end;
    if (head->fields > end) head->fields = end;

    /* Validate field lines: name ":" OWS value OWS CRLF, no obs-fold. */
    p = head->fields;
    while (p < end) {
        const char *q = p;
        const char *colon = NULL;
        while (q < end && !(q[0] == '\r' && q[1] == '\n')) {
            if (*q == '\r' || *q == '\n' || *q == '\0') return 0;
            if (colon == NULL && *q == ':') colon = q;
            q++;
        }
        if (q >= end) return 0; /* every field line ends with its own CRLF */
        if (colon == NULL || colon == p) return 0;
        {
            const char *c;
            for (c = p; c < colon; c++) {
                if (!ws_is_tchar(*c)) return 0;
            }
        }
        p = q + 2;
    }
    {
        size_t i;
        for (i = 0u; i < head->start_line.n; i++) {
            char c = head->start_line.p[i];
            if (c == '\0' || c == '\n' || c == '\r') return 0;
        }
    }
    return 1;
}

/* Iterate field instances named `name` (case-insensitive). *cursor starts
 * at head->fields. Returns 1 with the trimmed value, 0 when exhausted. */
static int ws_http_field_next(const ws_http_head *head, const char **cursor, const char *name,
                              ws_span *value) {
    const char *p = *cursor;

    while (p < head->fields_end) {
        const char *q = p;
        const char *colon = NULL;
        ws_span fname;
        while (q < head->fields_end && !(q[0] == '\r' && q[1] == '\n')) {
            if (colon == NULL && *q == ':') colon = q;
            q++;
        }
        *cursor = q + 2;
        if (colon != NULL) {
            fname.p = p;
            fname.n = (size_t)(colon - p);
            if (ws_span_eq(fname, name, 1)) {
                value->p = colon + 1;
                value->n = (size_t)(q - (colon + 1));
                *value = ws_trim(*value);
                return 1;
            }
        }
        p = *cursor;
    }
    return 0;
}

static uint32_t ws_http_field_count(const ws_http_head *head, const char *name) {
    const char *cursor = head->fields;
    ws_span value;
    uint32_t n = 0u;
    while (ws_http_field_next(head, &cursor, name, &value)) n++;
    return n;
}

static int ws_http_field_single(const ws_http_head *head, const char *name, ws_span *value) {
    const char *cursor = head->fields;
    if (ws_http_field_count(head, name) != 1u) return 0;
    return ws_http_field_next(head, &cursor, name, value);
}

static int ws_http_field_has_token(const ws_http_head *head, const char *name, const char *token) {
    const char *cursor = head->fields;
    ws_span value;
    ws_span tok = ws_cstr_span(token);
    while (ws_http_field_next(head, &cursor, name, &value)) {
        if (ws_list_contains(value, tok, 1)) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Handshake helpers (public)                                          */
/* ------------------------------------------------------------------ */

asx_status asx_ws_compute_accept(const char *key, size_t key_len, char out[ASX_WS_ACCEPT_LEN + 1]) {
    static const char guid[] = ASX_WS_GUID;
    asx_sha1_ctx sha;
    uint8_t digest[ASX_SHA1_DIGEST_SIZE];
    size_t n = 0u;

    if (key == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    asx_sha1_init(&sha);
    asx_sha1_update(&sha, key, key_len);
    asx_sha1_update(&sha, guid, sizeof(guid) - 1u);
    asx_sha1_final(&sha, digest);
    return asx_base64_encode(ASX_BASE64_STANDARD, digest, sizeof(digest), out,
                             ASX_WS_ACCEPT_LEN + 1u, &n);
}

/* A Sec-WebSocket-Key must be the base64 of exactly 16 bytes. */
static int ws_key_valid(ws_span key) {
    uint8_t nonce[16];
    size_t n = 0u;

    if (key.n != ASX_WS_KEY_LEN) return 0;
    if (asx_base64_decode(ASX_BASE64_STANDARD, key.p, key.n, nonce, sizeof(nonce), &n) != ASX_OK) {
        return 0;
    }
    return n == 16u;
}

asx_status asx_ws_handshake_build_request(const char *host, const char *path, const char *origin,
                                          const char *protocols, const char *key, char *out,
                                          size_t cap, size_t *out_len) {
    ws_sb sb;

    if (out_len != NULL) *out_len = 0u;
    if (out == NULL || cap == 0u || host == NULL || host[0] == '\0' || key == NULL) {
        return ASX_E_INVALID_ARGUMENT;
    }
    if (!ws_key_valid(ws_cstr_span(key))) return ASX_E_INVALID_ARGUMENT;
    if (path == NULL || path[0] == '\0') path = "/";
    if (!ws_field_safe(host) || !ws_field_safe(path) || !ws_field_safe(origin) ||
        !ws_field_safe(protocols) || strchr(path, ' ') != NULL) {
        return ASX_E_INVALID_ARGUMENT;
    }

    sb.buf = out;
    sb.cap = cap;
    sb.len = 0u;
    sb.overflow = 0;
    out[0] = '\0';
    ws_sb_put(&sb, "GET ");
    ws_sb_put(&sb, path);
    ws_sb_put(&sb, " HTTP/1.1\r\nHost: ");
    ws_sb_put(&sb, host);
    ws_sb_put(&sb, "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ");
    ws_sb_put(&sb, key);
    ws_sb_put(&sb, "\r\nSec-WebSocket-Version: 13\r\n");
    if (origin != NULL && origin[0] != '\0') {
        ws_sb_put(&sb, "Origin: ");
        ws_sb_put(&sb, origin);
        ws_sb_put(&sb, "\r\n");
    }
    if (protocols != NULL && protocols[0] != '\0') {
        ws_sb_put(&sb, "Sec-WebSocket-Protocol: ");
        ws_sb_put(&sb, protocols);
        ws_sb_put(&sb, "\r\n");
    }
    ws_sb_put(&sb, "\r\n");
    if (sb.overflow) {
        out[0] = '\0';
        return ASX_E_BUFFER_TOO_SMALL;
    }
    if (out_len != NULL) *out_len = sb.len;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Engine: output buffer                                               */
/* ------------------------------------------------------------------ */

static uint32_t ws_tx_free(const asx_ws_engine *ws) { return ASX_WS_TX_CAPACITY - ws->tx_len; }

/* Make room for n contiguous bytes at the tail; caller checked free space. */
static uint8_t *ws_tx_reserve(asx_ws_engine *ws, uint32_t n) {
    if (ws->tx_start + ws->tx_len + n > ASX_WS_TX_CAPACITY) {
        memmove(ws->tx_buf, ws->tx_buf + ws->tx_start, ws->tx_len);
        ws->tx_start = 0u;
    }
    return ws->tx_buf + ws->tx_start + ws->tx_len;
}

static asx_status ws_queue_raw(asx_ws_engine *ws, const void *data, uint32_t n) {
    uint8_t *dst;
    if (n > ws_tx_free(ws)) return ASX_E_WOULD_BLOCK;
    dst = ws_tx_reserve(ws, n);
    memcpy(dst, data, n);
    ws->tx_len += n;
    return ASX_OK;
}

/* Encode one frame into the output buffer, masking for clients. */
static asx_status ws_queue_frame(asx_ws_engine *ws, uint32_t opcode, int fin,
                                 const uint8_t *payload, uint32_t len) {
    uint8_t key[4];
    const uint8_t *keyp = NULL;
    int masked = (ws->role == ASX_WS_ROLE_CLIENT);
    uint32_t needed;
    uint32_t written = 0u;
    uint8_t *dst;
    asx_status st;

    needed = ws_header_size(len, masked);
    if (len > ASX_WS_TX_CAPACITY || needed > ASX_WS_TX_CAPACITY - len) {
        return ASX_E_BUFFER_TOO_SMALL;
    }
    needed += len;
    if (needed > ws_tx_free(ws)) return ASX_E_WOULD_BLOCK;
    if (masked) {
        st = ws->random_fn(ws->random_ctx, key, sizeof(key));
        if (st != ASX_OK) return st;
        keyp = key;
    }
    dst = ws_tx_reserve(ws, needed);
    st = asx_ws_frame_encode((asx_ws_opcode)opcode, fin, keyp, payload, len, dst, needed, &written);
    if (st != ASX_OK) return st;
    ws->tx_len += written;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Engine: lifecycle                                                   */
/* ------------------------------------------------------------------ */

static asx_status ws_runtime_entropy(void *ctx, uint64_t *out) {
    (void)ctx;
    return asx_runtime_random_u64(out);
}

/* Default random source: runtime entropy hook whitened via SHA-256. */
static asx_status ws_default_random(void *ctx, uint8_t *out, uint32_t len) {
    (void)ctx;
    return asx_crypto_random_bytes(ws_runtime_entropy, NULL, out, len);
}

void asx_ws_config_init(asx_ws_config *cfg, asx_ws_role role) {
    if (cfg == NULL) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->role = role;
    cfg->max_message = ASX_WS_MAX_MESSAGE;
}

asx_status asx_ws_engine_init(asx_ws_engine *ws, const asx_ws_config *cfg) {
    size_t plen;

    if (ws == NULL || cfg == NULL) return ASX_E_INVALID_ARGUMENT;
    if (cfg->role != ASX_WS_ROLE_CLIENT && cfg->role != ASX_WS_ROLE_SERVER) {
        return ASX_E_INVALID_ARGUMENT;
    }
    plen = (cfg->protocols != NULL) ? strlen(cfg->protocols) : 0u;
    if (plen >= ASX_WS_PROTOCOLS_MAX || !ws_field_safe(cfg->protocols)) {
        return ASX_E_INVALID_ARGUMENT;
    }

    memset(ws, 0, sizeof(*ws));
    ws->role = cfg->role;
    ws->max_message = (cfg->max_message == 0u || cfg->max_message > ASX_WS_MAX_MESSAGE)
                          ? ASX_WS_MAX_MESSAGE
                          : cfg->max_message;
    ws->random_fn = (cfg->random_fn != NULL) ? cfg->random_fn : ws_default_random;
    ws->random_ctx = (cfg->random_fn != NULL) ? cfg->random_ctx : NULL;
    if (plen > 0u) memcpy(ws->protocols, cfg->protocols, plen + 1u);
    asx_ws_utf8_init(&ws->rx_utf8);
    asx_ws_utf8_init(&ws->tx_utf8);

    if (cfg->skip_handshake) {
        ws->state = ASX_WS_STATE_OPEN;
        return ASX_OK;
    }
    ws->state = ASX_WS_STATE_CONNECTING;

    if (ws->role == ASX_WS_ROLE_CLIENT) {
        uint8_t nonce[16];
        size_t klen = 0u;
        size_t rlen = 0u;
        size_t cap =
            ASX_WS_HANDSHAKE_MAX < ASX_WS_TX_CAPACITY ? ASX_WS_HANDSHAKE_MAX : ASX_WS_TX_CAPACITY;
        asx_status st = ws->random_fn(ws->random_ctx, nonce, sizeof(nonce));
        if (st != ASX_OK) {
            ws->state = ASX_WS_STATE_CLOSED;
            return st;
        }
        st = asx_base64_encode(ASX_BASE64_STANDARD, nonce, sizeof(nonce), ws->key, sizeof(ws->key),
                               &klen);
        asx_crypto_secure_zero(nonce, sizeof(nonce));
        if (st == ASX_OK) {
            st = asx_ws_handshake_build_request(cfg->host, cfg->path, cfg->origin, cfg->protocols,
                                                ws->key, (char *)ws->tx_buf, cap, &rlen);
        }
        if (st != ASX_OK) {
            ws->state = ASX_WS_STATE_CLOSED;
            return st;
        }
        ws->tx_start = 0u;
        ws->tx_len = (uint32_t)rlen;
    }
    return ASX_OK;
}

asx_ws_state asx_ws_engine_state(const asx_ws_engine *ws) {
    return ws == NULL ? ASX_WS_STATE_CLOSED : ws->state;
}

uint16_t asx_ws_engine_close_code(const asx_ws_engine *ws) {
    return ws == NULL ? 0u : ws->close_code;
}

const char *asx_ws_engine_close_reason(const asx_ws_engine *ws) {
    return ws == NULL ? "" : ws->close_reason;
}

uint16_t asx_ws_engine_error_code(const asx_ws_engine *ws) {
    return ws == NULL ? 0u : ws->error_code;
}

const char *asx_ws_engine_protocol(const asx_ws_engine *ws) {
    return ws == NULL ? "" : ws->protocol;
}

uint32_t asx_ws_engine_output(const asx_ws_engine *ws, const uint8_t **data) {
    if (data != NULL) *data = NULL;
    if (ws == NULL || ws->tx_len == 0u) return 0u;
    if (data != NULL) *data = ws->tx_buf + ws->tx_start;
    return ws->tx_len;
}

asx_status asx_ws_engine_consume_output(asx_ws_engine *ws, uint32_t n) {
    if (ws == NULL || n > ws->tx_len) return ASX_E_INVALID_ARGUMENT;
    ws->tx_start += n;
    ws->tx_len -= n;
    if (ws->tx_len == 0u) ws->tx_start = 0u;
    return ASX_OK;
}

void asx_ws_engine_transport_closed(asx_ws_engine *ws) {
    if (ws == NULL || ws->state == ASX_WS_STATE_CLOSED) return;
    if (!ws->close_received) ws->close_code = ASX_WS_CLOSE_ABNORMAL;
    ws->msg_in_progress = 0u;
    ws->state = ASX_WS_STATE_CLOSED;
}

/* Fail the WebSocket connection (RFC 6455 §7.1.7): send a close frame with
 * the error code when possible, then stop processing input. */
static asx_status ws_fail(asx_ws_engine *ws, uint16_t code) {
    if (!ws->failed) {
        if (!ws->close_sent && ws->state != ASX_WS_STATE_CONNECTING) {
            uint8_t payload[2];
            asx_store_be_u16(payload, code);
            if (ws_queue_frame(ws, ASX_WS_OPCODE_CLOSE, 1, payload, 2u) == ASX_OK) {
                ws->close_sent = 1u;
            }
        }
        ws->failed = 1u;
        ws->error_code = code;
        ws->msg_in_progress = 0u;
        ws->state = ASX_WS_STATE_CLOSED;
    }
    return ASX_E_DISCONNECTED;
}

/* ------------------------------------------------------------------ */
/* Engine: opening handshake                                           */
/* ------------------------------------------------------------------ */

static asx_status ws_server_reject(asx_ws_engine *ws, int version_mismatch) {
    static const char bad_request[] = "HTTP/1.1 400 Bad Request\r\n"
                                      "Connection: close\r\n"
                                      "Content-Length: 0\r\n\r\n";
    static const char upgrade_required[] = "HTTP/1.1 426 Upgrade Required\r\n"
                                           "Sec-WebSocket-Version: 13\r\n"
                                           "Connection: close\r\n"
                                           "Content-Length: 0\r\n\r\n";
    if (version_mismatch) {
        (void)ws_queue_raw(ws, upgrade_required, (uint32_t)(sizeof(upgrade_required) - 1u));
    } else {
        (void)ws_queue_raw(ws, bad_request, (uint32_t)(sizeof(bad_request) - 1u));
    }
    return ws_fail(ws, ASX_WS_CLOSE_PROTOCOL_ERROR);
}

/* Pick the first client-offered subprotocol (client preference order) that
 * the server supports. */
static void ws_server_select_protocol(asx_ws_engine *ws, const ws_http_head *head) {
    const char *cursor = head->fields;
    ws_span value;
    ws_span supported = ws_cstr_span(ws->protocols);

    if (supported.n == 0u) return;
    while (ws_http_field_next(head, &cursor, "Sec-WebSocket-Protocol", &value)) {
        ws_span item;
        while (ws_list_next(&value, &item)) {
            if (item.n == 0u || item.n >= sizeof(ws->protocol)) continue;
            if (ws_list_contains(supported, item, 0)) {
                memcpy(ws->protocol, item.p, item.n);
                ws->protocol[item.n] = '\0';
                return;
            }
        }
    }
}

static asx_status ws_server_handshake(asx_ws_engine *ws, uint32_t head_len) {
    ws_http_head head;
    ws_span sl;
    ws_span method;
    ws_span target;
    ws_span version;
    ws_span value;
    ws_span key;
    char accept[ASX_WS_ACCEPT_LEN + 1];
    char response[512];
    ws_sb sb;
    size_t i;
    size_t sp1;
    size_t sp2;

    if (!ws_http_head_parse((const char *)ws->hs_buf, head_len, &head)) {
        return ws_server_reject(ws, 0);
    }

    /* Request line: GET SP request-target SP HTTP/1.1 */
    sl = head.start_line;
    for (sp1 = 0u; sp1 < sl.n && sl.p[sp1] != ' '; sp1++) {}
    for (sp2 = sp1 + 1u; sp2 < sl.n && sl.p[sp2] != ' '; sp2++) {}
    if (sp1 >= sl.n || sp2 >= sl.n) return ws_server_reject(ws, 0);
    method.p = sl.p;
    method.n = sp1;
    target.p = sl.p + sp1 + 1u;
    target.n = sp2 - sp1 - 1u;
    version.p = sl.p + sp2 + 1u;
    version.n = sl.n - sp2 - 1u;
    if (!ws_span_eq(method, "GET", 0) || !ws_span_eq(version, "HTTP/1.1", 0) || target.n == 0u) {
        return ws_server_reject(ws, 0);
    }
    for (i = 0u; i < target.n; i++) {
        if ((unsigned char)target.p[i] <= 0x20u) return ws_server_reject(ws, 0);
    }

    if (ws_http_field_count(&head, "Host") != 1u) return ws_server_reject(ws, 0);
    if (!ws_http_field_has_token(&head, "Upgrade", "websocket")) return ws_server_reject(ws, 0);
    if (!ws_http_field_has_token(&head, "Connection", "upgrade")) return ws_server_reject(ws, 0);
    if (ws_http_field_count(&head, "Sec-WebSocket-Version") == 0u) return ws_server_reject(ws, 0);
    if (!ws_http_field_single(&head, "Sec-WebSocket-Version", &value) ||
        !ws_span_eq(value, "13", 0)) {
        return ws_server_reject(ws, 1);
    }
    if (!ws_http_field_single(&head, "Sec-WebSocket-Key", &key) || !ws_key_valid(key)) {
        return ws_server_reject(ws, 0);
    }

    ws_server_select_protocol(ws, &head);
    if (asx_ws_compute_accept(key.p, key.n, accept) != ASX_OK) return ws_server_reject(ws, 0);

    sb.buf = response;
    sb.cap = sizeof(response);
    sb.len = 0u;
    sb.overflow = 0;
    response[0] = '\0';
    ws_sb_put(&sb, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                   "Connection: Upgrade\r\nSec-WebSocket-Accept: ");
    ws_sb_put(&sb, accept);
    ws_sb_put(&sb, "\r\n");
    if (ws->protocol[0] != '\0') {
        ws_sb_put(&sb, "Sec-WebSocket-Protocol: ");
        ws_sb_put(&sb, ws->protocol);
        ws_sb_put(&sb, "\r\n");
    }
    ws_sb_put(&sb, "\r\n");
    if (sb.overflow || ws_queue_raw(ws, response, (uint32_t)sb.len) != ASX_OK) {
        return ws_fail(ws, ASX_WS_CLOSE_INTERNAL_ERROR);
    }
    ws->state = ASX_WS_STATE_OPEN;
    return ASX_OK;
}

static asx_status ws_client_handshake(asx_ws_engine *ws, uint32_t head_len) {
    ws_http_head head;
    ws_span sl;
    ws_span value;
    char expected[ASX_WS_ACCEPT_LEN + 1];

    if (!ws_http_head_parse((const char *)ws->hs_buf, head_len, &head)) {
        return ws_fail(ws, ASX_WS_CLOSE_PROTOCOL_ERROR);
    }
    /* Status line: HTTP/1.1 SP 101 [SP reason] */
    sl = head.start_line;
    if (sl.n < 12u || memcmp(sl.p, "HTTP/1.1 101", 12u) != 0 || (sl.n > 12u && sl.p[12] != ' ')) {
        return ws_fail(ws, ASX_WS_CLOSE_PROTOCOL_ERROR);
    }
    if (!ws_http_field_has_token(&head, "Upgrade", "websocket") ||
        !ws_http_field_has_token(&head, "Connection", "upgrade")) {
        return ws_fail(ws, ASX_WS_CLOSE_PROTOCOL_ERROR);
    }
    if (asx_ws_compute_accept(ws->key, strlen(ws->key), expected) != ASX_OK ||
        !ws_http_field_single(&head, "Sec-WebSocket-Accept", &value) ||
        !ws_span_eq(value, expected, 0)) {
        return ws_fail(ws, ASX_WS_CLOSE_PROTOCOL_ERROR);
    }
    /* We never offer extensions, so the server must not select any. */
    if (ws_http_field_count(&head, "Sec-WebSocket-Extensions") != 0u) {
        return ws_fail(ws, ASX_WS_CLOSE_PROTOCOL_ERROR);
    }
    switch (ws_http_field_count(&head, "Sec-WebSocket-Protocol")) {
    case 0u: break;
    case 1u:
        (void)ws_http_field_single(&head, "Sec-WebSocket-Protocol", &value);
        if (value.n == 0u || value.n >= sizeof(ws->protocol) ||
            memchr(value.p, ',', value.n) != NULL ||
            !ws_list_contains(ws_cstr_span(ws->protocols), value, 0)) {
            return ws_fail(ws, ASX_WS_CLOSE_PROTOCOL_ERROR);
        }
        memcpy(ws->protocol, value.p, value.n);
        ws->protocol[value.n] = '\0';
        break;
    default: return ws_fail(ws, ASX_WS_CLOSE_PROTOCOL_ERROR);
    }
    ws->state = ASX_WS_STATE_OPEN;
    return ASX_OK;
}

/* Accumulate handshake bytes. On completion, *used counts only the bytes
 * that belonged to the HTTP head; anything after it is frame data. */
static asx_status ws_feed_handshake(asx_ws_engine *ws, const uint8_t *data, uint32_t len,
                                    uint32_t *used) {
    uint32_t room = ASX_WS_HANDSHAKE_MAX - ws->hs_len;
    uint32_t take = ws_min_u32(room, len);
    uint32_t end = 0u;
    uint32_t before = ws->hs_len;
    asx_status st;

    *used = 0u;
    if (take > 0u) memcpy(ws->hs_buf + ws->hs_len, data, take);
    ws->hs_len += take;

    if (!ws_find_header_end(ws->hs_buf, ws->hs_len, &end)) {
        *used = take;
        if (ws->hs_len >= ASX_WS_HANDSHAKE_MAX) {
            return (ws->role == ASX_WS_ROLE_SERVER) ? ws_server_reject(ws, 0)
                                                    : ws_fail(ws, ASX_WS_CLOSE_PROTOCOL_ERROR);
        }
        return ASX_OK;
    }
    *used = end - before;
    st = (ws->role == ASX_WS_ROLE_SERVER) ? ws_server_handshake(ws, end)
                                          : ws_client_handshake(ws, end);
    asx_crypto_secure_zero(ws->hs_buf, sizeof(ws->hs_buf));
    ws->hs_len = 0u;
    return st;
}

/* ------------------------------------------------------------------ */
/* Engine: frame receive path                                          */
/* ------------------------------------------------------------------ */

static asx_status ws_on_header(asx_ws_engine *ws, const asx_ws_frame_header *h) {
    uint16_t code = asx_ws_header_check(h, ws->role);

    if (code != 0u) return ws_fail(ws, code);
    if (!asx_ws_opcode_is_control(h->opcode)) {
        if (h->opcode == ASX_WS_OPCODE_CONTINUATION) {
            if (!ws->msg_in_progress) return ws_fail(ws, ASX_WS_CLOSE_PROTOCOL_ERROR);
        } else {
            if (ws->msg_in_progress) return ws_fail(ws, ASX_WS_CLOSE_PROTOCOL_ERROR);
            ws->msg_in_progress = 1u;
            ws->msg_opcode = h->opcode;
            ws->msg_len = 0u;
            asx_ws_utf8_init(&ws->rx_utf8);
        }
        if (h->payload_len > (uint64_t)(ws->max_message - ws->msg_len)) {
            return ws_fail(ws, ASX_WS_CLOSE_MESSAGE_TOO_BIG);
        }
    }
    ws->cur = *h;
    ws->payload_pos = 0u;
    ws->hdr_len = 0u;
    ws->rx_in_payload = 1u;
    return ASX_OK;
}

static asx_status ws_on_payload(asx_ws_engine *ws, const uint8_t *chunk, uint32_t n) {
    uint8_t *dst;

    if (asx_ws_opcode_is_control(ws->cur.opcode)) {
        dst = ws->ctrl_buf + (uint32_t)ws->payload_pos;
    } else {
        dst = ws->msg_buf + ws->msg_len;
    }
    memcpy(dst, chunk, n);
    if (ws->cur.masked) asx_ws_mask(dst, n, ws->cur.mask_key, ws->payload_pos);
    ws->payload_pos += n;
    if (!asx_ws_opcode_is_control(ws->cur.opcode)) {
        ws->msg_len += n;
        if (ws->msg_opcode == ASX_WS_OPCODE_TEXT && !asx_ws_utf8_feed(&ws->rx_utf8, dst, n)) {
            return ws_fail(ws, ASX_WS_CLOSE_INVALID_PAYLOAD);
        }
    }
    return ASX_OK;
}

static asx_status ws_on_close_frame(asx_ws_engine *ws) {
    uint32_t n = (uint32_t)ws->cur.payload_len;
    uint16_t code = ASX_WS_CLOSE_NO_STATUS;

    if (n == 1u) return ws_fail(ws, ASX_WS_CLOSE_PROTOCOL_ERROR);
    if (n >= 2u) {
        code = asx_load_be_u16(ws->ctrl_buf);
        if (!asx_ws_close_code_receivable(code)) return ws_fail(ws, ASX_WS_CLOSE_PROTOCOL_ERROR);
        if (!asx_ws_utf8_valid(ws->ctrl_buf + 2, n - 2u)) {
            return ws_fail(ws, ASX_WS_CLOSE_INVALID_PAYLOAD);
        }
        memcpy(ws->close_reason, ws->ctrl_buf + 2, n - 2u);
        ws->close_reason[n - 2u] = '\0';
    }
    ws->close_code = code;
    ws->close_received = 1u;
    if (!ws->close_sent) {
        /* Echo the status code (RFC 6455 §5.5.1); space was reserved. */
        uint8_t payload[2];
        uint32_t plen = 0u;
        if (n >= 2u) {
            asx_store_be_u16(payload, code);
            plen = 2u;
        }
        if (ws_queue_frame(ws, ASX_WS_OPCODE_CLOSE, 1, payload, plen) == ASX_OK) {
            ws->close_sent = 1u;
        }
    }
    ws->state = ASX_WS_STATE_CLOSED;
    return ASX_OK;
}

static asx_status ws_on_frame_end(asx_ws_engine *ws) {
    ws->rx_in_payload = 0u;
    switch (ws->cur.opcode) {
    case ASX_WS_OPCODE_PING:
        ws->pings_received++;
        if (!ws->close_sent) {
            asx_status st = ws_queue_frame(ws, ASX_WS_OPCODE_PONG, 1, ws->ctrl_buf,
                                           (uint32_t)ws->cur.payload_len);
            if (st != ASX_OK) return ws_fail(ws, ASX_WS_CLOSE_INTERNAL_ERROR);
        }
        return ASX_OK;
    case ASX_WS_OPCODE_PONG: ws->pongs_received++; return ASX_OK;
    case ASX_WS_OPCODE_CLOSE: return ws_on_close_frame(ws);
    default: break;
    }
    if (ws->cur.fin) {
        if (ws->msg_opcode == ASX_WS_OPCODE_TEXT && !asx_ws_utf8_complete(&ws->rx_utf8)) {
            return ws_fail(ws, ASX_WS_CLOSE_INVALID_PAYLOAD);
        }
        ws->msg_in_progress = 0u;
        ws->msg_ready = 1u;
    }
    return ASX_OK;
}

static asx_status ws_feed_frames(asx_ws_engine *ws, const uint8_t *p, uint32_t len,
                                 uint32_t *consumed) {
    uint32_t pos = 0u;
    asx_status st = ASX_OK;

    while (pos < len && st == ASX_OK) {
        if (ws->close_received || ws->failed) {
            pos = len; /* discard after the close handshake / failure */
            break;
        }
        if (!ws->rx_in_payload) {
            asx_ws_frame_header h;
            asx_status hst;

            if (ws->hdr_len == 0u) {
                /* Frame boundary backpressure: hand over the finished
                 * message first, and keep room for any automatic reply. */
                if (ws->msg_ready) break;
                if (ws_tx_free(ws) < ASX_WS_MAX_FRAME_HEADER + ASX_WS_CONTROL_MAX) break;
            }
            ws->hdr_buf[ws->hdr_len++] = p[pos++];
            hst = asx_ws_header_decode(ws->hdr_buf, ws->hdr_len, &h);
            if (hst == ASX_E_PENDING) continue;
            if (hst != ASX_OK) {
                st = ws_fail(ws, ASX_WS_CLOSE_PROTOCOL_ERROR);
                break;
            }
            st = ws_on_header(ws, &h);
            if (st == ASX_OK && ws->cur.payload_len == 0u) st = ws_on_frame_end(ws);
        } else {
            uint64_t remaining = ws->cur.payload_len - ws->payload_pos;
            uint32_t take = len - pos;
            if ((uint64_t)take > remaining) take = (uint32_t)remaining;
            st = ws_on_payload(ws, p + pos, take);
            pos += take;
            if (st == ASX_OK && ws->payload_pos == ws->cur.payload_len) st = ws_on_frame_end(ws);
        }
    }
    if (ws->failed) pos = len;
    *consumed = pos;
    return st;
}

asx_status asx_ws_engine_feed(asx_ws_engine *ws, const uint8_t *data, uint32_t len,
                              uint32_t *consumed) {
    uint32_t used = 0u;
    uint32_t n = 0u;
    asx_status st;

    if (consumed != NULL) *consumed = 0u;
    if (ws == NULL || consumed == NULL || (data == NULL && len > 0u)) {
        return ASX_E_INVALID_ARGUMENT;
    }
    if (ws->failed) {
        *consumed = len;
        return ASX_E_DISCONNECTED;
    }
    if (ws->state == ASX_WS_STATE_CONNECTING) {
        st = ws_feed_handshake(ws, data, len, &used);
        if (st != ASX_OK) {
            *consumed = len;
            return st;
        }
        if (ws->state == ASX_WS_STATE_CONNECTING) {
            *consumed = used;
            return ASX_OK;
        }
    }
    st = ws_feed_frames(ws, data + used, len - used, &n);
    *consumed = used + n;
    return st;
}

/* ------------------------------------------------------------------ */
/* Engine: application API                                             */
/* ------------------------------------------------------------------ */

asx_status asx_ws_engine_recv(asx_ws_engine *ws, asx_ws_opcode *opcode, uint8_t *buf, uint32_t cap,
                              uint32_t *len) {
    if (ws == NULL || opcode == NULL || len == NULL || (buf == NULL && cap > 0u)) {
        return ASX_E_INVALID_ARGUMENT;
    }
    if (!ws->msg_ready) return ASX_E_PENDING;
    *len = ws->msg_len;
    if (ws->msg_len > cap) return ASX_E_BUFFER_TOO_SMALL;
    if (ws->msg_len > 0u) memcpy(buf, ws->msg_buf, ws->msg_len);
    *opcode = (asx_ws_opcode)ws->msg_opcode;
    ws->msg_ready = 0u;
    ws->msg_len = 0u;
    return ASX_OK;
}

asx_status asx_ws_engine_send_frame(asx_ws_engine *ws, asx_ws_opcode opcode, int fin,
                                    const uint8_t *data, uint32_t len) {
    uint32_t op = (uint32_t)opcode;
    asx_ws_utf8 v;
    asx_status st;
    int is_text;

    if (ws == NULL || (data == NULL && len > 0u)) return ASX_E_INVALID_ARGUMENT;
    if (ws->state != ASX_WS_STATE_OPEN) return ASX_E_INVALID_STATE;

    if (op == ASX_WS_OPCODE_PING || op == ASX_WS_OPCODE_PONG) {
        if (!fin || len > ASX_WS_CONTROL_MAX) return ASX_E_INVALID_ARGUMENT;
        return ws_queue_frame(ws, op, 1, data, len);
    }
    if (op == ASX_WS_OPCODE_TEXT || op == ASX_WS_OPCODE_BINARY) {
        if (ws->tx_fragmenting) return ASX_E_INVALID_ARGUMENT;
        asx_ws_utf8_init(&v);
        is_text = (op == ASX_WS_OPCODE_TEXT);
    } else if (op == ASX_WS_OPCODE_CONTINUATION) {
        if (!ws->tx_fragmenting) return ASX_E_INVALID_ARGUMENT;
        v = ws->tx_utf8;
        is_text = (ws->tx_frag_opcode == ASX_WS_OPCODE_TEXT);
    } else {
        return ASX_E_INVALID_ARGUMENT; /* CLOSE goes through asx_ws_engine_close() */
    }
    if (is_text) {
        if (!asx_ws_utf8_feed(&v, data, len)) return ASX_E_INVALID_ARGUMENT;
        if (fin && !asx_ws_utf8_complete(&v)) return ASX_E_INVALID_ARGUMENT;
    }

    st = ws_queue_frame(ws, op, fin, data, len);
    if (st != ASX_OK) return st;

    /* commit fragmentation state only after the frame is queued */
    if (op != ASX_WS_OPCODE_CONTINUATION) ws->tx_frag_opcode = (uint8_t)op;
    ws->tx_fragmenting = (uint8_t)(fin ? 0u : 1u);
    ws->tx_utf8 = v;
    return ASX_OK;
}

asx_status asx_ws_engine_send(asx_ws_engine *ws, asx_ws_opcode opcode, const uint8_t *data,
                              uint32_t len) {
    if (opcode == ASX_WS_OPCODE_CONTINUATION) return ASX_E_INVALID_ARGUMENT;
    return asx_ws_engine_send_frame(ws, opcode, 1, data, len);
}

asx_status asx_ws_engine_close(asx_ws_engine *ws, uint16_t code, const char *reason) {
    uint8_t payload[2u + ASX_WS_CLOSE_REASON_MAX];
    size_t rlen;
    uint32_t plen = 0u;
    asx_status st;

    if (ws == NULL) return ASX_E_INVALID_ARGUMENT;
    if (ws->state == ASX_WS_STATE_CONNECTING || ws->close_sent || ws->failed) {
        return ASX_E_INVALID_STATE;
    }
    rlen = (reason != NULL) ? strlen(reason) : 0u;
    if (code == 0u) {
        if (rlen > 0u) return ASX_E_INVALID_ARGUMENT;
    } else {
        if (!asx_ws_close_code_sendable(code)) return ASX_E_INVALID_ARGUMENT;
        if (rlen > ASX_WS_CLOSE_REASON_MAX) return ASX_E_INVALID_ARGUMENT;
        if (!asx_ws_utf8_valid((const uint8_t *)reason, rlen)) return ASX_E_INVALID_ARGUMENT;
        asx_store_be_u16(payload, code);
        if (rlen > 0u) memcpy(payload + 2, reason, rlen);
        plen = 2u + (uint32_t)rlen;
    }
    st = ws_queue_frame(ws, ASX_WS_OPCODE_CLOSE, 1, payload, plen);
    if (st != ASX_OK) return st;
    ws->close_sent = 1u;
    ws->state = ws->close_received ? ASX_WS_STATE_CLOSED : ASX_WS_STATE_CLOSING;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* TCP adapter                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_ws_engine engine;
    asx_buf_mut rx; /* bytes read from TCP not yet consumed by the engine */
    asx_tcp_stream tcp;
    uint32_t generation;
    int alive;
} ws_conn_slot;

static ws_conn_slot g_ws_conns[ASX_MAX_WS_CONNECTIONS];

static uint32_t ws_next_gen(uint32_t g) {
    g++;
    return g == 0u ? 1u : g;
}

static ws_conn_slot *ws_lookup(asx_ws_conn h) {
    ws_conn_slot *s;

    if (h.slot >= ASX_MAX_WS_CONNECTIONS) return NULL;
    s = &g_ws_conns[h.slot];
    if (!s->alive || s->generation != h.generation) return NULL;
    return s;
}

/* Write pending engine output to the stream. Stops (ASX_OK) when the stream
 * cannot take more right now. */
static asx_status ws_flush(ws_conn_slot *s, int *progress) {
    for (;;) {
        const uint8_t *p = NULL;
        uint32_t n = asx_ws_engine_output(&s->engine, &p);
        uint32_t chunk;

        if (n == 0u) return ASX_OK;
        chunk = ws_min_u32(n, ASX_BUF_CAPACITY);
        for (;;) {
            asx_buf b = asx_buf_from(p, chunk);
            uint32_t written = 0u;
            asx_status st = asx_tcp_stream_poll_write(s->tcp, &b, &written);
            if (st == ASX_OK) {
                if (written == 0u) return ASX_OK;
                st = asx_ws_engine_consume_output(&s->engine, written);
                if (st != ASX_OK) return st;
                *progress = 1;
                break;
            }
            if (st == ASX_E_WOULD_BLOCK && chunk > 1u) {
                chunk /= 2u; /* peer buffer nearly full: try a smaller write */
                continue;
            }
            if (st == ASX_E_WOULD_BLOCK || st == ASX_E_PENDING) return ASX_OK;
            return st;
        }
    }
}

static asx_status ws_pump(ws_conn_slot *s) {
    uint32_t rounds;
    int progress = 0;
    asx_status st;

    if (!asx_tcp_stream_is_alive(s->tcp)) {
        /* The stream was closed underneath us: abnormal closure (1006)
         * unless the close handshake already completed. */
        asx_ws_engine_transport_closed(&s->engine);
        return ASX_E_DISCONNECTED;
    }
    for (rounds = 0u; rounds < 64u; rounds++) {
        progress = 0;
        st = ws_flush(s, &progress);
        if (st != ASX_OK) return st;

        if (asx_buf_mut_remaining(&s->rx) > 0u) {
            asx_buf readable = asx_buf_mut_readable(&s->rx);
            uint32_t used = 0u;
            asx_status fst = asx_ws_engine_feed(&s->engine, readable.ptr, readable.len, &used);
            if (used > 0u) {
                st = asx_buf_mut_advance(&s->rx, used);
                if (st != ASX_OK) return st;
                progress = 1;
            }
            if (asx_buf_mut_remaining(&s->rx) == 0u) asx_buf_mut_clear(&s->rx);
            if (fst != ASX_OK) {
                (void)ws_flush(s, &progress);
                return fst;
            }
        }

        if (asx_buf_mut_remaining(&s->rx) == 0u) {
            uint32_t got = 0u;
            asx_buf_mut_clear(&s->rx);
            st = asx_tcp_stream_poll_read(s->tcp, &s->rx, &got);
            if (st == ASX_OK && got > 0u) {
                progress = 1;
            } else if (st != ASX_OK && st != ASX_E_PENDING) {
                return st;
            }
        }
        if (!progress) break;
    }
    st = ws_flush(s, &progress);
    if (st != ASX_OK) return st;
    return s->engine.failed ? ASX_E_DISCONNECTED : ASX_OK;
}

static asx_status ws_open(asx_ws_conn *out, asx_tcp_stream tcp, const asx_ws_config *cfg,
                          asx_ws_role role) {
    asx_ws_config local;
    uint32_t idx;
    uint32_t gen;
    ws_conn_slot *s;
    asx_status st;
    int progress = 0;

    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!asx_tcp_stream_is_alive(tcp)) return ASX_E_INVALID_ARGUMENT;
    if (cfg != NULL) {
        local = *cfg;
    } else {
        asx_ws_config_init(&local, role);
    }
    local.role = role;

    for (idx = 0u; idx < ASX_MAX_WS_CONNECTIONS; idx++) {
        if (!g_ws_conns[idx].alive) break;
    }
    if (idx >= ASX_MAX_WS_CONNECTIONS) return ASX_E_RESOURCE_EXHAUSTED;

    s = &g_ws_conns[idx];
    gen = s->generation;
    memset(s, 0, sizeof(*s));
    s->generation = gen;
    st = asx_ws_engine_init(&s->engine, &local);
    if (st != ASX_OK) {
        memset(&s->engine, 0, sizeof(s->engine));
        return st;
    }
    asx_buf_mut_init(&s->rx);
    s->tcp = tcp;
    s->generation = ws_next_gen(gen);
    s->alive = 1;

    out->slot = idx;
    out->generation = s->generation;
    return ws_flush(s, &progress);
}

asx_status asx_ws_connect(asx_ws_conn *out, asx_tcp_stream tcp, const asx_ws_config *cfg) {
    if (cfg == NULL) return ASX_E_INVALID_ARGUMENT; /* clients need at least a Host */
    return ws_open(out, tcp, cfg, ASX_WS_ROLE_CLIENT);
}

asx_status asx_ws_accept(asx_ws_conn *out, asx_tcp_stream tcp, const asx_ws_config *cfg) {
    return ws_open(out, tcp, cfg, ASX_WS_ROLE_SERVER);
}

asx_status asx_ws_poll(asx_ws_conn conn) {
    ws_conn_slot *s = ws_lookup(conn);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    return ws_pump(s);
}

asx_status asx_ws_send_frame(asx_ws_conn conn, asx_ws_opcode opcode, int fin, const void *data,
                             uint32_t len) {
    ws_conn_slot *s = ws_lookup(conn);
    asx_status st;
    int progress = 0;

    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    st = asx_ws_engine_send_frame(&s->engine, opcode, fin, (const uint8_t *)data, len);
    if (st == ASX_E_WOULD_BLOCK) {
        st = ws_flush(s, &progress);
        if (st != ASX_OK) return st;
        st = asx_ws_engine_send_frame(&s->engine, opcode, fin, (const uint8_t *)data, len);
    }
    if (st != ASX_OK) return st;
    return ws_flush(s, &progress);
}

asx_status asx_ws_send(asx_ws_conn conn, asx_ws_opcode opcode, const void *data, uint32_t len) {
    if (opcode == ASX_WS_OPCODE_CONTINUATION) return ASX_E_INVALID_ARGUMENT;
    return asx_ws_send_frame(conn, opcode, 1, data, len);
}

asx_status asx_ws_poll_recv(asx_ws_conn conn, asx_ws_opcode *opcode, void *buf, uint32_t cap,
                            uint32_t *len) {
    ws_conn_slot *s = ws_lookup(conn);
    asx_status pst;
    asx_status rst;

    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    pst = ws_pump(s);
    rst = asx_ws_engine_recv(&s->engine, opcode, (uint8_t *)buf, cap, len);
    if (rst == ASX_OK) {
        (void)ws_pump(s); /* keep draining buffered input behind the message */
        return ASX_OK;
    }
    if (rst == ASX_E_PENDING && pst != ASX_OK) return pst;
    return rst;
}

asx_status asx_ws_close(asx_ws_conn conn, uint16_t code, const char *reason) {
    ws_conn_slot *s = ws_lookup(conn);
    asx_status st;
    int progress = 0;

    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    st = asx_ws_engine_close(&s->engine, code, reason);
    if (st == ASX_E_WOULD_BLOCK) {
        st = ws_flush(s, &progress);
        if (st != ASX_OK) return st;
        st = asx_ws_engine_close(&s->engine, code, reason);
    }
    if (st != ASX_OK) return st;
    return ws_flush(s, &progress);
}

asx_ws_state asx_ws_conn_state(asx_ws_conn conn) {
    ws_conn_slot *s = ws_lookup(conn);
    return s == NULL ? ASX_WS_STATE_CLOSED : s->engine.state;
}

asx_ws_role asx_ws_conn_role(asx_ws_conn conn) {
    ws_conn_slot *s = ws_lookup(conn);
    return s == NULL ? ASX_WS_ROLE_CLIENT : s->engine.role;
}

const asx_ws_engine *asx_ws_conn_engine(asx_ws_conn conn) {
    ws_conn_slot *s = ws_lookup(conn);
    return s == NULL ? NULL : &s->engine;
}

asx_status asx_ws_conn_release(asx_ws_conn conn) {
    ws_conn_slot *s = ws_lookup(conn);
    uint32_t gen;

    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    gen = s->generation;
    memset(s, 0, sizeof(*s));
    s->generation = gen; /* keep generation so stale handles stay stale */
    return ASX_OK;
}

int asx_ws_conn_is_alive(asx_ws_conn conn) { return ws_lookup(conn) != NULL; }

void asx_ws_reset(void) { memset(g_ws_conns, 0, sizeof(g_ws_conns)); }
