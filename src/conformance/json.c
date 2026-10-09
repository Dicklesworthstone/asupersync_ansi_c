/*
 * conformance/json.c — bounded JSON DOM and RFC 8785 canonical writer
 * (bead W1.5, bd-9kll.2.5). See json.h for the contract.
 *
 * SPDX-License-Identifier: MIT
 */

#include "json.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Pools                                                               */
/* ------------------------------------------------------------------ */

void asx_json_doc_init(asx_json_doc *doc) {
    doc->node_count = 0;
    doc->text_used = 0;
    doc->error_offset = 0;
    doc->error = NULL;
}

static uint32_t node_alloc(asx_json_doc *doc, asx_json_type type) {
    asx_json_node *n;
    uint32_t idx;
    if (doc->node_count >= ASX_JSON_MAX_NODES) {
        doc->error = "node pool exhausted";
        return ASX_JSON_NONE;
    }
    idx = doc->node_count++;
    n = &doc->nodes[idx];
    n->type = type;
    n->key = ASX_JSON_NONE;
    n->key_len = 0;
    n->str = ASX_JSON_NONE;
    n->str_len = 0;
    n->magnitude = 0;
    n->negative = 0;
    n->first = ASX_JSON_NONE;
    n->last = ASX_JSON_NONE;
    n->next = ASX_JSON_NONE;
    n->count = 0;
    return idx;
}

/* Copy `len` bytes plus a NUL into the text pool; returns the offset or
 * ASX_JSON_NONE when the pool is full. */
static uint32_t text_store(asx_json_doc *doc, const char *bytes, size_t len) {
    uint32_t off;
    if (len >= (size_t)ASX_JSON_MAX_TEXT || (size_t)doc->text_used + len + 1u > ASX_JSON_MAX_TEXT) {
        doc->error = "text pool exhausted";
        return ASX_JSON_NONE;
    }
    off = doc->text_used;
    if (len > 0u) memcpy(&doc->text[off], bytes, len);
    doc->text[off + (uint32_t)len] = '\0';
    doc->text_used += (uint32_t)len + 1u;
    return off;
}

int asx_json_doc_ok(const asx_json_doc *doc) { return doc->error == NULL; }

/* ------------------------------------------------------------------ */
/* Parser                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_json_doc *doc;
    const char *src;
    size_t len;
    size_t pos;
    uint32_t depth;
} json_parser;

static asx_status parse_fail(json_parser *p, const char *why) {
    p->doc->error = why;
    p->doc->error_offset = p->pos;
    return ASX_E_INVALID_ARGUMENT;
}

static asx_status parse_exhausted(json_parser *p) {
    p->doc->error_offset = p->pos;
    return ASX_E_RESOURCE_EXHAUSTED;
}

static void skip_ws(json_parser *p) {
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
        p->pos++;
    }
}

static int at_literal(const json_parser *p, const char *lit) {
    size_t n = strlen(lit);
    return p->len - p->pos >= n && memcmp(p->src + p->pos, lit, n) == 0;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int read_hex4(json_parser *p, uint32_t *out) {
    uint32_t v = 0;
    size_t i;
    if (p->len - p->pos < 4u) return 0;
    for (i = 0; i < 4u; i++) {
        int h = hex_value(p->src[p->pos + i]);
        if (h < 0) return 0;
        v = (v << 4) | (uint32_t)h;
    }
    p->pos += 4u;
    *out = v;
    return 1;
}

/* Append one byte to the string being decoded (text pool tail). */
static int text_push(asx_json_doc *doc, uint32_t start, char c) {
    if ((size_t)doc->text_used + 1u >= ASX_JSON_MAX_TEXT) return 0;
    (void)start;
    doc->text[doc->text_used++] = c;
    return 1;
}

