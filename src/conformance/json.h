/*
 * conformance/json.h — bounded JSON DOM and RFC 8785 canonical writer for
 * the conformance oracle (bead W1.5, bd-9kll.2.5).
 *
 * The oracle reads DSL v2 scenarios and asx.fixture.v2 files, builds its
 * own run as a JSON value, and compares canonical bytes. This module is
 * the one JSON implementation it uses for all three.
 *
 * Scope is the subset the vocabulary admits (docs/CANONICAL_VOCABULARY_V2.md
 * §9): null, booleans, integers, strings, arrays and objects. Fractions and
 * exponents are rejected, as are duplicate object keys, so a value has
 * exactly one canonical form. All storage is a caller-owned document with
 * fixed node and text pools; running out of either is an error, never a
 * truncation.
 *
 * Not part of the public API: the conformance tools and tests only.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_CONFORMANCE_JSON_H
#define ASX_CONFORMANCE_JSON_H

#include <asx/asx_status.h>
#include <stddef.h>
#include <stdint.h>

#ifndef ASX_JSON_MAX_NODES
#define ASX_JSON_MAX_NODES 32768u
#endif
#ifndef ASX_JSON_MAX_TEXT
#define ASX_JSON_MAX_TEXT (512u * 1024u)
#endif
#define ASX_JSON_MAX_DEPTH 64u

/* Index of "no node". */
#define ASX_JSON_NONE 0xFFFFFFFFu

typedef enum {
    ASX_JSON_NULL = 0,
    ASX_JSON_BOOL = 1,
    ASX_JSON_INT = 2,
    ASX_JSON_STRING = 3,
    ASX_JSON_ARRAY = 4,
    ASX_JSON_OBJECT = 5
} asx_json_type;

typedef struct {
    asx_json_type type;
    uint32_t key;       /* text offset of the member key, ASX_JSON_NONE outside an object */
    uint32_t key_len;   /* key length in bytes */
    uint32_t str;       /* text offset of a string value (NUL-terminated copy) */
    uint32_t str_len;   /* string length in bytes (may contain NUL) */
    uint64_t magnitude; /* integer magnitude; 0/1 for booleans */
    int negative;       /* integer sign */
    uint32_t first;     /* first child (array element or object member) */
    uint32_t last;      /* last child */
    uint32_t next;      /* next sibling */
    uint32_t count;     /* number of children */
} asx_json_node;

typedef struct {
    asx_json_node nodes[ASX_JSON_MAX_NODES];
    uint32_t node_count;
    char text[ASX_JSON_MAX_TEXT];
    uint32_t text_used;
    /* Parse diagnostics: byte offset and reason of the first error. */
    size_t error_offset;
    const char *error;
} asx_json_doc;

/* Output buffer over caller storage. An overflow sets `overflow` and
 * makes every later write a no-op; asx_json_out_finish reports it. */
typedef struct {
    char *data;
    size_t cap;
    size_t len;
    int overflow;
} asx_json_out;

void asx_json_doc_init(asx_json_doc *doc);

/* Parse `len` bytes of strict JSON into `doc`. On success *out_root is the
 * root node. Errors: ASX_E_INVALID_ARGUMENT for malformed or unsupported
 * input (doc->error says why), ASX_E_RESOURCE_EXHAUSTED when a pool or the
 * depth limit is exceeded. */
ASX_MUST_USE asx_status asx_json_parse(asx_json_doc *doc, const char *src, size_t len,
                                       uint32_t *out_root);

/* ---- reading ---------------------------------------------------------- */

const asx_json_node *asx_json_at(const asx_json_doc *doc, uint32_t node);
asx_json_type asx_json_type_of(const asx_json_doc *doc, uint32_t node);
/* Member `key` of object `obj`, or ASX_JSON_NONE. */
uint32_t asx_json_get(const asx_json_doc *doc, uint32_t obj, const char *key);
/* The `index`-th element of an array or object, or ASX_JSON_NONE. */
uint32_t asx_json_item(const asx_json_doc *doc, uint32_t container, uint32_t index);
uint32_t asx_json_count(const asx_json_doc *doc, uint32_t container);
/* String contents (NUL-terminated), or NULL when `node` is not a string. */
const char *asx_json_string(const asx_json_doc *doc, uint32_t node);
const char *asx_json_key(const asx_json_doc *doc, uint32_t member);
int asx_json_is_null(const asx_json_doc *doc, uint32_t node);
/* Reads a non-negative integer that fits in uint64_t. Returns 1 on success. */
int asx_json_u64(const asx_json_doc *doc, uint32_t node, uint64_t *out);
/* Reads a boolean. Returns 1 on success. */
int asx_json_bool(const asx_json_doc *doc, uint32_t node, int *out);
/* String-valued member of `obj`, or NULL when absent or not a string. */
const char *asx_json_get_string(const asx_json_doc *doc, uint32_t obj, const char *key);

/* ---- building --------------------------------------------------------- */
/* Each constructor returns the new node or ASX_JSON_NONE when a pool is
 * exhausted; the containers propagate ASX_JSON_NONE, so a build sequence
 * can check once at the end with asx_json_doc_ok. */

uint32_t asx_json_new_null(asx_json_doc *doc);
uint32_t asx_json_new_bool(asx_json_doc *doc, int value);
uint32_t asx_json_new_u64(asx_json_doc *doc, uint64_t value);
uint32_t asx_json_new_string(asx_json_doc *doc, const char *s);
uint32_t asx_json_new_string_len(asx_json_doc *doc, const char *s, size_t len);
uint32_t asx_json_new_array(asx_json_doc *doc);
uint32_t asx_json_new_object(asx_json_doc *doc);
/* Append `child` to array `arr`. */
void asx_json_push(asx_json_doc *doc, uint32_t arr, uint32_t child);
/* Add member `key` = `child` to object `obj`, replacing an existing value. */
void asx_json_set(asx_json_doc *doc, uint32_t obj, const char *key, uint32_t child);
/* Deep copy of `node` from `src` into `dst`. */
uint32_t asx_json_copy(asx_json_doc *dst, const asx_json_doc *src, uint32_t node);
/* 0 once any constructor has failed for lack of space. */
int asx_json_doc_ok(const asx_json_doc *doc);

/* ---- canonical form --------------------------------------------------- */

void asx_json_out_init(asx_json_out *out, char *storage, size_t cap);
/* NUL-terminates; returns ASX_E_BUFFER_TOO_SMALL after an overflow. */
ASX_MUST_USE asx_status asx_json_out_finish(asx_json_out *out);
void asx_json_out_append(asx_json_out *out, const char *bytes, size_t len);
void asx_json_out_cstr(asx_json_out *out, const char *s);

/* RFC 8785 canonical bytes of `node`: object members ordered by the UTF-16
 * code units of their keys, minimal string escapes, integers in decimal. */
ASX_MUST_USE asx_status asx_json_write_canonical(const asx_json_doc *doc, uint32_t node,
                                                 asx_json_out *out);

/* Compare two strings by their UTF-16 code units (RFC 8785 key order).
 * Returns <0, 0 or >0. */
int asx_json_utf16_cmp(const char *a, size_t a_len, const char *b, size_t b_len);

#endif /* ASX_CONFORMANCE_JSON_H */