static int text_push_utf8(asx_json_doc *doc, uint32_t start, uint32_t cp) {
    if (cp < 0x80u) return text_push(doc, start, (char)cp);
    if (cp < 0x800u) {
        return text_push(doc, start, (char)(0xC0u | (cp >> 6))) &&
               text_push(doc, start, (char)(0x80u | (cp & 0x3Fu)));
    }
    if (cp < 0x10000u) {
        return text_push(doc, start, (char)(0xE0u | (cp >> 12))) &&
               text_push(doc, start, (char)(0x80u | ((cp >> 6) & 0x3Fu))) &&
               text_push(doc, start, (char)(0x80u | (cp & 0x3Fu)));
    }
    return text_push(doc, start, (char)(0xF0u | (cp >> 18))) &&
           text_push(doc, start, (char)(0x80u | ((cp >> 12) & 0x3Fu))) &&
           text_push(doc, start, (char)(0x80u | ((cp >> 6) & 0x3Fu))) &&
           text_push(doc, start, (char)(0x80u | (cp & 0x3Fu)));
}

/* Length of the valid UTF-8 sequence at s[0..avail), or 0 if invalid
 * (overlong forms, surrogates and code points above U+10FFFF are invalid). */
static size_t utf8_seq_len(const unsigned char *s, size_t avail) {
    unsigned char c = s[0];
    if (c < 0x80u) return 1;
    if (c >= 0xC2u && c <= 0xDFu) { return (avail >= 2u && (s[1] & 0xC0u) == 0x80u) ? 2u : 0u; }
    if (c >= 0xE0u && c <= 0xEFu) {
        if (avail < 3u || (s[1] & 0xC0u) != 0x80u || (s[2] & 0xC0u) != 0x80u) return 0;
        if (c == 0xE0u && s[1] < 0xA0u) return 0;  /* overlong */
        if (c == 0xEDu && s[1] >= 0xA0u) return 0; /* surrogate */
        return 3;
    }
    if (c >= 0xF0u && c <= 0xF4u) {
        if (avail < 4u || (s[1] & 0xC0u) != 0x80u || (s[2] & 0xC0u) != 0x80u ||
            (s[3] & 0xC0u) != 0x80u) {
            return 0;
        }
        if (c == 0xF0u && s[1] < 0x90u) return 0;  /* overlong */
        if (c == 0xF4u && s[1] >= 0x90u) return 0; /* above U+10FFFF */
        return 4;
    }
    return 0;
}

/* Decode the string starting at the opening quote into the text pool. */
static asx_status parse_string(json_parser *p, uint32_t *out_off, uint32_t *out_len) {
    asx_json_doc *doc = p->doc;
    uint32_t start = doc->text_used;
    p->pos++; /* opening quote */
    for (;;) {
        unsigned char c;
        if (p->pos >= p->len) return parse_fail(p, "unterminated string");
        c = (unsigned char)p->src[p->pos];
        if (c == '"') {
            p->pos++;
            break;
        }
        if (c < 0x20u) return parse_fail(p, "unescaped control character in string");
        if (c == '\\') {
            char e;
            p->pos++;
            if (p->pos >= p->len) return parse_fail(p, "unterminated escape");
            e = p->src[p->pos++];
            switch (e) {
            case '"': c = '"'; break;
            case '\\': c = '\\'; break;
            case '/': c = '/'; break;
            case 'b': c = 0x08u; break;
            case 'f': c = 0x0Cu; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'u': {
                uint32_t cp;
                if (!read_hex4(p, &cp)) return parse_fail(p, "bad \\u escape");
                if (cp >= 0xD800u && cp <= 0xDBFFu) {
                    uint32_t lo;
                    if (!at_literal(p, "\\u")) return parse_fail(p, "lone high surrogate");
                    p->pos += 2u;
                    if (!read_hex4(p, &lo) || lo < 0xDC00u || lo > 0xDFFFu) {
                        return parse_fail(p, "bad low surrogate");
                    }
                    cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
                    return parse_fail(p, "lone low surrogate");
                }
                if (!text_push_utf8(doc, start, cp)) return parse_exhausted(p);
                continue;
            }
            default: return parse_fail(p, "unknown escape");
            }
            if (!text_push(doc, start, (char)c)) return parse_exhausted(p);
            continue;
        }
        {
            size_t n = utf8_seq_len((const unsigned char *)p->src + p->pos, p->len - p->pos);
            size_t i;
            if (n == 0u) return parse_fail(p, "invalid UTF-8");
            for (i = 0; i < n; i++) {
                if (!text_push(doc, start, p->src[p->pos + i])) return parse_exhausted(p);
            }
            p->pos += n;
        }
    }
    if ((size_t)doc->text_used + 1u > ASX_JSON_MAX_TEXT) return parse_exhausted(p);
    doc->text[doc->text_used++] = '\0';
    *out_off = start;
    *out_len = doc->text_used - start - 1u;
    return ASX_OK;
}

static asx_status parse_number(json_parser *p, uint32_t node) {
    asx_json_node *n = &p->doc->nodes[node];
    uint64_t v = 0;
    int neg = 0;
    if (p->src[p->pos] == '-') {
        neg = 1;
        p->pos++;
    }
    if (p->pos >= p->len || p->src[p->pos] < '0' || p->src[p->pos] > '9') {
        return parse_fail(p, "bad number");
    }
    if (p->src[p->pos] == '0') {
        p->pos++;
    } else {
        while (p->pos < p->len && p->src[p->pos] >= '0' && p->src[p->pos] <= '9') {
            uint64_t d = (uint64_t)(p->src[p->pos] - '0');
            if (v > (UINT64_MAX - d) / 10u) return parse_fail(p, "integer out of range");
            v = v * 10u + d;
            p->pos++;
        }
    }
    if (p->pos < p->len &&
        (p->src[p->pos] == '.' || p->src[p->pos] == 'e' || p->src[p->pos] == 'E')) {
        return parse_fail(p, "fractions and exponents are not supported");
    }
    if (neg && v > (uint64_t)INT64_MAX + 1u) return parse_fail(p, "integer out of range");
    n->magnitude = v;
    n->negative = (neg && v != 0u) ? 1 : 0;
    return ASX_OK;
}

static asx_status parse_value(json_parser *p, uint32_t *out);

static int member_key_eq(const asx_json_doc *doc, uint32_t member, const char *key, size_t len) {
    const asx_json_node *m = &doc->nodes[member];
    return m->key_len == len && memcmp(&doc->text[m->key], key, len) == 0;
}

static void link_child(asx_json_doc *doc, uint32_t parent, uint32_t child) {
    asx_json_node *pn = &doc->nodes[parent];
    if (pn->last == ASX_JSON_NONE) {
        pn->first = child;
    } else {
        doc->nodes[pn->last].next = child;
    }
    pn->last = child;
    pn->count++;
}

static asx_status parse_container(json_parser *p, uint32_t node, int is_object) {
    char close = is_object ? '}' : ']';
    asx_status st;
    p->pos++; /* opening bracket */
    if (++p->depth > ASX_JSON_MAX_DEPTH) {
        p->doc->error = "nesting too deep";
        return parse_exhausted(p);
    }
    skip_ws(p);
    if (p->pos < p->len && p->src[p->pos] == close) {
        p->pos++;
        p->depth--;
        return ASX_OK;
    }
    for (;;) {
        uint32_t child;
        uint32_t key_off = ASX_JSON_NONE;
        uint32_t key_len = 0;
        skip_ws(p);
        if (is_object) {
            uint32_t m;
            if (p->pos >= p->len || p->src[p->pos] != '"') return parse_fail(p, "expected key");
            st = parse_string(p, &key_off, &key_len);
            if (st != ASX_OK) return st;
            for (m = p->doc->nodes[node].first; m != ASX_JSON_NONE; m = p->doc->nodes[m].next) {
                if (member_key_eq(p->doc, m, &p->doc->text[key_off], key_len)) {
                    return parse_fail(p, "duplicate object key");
                }
            }
            skip_ws(p);
            if (p->pos >= p->len || p->src[p->pos] != ':') return parse_fail(p, "expected ':'");
            p->pos++;
        }
        st = parse_value(p, &child);
        if (st != ASX_OK) return st;
        p->doc->nodes[child].key = key_off;
        p->doc->nodes[child].key_len = key_len;
        link_child(p->doc, node, child);
        skip_ws(p);
        if (p->pos >= p->len) return parse_fail(p, "unterminated container");
        if (p->src[p->pos] == ',') {
            p->pos++;
            continue;
        }
        if (p->src[p->pos] == close) {
            p->pos++;
            break;
        }
        return parse_fail(p, "expected ',' or closing bracket");
    }
    p->depth--;
    return ASX_OK;
}

static asx_status parse_value(json_parser *p, uint32_t *out) {
    uint32_t node;
    char c;
    skip_ws(p);
    if (p->pos >= p->len) return parse_fail(p, "unexpected end of input");
    c = p->src[p->pos];
    if (c == '{' || c == '[') {
        node = node_alloc(p->doc, c == '{' ? ASX_JSON_OBJECT : ASX_JSON_ARRAY);
        if (node == ASX_JSON_NONE) return parse_exhausted(p);
        *out = node;
        return parse_container(p, node, c == '{');
    }
    if (c == '"') {
        asx_status st;
        node = node_alloc(p->doc, ASX_JSON_STRING);
        if (node == ASX_JSON_NONE) return parse_exhausted(p);
        st = parse_string(p, &p->doc->nodes[node].str, &p->doc->nodes[node].str_len);
        *out = node;
        return st;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        node = node_alloc(p->doc, ASX_JSON_INT);
        if (node == ASX_JSON_NONE) return parse_exhausted(p);
        *out = node;
        return parse_number(p, node);
    }
    if (at_literal(p, "true") || at_literal(p, "false")) {
        int t = at_literal(p, "true");
        node = node_alloc(p->doc, ASX_JSON_BOOL);
        if (node == ASX_JSON_NONE) return parse_exhausted(p);
        p->doc->nodes[node].magnitude = t ? 1u : 0u;
        p->pos += t ? 4u : 5u;
        *out = node;
        return ASX_OK;
    }
    if (at_literal(p, "null")) {
        node = node_alloc(p->doc, ASX_JSON_NULL);
        if (node == ASX_JSON_NONE) return parse_exhausted(p);
        p->pos += 4u;
        *out = node;
        return ASX_OK;
    }
    return parse_fail(p, "unexpected character");
}

asx_status asx_json_parse(asx_json_doc *doc, const char *src, size_t len, uint32_t *out_root) {
    json_parser p;
    asx_status st;
    if (doc == NULL || src == NULL || out_root == NULL) return ASX_E_INVALID_ARGUMENT;
    p.doc = doc;
    p.src = src;
    p.len = len;
    p.pos = 0;
    p.depth = 0;
    doc->error = NULL;
    st = parse_value(&p, out_root);
    if (st != ASX_OK) return st;
    skip_ws(&p);
    if (p.pos != p.len) return parse_fail(&p, "trailing characters");
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Reading                                                             */
/* ------------------------------------------------------------------ */

const asx_json_node *asx_json_at(const asx_json_doc *doc, uint32_t node) {
    if (node == ASX_JSON_NONE || node >= doc->node_count) return NULL;
    return &doc->nodes[node];
}

asx_json_type asx_json_type_of(const asx_json_doc *doc, uint32_t node) {
    const asx_json_node *n = asx_json_at(doc, node);
    return n == NULL ? ASX_JSON_NULL : n->type;
}

uint32_t asx_json_get(const asx_json_doc *doc, uint32_t obj, const char *key) {
    const asx_json_node *n = asx_json_at(doc, obj);
    size_t len;
    uint32_t m;
    if (n == NULL || n->type != ASX_JSON_OBJECT || key == NULL) return ASX_JSON_NONE;
    len = strlen(key);
    for (m = n->first; m != ASX_JSON_NONE; m = doc->nodes[m].next) {
        if (member_key_eq(doc, m, key, len)) return m;
    }
    return ASX_JSON_NONE;
}

uint32_t asx_json_item(const asx_json_doc *doc, uint32_t container, uint32_t index) {
    const asx_json_node *n = asx_json_at(doc, container);
    uint32_t m;
    uint32_t i = 0;
    if (n == NULL || (n->type != ASX_JSON_ARRAY && n->type != ASX_JSON_OBJECT)) {
        return ASX_JSON_NONE;
    }
    for (m = n->first; m != ASX_JSON_NONE; m = doc->nodes[m].next) {
        if (i++ == index) return m;
    }
    return ASX_JSON_NONE;
}

uint32_t asx_json_count(const asx_json_doc *doc, uint32_t container) {
    const asx_json_node *n = asx_json_at(doc, container);
    if (n == NULL || (n->type != ASX_JSON_ARRAY && n->type != ASX_JSON_OBJECT)) return 0;
    return n->count;
}

const char *asx_json_string(const asx_json_doc *doc, uint32_t node) {
    const asx_json_node *n = asx_json_at(doc, node);
    if (n == NULL || n->type != ASX_JSON_STRING) return NULL;
    return &doc->text[n->str];
}

const char *asx_json_key(const asx_json_doc *doc, uint32_t member) {
    const asx_json_node *n = asx_json_at(doc, member);
    if (n == NULL || n->key == ASX_JSON_NONE) return NULL;
    return &doc->text[n->key];
}

int asx_json_is_null(const asx_json_doc *doc, uint32_t node) {
    const asx_json_node *n = asx_json_at(doc, node);
    return n != NULL && n->type == ASX_JSON_NULL;
}

int asx_json_u64(const asx_json_doc *doc, uint32_t node, uint64_t *out) {
    const asx_json_node *n = asx_json_at(doc, node);
    if (n == NULL || n->type != ASX_JSON_INT || n->negative) return 0;
    *out = n->magnitude;
    return 1;
}

int asx_json_bool(const asx_json_doc *doc, uint32_t node, int *out) {
    const asx_json_node *n = asx_json_at(doc, node);
    if (n == NULL || n->type != ASX_JSON_BOOL) return 0;
    *out = n->magnitude != 0u;
    return 1;
}

const char *asx_json_get_string(const asx_json_doc *doc, uint32_t obj, const char *key) {
    return asx_json_string(doc, asx_json_get(doc, obj, key));
}

/* ------------------------------------------------------------------ */
/* Building                                                            */
/* ------------------------------------------------------------------ */

uint32_t asx_json_new_null(asx_json_doc *doc) { return node_alloc(doc, ASX_JSON_NULL); }

uint32_t asx_json_new_bool(asx_json_doc *doc, int value) {
    uint32_t n = node_alloc(doc, ASX_JSON_BOOL);
    if (n != ASX_JSON_NONE) doc->nodes[n].magnitude = value ? 1u : 0u;
    return n;
}

uint32_t asx_json_new_u64(asx_json_doc *doc, uint64_t value) {
    uint32_t n = node_alloc(doc, ASX_JSON_INT);
    if (n != ASX_JSON_NONE) doc->nodes[n].magnitude = value;
    return n;
}

uint32_t asx_json_new_string_len(asx_json_doc *doc, const char *s, size_t len) {
    uint32_t off;
    uint32_t n;
    if (s == NULL) return asx_json_new_null(doc);
    off = text_store(doc, s, len);
    if (off == ASX_JSON_NONE) return ASX_JSON_NONE;
    n = node_alloc(doc, ASX_JSON_STRING);
    if (n == ASX_JSON_NONE) return ASX_JSON_NONE;
    doc->nodes[n].str = off;
    doc->nodes[n].str_len = (uint32_t)len;
    return n;
}

uint32_t asx_json_new_string(asx_json_doc *doc, const char *s) {
    return asx_json_new_string_len(doc, s, s == NULL ? 0u : strlen(s));
}

uint32_t asx_json_new_array(asx_json_doc *doc) { return node_alloc(doc, ASX_JSON_ARRAY); }

uint32_t asx_json_new_object(asx_json_doc *doc) { return node_alloc(doc, ASX_JSON_OBJECT); }

void asx_json_push(asx_json_doc *doc, uint32_t arr, uint32_t child) {
    if (arr == ASX_JSON_NONE || child == ASX_JSON_NONE) {
        if (doc->error == NULL) doc->error = "push of a missing node";
        return;
    }
    doc->nodes[child].next = ASX_JSON_NONE;
    link_child(doc, arr, child);
}

/* Add or replace member `key` (exactly `len` bytes, which may include NUL). */
static void set_member(asx_json_doc *doc, uint32_t obj, const char *key, size_t len,
                       uint32_t child) {
    uint32_t prev = ASX_JSON_NONE;
    uint32_t m;
    uint32_t off;
    if (obj == ASX_JSON_NONE || child == ASX_JSON_NONE || key == NULL) {
        if (doc->error == NULL) doc->error = "set of a missing node";
        return;
    }
    off = text_store(doc, key, len);
    if (off == ASX_JSON_NONE) return;
    doc->nodes[child].key = off;
    doc->nodes[child].key_len = (uint32_t)len;
    doc->nodes[child].next = ASX_JSON_NONE;
    for (m = doc->nodes[obj].first; m != ASX_JSON_NONE; prev = m, m = doc->nodes[m].next) {
        if (member_key_eq(doc, m, key, len)) {
            /* Replace the member in place. */
            doc->nodes[child].next = doc->nodes[m].next;
            if (prev == ASX_JSON_NONE) {
                doc->nodes[obj].first = child;
            } else {
                doc->nodes[prev].next = child;
            }
            if (doc->nodes[obj].last == m) doc->nodes[obj].last = child;
            return;
        }
    }
    link_child(doc, obj, child);
}

void asx_json_set(asx_json_doc *doc, uint32_t obj, const char *key, uint32_t child) {
    set_member(doc, obj, key, key == NULL ? 0u : strlen(key), child);
}

static uint32_t copy_node(asx_json_doc *dst, const asx_json_doc *src, uint32_t node,
                          uint32_t depth) {
    const asx_json_node *s = asx_json_at(src, node);
    uint32_t n;
    uint32_t c;
    if (s == NULL || depth > ASX_JSON_MAX_DEPTH) {
        if (dst->error == NULL) dst->error = "copy of a missing node";
        return ASX_JSON_NONE;
    }
    switch (s->type) {
    case ASX_JSON_STRING: return asx_json_new_string_len(dst, &src->text[s->str], s->str_len);
    case ASX_JSON_NULL:
    case ASX_JSON_BOOL:
    case ASX_JSON_INT:
        n = node_alloc(dst, s->type);
        if (n == ASX_JSON_NONE) return ASX_JSON_NONE;
        dst->nodes[n].magnitude = s->magnitude;
        dst->nodes[n].negative = s->negative;
        return n;
    case ASX_JSON_ARRAY:
    case ASX_JSON_OBJECT:
        n = node_alloc(dst, s->type);
        if (n == ASX_JSON_NONE) return ASX_JSON_NONE;
        for (c = s->first; c != ASX_JSON_NONE; c = src->nodes[c].next) {
            uint32_t cc = copy_node(dst, src, c, depth + 1u);
            if (cc == ASX_JSON_NONE) return ASX_JSON_NONE;
            if (s->type == ASX_JSON_OBJECT) {
                set_member(dst, n, &src->text[src->nodes[c].key], src->nodes[c].key_len, cc);
            } else {
                asx_json_push(dst, n, cc);
            }
        }
        return n;
    }
    return ASX_JSON_NONE;
}

uint32_t asx_json_copy(asx_json_doc *dst, const asx_json_doc *src, uint32_t node) {
    return copy_node(dst, src, node, 0);
}

/* ------------------------------------------------------------------ */
/* Canonical writer                                                    */
/* ------------------------------------------------------------------ */

void asx_json_out_init(asx_json_out *out, char *storage, size_t cap) {
    out->data = storage;
    out->cap = cap;
    out->len = 0;
    out->overflow = 0;
    out->flush = NULL;
    out->flush_ctx = NULL;
}

void asx_json_out_init_stream(asx_json_out *out, char *storage, size_t cap, asx_json_flush_fn flush,
                              void *ctx) {
    asx_json_out_init(out, storage, cap);
    out->flush = flush;
    out->flush_ctx = ctx;
}

void asx_json_out_append(asx_json_out *out, const char *bytes, size_t len) {
    if (out->overflow) return;
    if (out->flush != NULL) {
        if (out->cap == 0u || len > out->cap - out->len) {
            if (out->len > 0u) out->flush(out->flush_ctx, out->data, out->len);
            out->len = 0;
            if (out->cap == 0u || len > out->cap) {
                out->flush(out->flush_ctx, bytes, len);
                return;
            }
        }
        memcpy(out->data + out->len, bytes, len);
        out->len += len;
        return;
    }
    if (out->cap == 0u || len > out->cap - 1u - out->len) {
        out->overflow = 1;
        return;
    }
    memcpy(out->data + out->len, bytes, len);
    out->len += len;
}

void asx_json_out_cstr(asx_json_out *out, const char *s) { asx_json_out_append(out, s, strlen(s)); }

asx_status asx_json_out_finish(asx_json_out *out) {
    if (out->flush != NULL) {
        if (out->len > 0u) out->flush(out->flush_ctx, out->data, out->len);
        out->len = 0;
        return ASX_OK;
    }
    if (out->overflow || out->cap == 0u) return ASX_E_BUFFER_TOO_SMALL;
    out->data[out->len] = '\0';
    return ASX_OK;
}

static void write_u64(asx_json_out *out, uint64_t v) {
    char buf[24];
    size_t i = sizeof(buf);
    do {
        buf[--i] = (char)('0' + (int)(v % 10u));
        v /= 10u;
    } while (v != 0u);
    asx_json_out_append(out, buf + i, sizeof(buf) - i);
}

static void write_string(asx_json_out *out, const char *s, size_t len) {
    static const char hex[] = "0123456789abcdef";
    size_t i;
    size_t run = 0;
    asx_json_out_append(out, "\"", 1);
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        const char *esc = NULL;
        char ubuf[6];
        if (c == '"') {
            esc = "\\\"";
        } else if (c == '\\') {
            esc = "\\\\";
        } else if (c == 0x08u) {
            esc = "\\b";
        } else if (c == 0x0Cu) {
            esc = "\\f";
        } else if (c == '\n') {
            esc = "\\n";
        } else if (c == '\r') {
            esc = "\\r";
        } else if (c == '\t') {
            esc = "\\t";
        } else if (c < 0x20u) {
            ubuf[0] = '\\';
            ubuf[1] = 'u';
            ubuf[2] = '0';
            ubuf[3] = '0';
            ubuf[4] = hex[c >> 4];
            ubuf[5] = hex[c & 0x0Fu];
        } else {
            run++;
            continue;
        }
        asx_json_out_append(out, s + i - run, run);
        run = 0;
        if (esc != NULL) {
            asx_json_out_cstr(out, esc);
        } else {
            asx_json_out_append(out, ubuf, sizeof(ubuf));
        }
    }
    asx_json_out_append(out, s + len - run, run);
    asx_json_out_append(out, "\"", 1);
}

/* Next UTF-16 code unit of a UTF-8 string; *pending carries the low half
 * of a surrogate pair. Invalid bytes are returned as themselves. */
static uint32_t next_utf16_unit(const unsigned char *s, size_t len, size_t *pos,
                                uint32_t *pending) {
    uint32_t cp;
    size_t n;
    if (*pending != 0u) {
        cp = *pending;
        *pending = 0u;
        return cp;
    }
    n = utf8_seq_len(s + *pos, len - *pos);
    if (n <= 1u) {
        cp = s[*pos];
        *pos += 1u;
        return cp;
    }
    if (n == 2u) {
        cp = ((uint32_t)(s[*pos] & 0x1Fu) << 6) | (uint32_t)(s[*pos + 1u] & 0x3Fu);
    } else if (n == 3u) {
        cp = ((uint32_t)(s[*pos] & 0x0Fu) << 12) | ((uint32_t)(s[*pos + 1u] & 0x3Fu) << 6) |
             (uint32_t)(s[*pos + 2u] & 0x3Fu);
    } else {
        cp = ((uint32_t)(s[*pos] & 0x07u) << 18) | ((uint32_t)(s[*pos + 1u] & 0x3Fu) << 12) |
             ((uint32_t)(s[*pos + 2u] & 0x3Fu) << 6) | (uint32_t)(s[*pos + 3u] & 0x3Fu);
    }
    *pos += n;
    if (cp >= 0x10000u) {
        cp -= 0x10000u;
        *pending = 0xDC00u + (cp & 0x3FFu);
        return 0xD800u + (cp >> 10);
    }
    return cp;
}

int asx_json_utf16_cmp(const char *a, size_t a_len, const char *b, size_t b_len) {
    const unsigned char *ua = (const unsigned char *)a;
    const unsigned char *ub = (const unsigned char *)b;
    size_t pa = 0;
    size_t pb = 0;
    uint32_t qa = 0;
    uint32_t qb = 0;
    for (;;) {
        int a_end = pa >= a_len && qa == 0u;
        int b_end = pb >= b_len && qb == 0u;
        uint32_t ca;
        uint32_t cb;
        if (a_end || b_end) return a_end && b_end ? 0 : (a_end ? -1 : 1);
        ca = next_utf16_unit(ua, a_len, &pa, &qa);
        cb = next_utf16_unit(ub, b_len, &pb, &qb);
        if (ca != cb) return ca < cb ? -1 : 1;
    }
}

static int key_less(const asx_json_doc *doc, uint32_t a, uint32_t b) {
    const asx_json_node *na = &doc->nodes[a];
    const asx_json_node *nb = &doc->nodes[b];
    return asx_json_utf16_cmp(&doc->text[na->key], na->key_len, &doc->text[nb->key], nb->key_len) <
           0;
}

static asx_status write_node(const asx_json_doc *doc, uint32_t node, asx_json_out *out,
                             uint32_t depth) {
    const asx_json_node *n = asx_json_at(doc, node);
    uint32_t c;
    if (n == NULL) return ASX_E_INVALID_ARGUMENT;
    if (depth > ASX_JSON_MAX_DEPTH) return ASX_E_RESOURCE_EXHAUSTED;
    switch (n->type) {
    case ASX_JSON_NULL: asx_json_out_cstr(out, "null"); break;
    case ASX_JSON_BOOL: asx_json_out_cstr(out, n->magnitude ? "true" : "false"); break;
    case ASX_JSON_INT:
        if (n->negative) asx_json_out_append(out, "-", 1);
        write_u64(out, n->magnitude);
        break;
    case ASX_JSON_STRING: write_string(out, &doc->text[n->str], n->str_len); break;
    case ASX_JSON_ARRAY:
        asx_json_out_append(out, "[", 1);
        for (c = n->first; c != ASX_JSON_NONE; c = doc->nodes[c].next) {
            asx_status st;
            if (c != n->first) asx_json_out_append(out, ",", 1);
            st = write_node(doc, c, out, depth + 1u);
            if (st != ASX_OK) return st;
        }
        asx_json_out_append(out, "]", 1);
        break;
    case ASX_JSON_OBJECT: {
        /* Emit members in key order by repeated minimum selection: keys are
         * unique, so each round picks the smallest key above the last one. */
        uint32_t prev = ASX_JSON_NONE;
        uint32_t emitted;
        asx_json_out_append(out, "{", 1);
        for (emitted = 0; emitted < n->count; emitted++) {
            uint32_t best = ASX_JSON_NONE;
            asx_status st;
            for (c = n->first; c != ASX_JSON_NONE; c = doc->nodes[c].next) {
                if (prev != ASX_JSON_NONE && !key_less(doc, prev, c)) continue;
                if (best == ASX_JSON_NONE || key_less(doc, c, best)) best = c;
            }
            if (best == ASX_JSON_NONE) return ASX_E_INVALID_STATE; /* duplicate keys */
            if (emitted > 0u) asx_json_out_append(out, ",", 1);
            write_string(out, &doc->text[doc->nodes[best].key], doc->nodes[best].key_len);
            asx_json_out_append(out, ":", 1);
            st = write_node(doc, best, out, depth + 1u);
            if (st != ASX_OK) return st;
            prev = best;
        }
        asx_json_out_append(out, "}", 1);
        break;
    }
    }
    return out->overflow ? ASX_E_BUFFER_TOO_SMALL : ASX_OK;
}

asx_status asx_json_write_canonical(const asx_json_doc *doc, uint32_t node, asx_json_out *out) {
    if (doc == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    return write_node(doc, node, out, 0);
}
