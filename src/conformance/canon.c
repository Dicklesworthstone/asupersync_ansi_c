/*
 * conformance/canon.c — footprints, independence, Foata layering and
 * digests for vocabulary v2 runs (bead W1.5, bd-9kll.2.5). The C twin of
 * tools/twin_run/src/canon.rs; see canon.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "canon.h"

#include <asx/security/crypto.h>
#include <string.h>

typedef enum { RES_TASK = 0, RES_REGION, RES_OBLIGATION, RES_TIMER, RES_CLOCK } res_type;

typedef struct {
    res_type type;
    const char *name; /* NUL-terminated, in the document's text pool */
    int write;
} res_access;

/* obligation.handoff writes five resources; no kind touches more. */
#define MAX_ACCESSES 5u

static const char *g_error;

const char *asx_canon_error(void) { return g_error; }

static asx_status canon_fail(const char *why) {
    g_error = why;
    return ASX_E_INVALID_ARGUMENT;
}

static void add_access(res_access *fp, uint32_t *n, res_type type, const char *name, int write) {
    fp[*n].type = type;
    fp[*n].name = name;
    fp[*n].write = write;
    (*n)++;
}

/* Resource footprint of one vocabulary event (§8). */
static asx_status footprint(const asx_json_doc *doc, uint32_t ev, res_access fp[MAX_ACCESSES],
                            uint32_t *n) {
    const char *k = asx_json_get_string(doc, ev, "k");
    const char *a;
    const char *b;
    *n = 0;
    if (k == NULL) return canon_fail("vocabulary event lacks string field \"k\"");

    if (strcmp(k, "task.spawned") == 0 || strcmp(k, "task.completed") == 0) {
        a = asx_json_get_string(doc, ev, "task");
        b = asx_json_get_string(doc, ev, "region");
        if (a == NULL || b == NULL) return canon_fail("task event lacks task/region");
        add_access(fp, n, RES_TASK, a, 1);
        add_access(fp, n, RES_REGION, b, 0);
    } else if (strcmp(k, "cancel.requested") == 0) {
        a = asx_json_get_string(doc, ev, "task");
        b = asx_json_get_string(doc, ev, "region");
        if (a == NULL || b == NULL) return canon_fail("cancel.requested lacks task/region");
        add_access(fp, n, RES_TASK, a, 1);
        add_access(fp, n, RES_REGION, b, 1);
    } else if (strcmp(k, "region.created") == 0) {
        a = asx_json_get_string(doc, ev, "region");
        if (a == NULL) return canon_fail("region.created lacks region");
        add_access(fp, n, RES_REGION, a, 1);
        b = asx_json_get_string(doc, ev, "parent");
        if (b != NULL) add_access(fp, n, RES_REGION, b, 0);
    } else if (strcmp(k, "region.close_begin") == 0 || strcmp(k, "region.closed") == 0 ||
               strcmp(k, "region.cancelled") == 0) {
        a = asx_json_get_string(doc, ev, "region");
        if (a == NULL) return canon_fail("region event lacks region");
        add_access(fp, n, RES_REGION, a, 1);
    } else if (strcmp(k, "obligation.reserved") == 0 || strcmp(k, "obligation.committed") == 0 ||
               strcmp(k, "obligation.aborted") == 0 || strcmp(k, "obligation.leaked") == 0) {
        const char *o = asx_json_get_string(doc, ev, "obligation");
        a = asx_json_get_string(doc, ev, "task");
        b = asx_json_get_string(doc, ev, "region");
        if (o == NULL || a == NULL || b == NULL) {
            return canon_fail("obligation event lacks obligation/task/region");
        }
        add_access(fp, n, RES_OBLIGATION, o, 1);
        add_access(fp, n, RES_TASK, a, 0);
        add_access(fp, n, RES_REGION, b, 0);
    } else if (strcmp(k, "obligation.handoff") == 0) {
        const char *o = asx_json_get_string(doc, ev, "obligation");
        const char *ft = asx_json_get_string(doc, ev, "from_task");
        const char *tt = asx_json_get_string(doc, ev, "to_task");
        const char *fr = asx_json_get_string(doc, ev, "from_region");
        const char *tr = asx_json_get_string(doc, ev, "to_region");
        if (o == NULL || ft == NULL || tt == NULL || fr == NULL || tr == NULL) {
            return canon_fail("obligation.handoff lacks a field");
        }
        add_access(fp, n, RES_OBLIGATION, o, 1);
        add_access(fp, n, RES_TASK, ft, 1);
        add_access(fp, n, RES_TASK, tt, 1);
        add_access(fp, n, RES_REGION, fr, 1);
        add_access(fp, n, RES_REGION, tr, 1);
    } else if (strcmp(k, "timer.scheduled") == 0 || strcmp(k, "timer.fired") == 0 ||
               strcmp(k, "timer.cancelled") == 0) {
        a = asx_json_get_string(doc, ev, "timer");
        if (a == NULL) return canon_fail("timer event lacks timer");
        add_access(fp, n, RES_TIMER, a, 1);
        add_access(fp, n, RES_CLOCK, "", 0);
    } else if (strcmp(k, "user.trace") == 0) {
        /* commutes with everything */
    } else {
        return canon_fail("not a vocabulary v2 event kind");
    }
    return ASX_OK;
}

static int same_resource(const res_access *x, const res_access *y) {
    return x->type == y->type && strcmp(x->name, y->name) == 0;
}

asx_status asx_canon_independent(const asx_json_doc *doc, uint32_t a, uint32_t b,
                                 int *out_independent) {
    res_access fa[MAX_ACCESSES];
    res_access fb[MAX_ACCESSES];
    uint32_t na;
    uint32_t nb;
    uint32_t i;
    uint32_t j;
    asx_status st;
    if (doc == NULL || out_independent == NULL) return ASX_E_INVALID_ARGUMENT;
    st = footprint(doc, a, fa, &na);
    if (st != ASX_OK) return st;
    st = footprint(doc, b, fb, &nb);
    if (st != ASX_OK) return st;
    *out_independent = 1;
    for (i = 0; i < na; i++) {
        for (j = 0; j < nb; j++) {
            if (same_resource(&fa[i], &fb[j]) && (fa[i].write || fb[j].write)) {
                *out_independent = 0;
                return ASX_OK;
            }
        }
    }
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Foata layering                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    res_type type;
    const char *name;
    int64_t any;   /* highest layer of any access, -1 if none */
    int64_t write; /* highest layer of a write, -1 if none */
} res_state;

#define CANON_BYTES_CAP (1024u * 1024u)

static res_state g_res[ASX_CANON_MAX_RESOURCES];
static uint32_t g_layer_of[ASX_CANON_MAX_EVENTS];
static uint32_t g_event_node[ASX_CANON_MAX_EVENTS];
static uint32_t g_bytes_off[ASX_CANON_MAX_EVENTS];
static uint32_t g_bytes_len[ASX_CANON_MAX_EVENTS];
static uint32_t g_order[ASX_CANON_MAX_EVENTS];
static char g_bytes[CANON_BYTES_CAP];

static res_state *res_lookup(uint32_t *count, const res_access *acc) {
    uint32_t i;
    for (i = 0; i < *count; i++) {
        if (g_res[i].type == acc->type && strcmp(g_res[i].name, acc->name) == 0) return &g_res[i];
    }
    if (*count >= ASX_CANON_MAX_RESOURCES) return NULL;
    g_res[*count].type = acc->type;
    g_res[*count].name = acc->name;
    g_res[*count].any = -1;
    g_res[*count].write = -1;
    return &g_res[(*count)++];
}

/* Rust `String` ordering: bytewise, a proper prefix first. */
static int bytes_less(uint32_t a, uint32_t b) {
    uint32_t la = g_bytes_len[a];
    uint32_t lb = g_bytes_len[b];
    int c = memcmp(&g_bytes[g_bytes_off[a]], &g_bytes[g_bytes_off[b]], la < lb ? la : lb);
    if (c != 0) return c < 0;
    return la < lb;
}

asx_status asx_canon_trace(const asx_json_doc *src, uint32_t events, asx_json_doc *dst,
                           uint32_t *out_layers) {
    uint32_t count;
    uint32_t res_count = 0;
    uint32_t bytes_used = 0;
    uint32_t max_layer = 0;
    uint32_t i;
    uint32_t ev;
    uint32_t layers;
    asx_status st;

    g_error = NULL;
    if (src == NULL || dst == NULL || out_layers == NULL) return ASX_E_INVALID_ARGUMENT;
    if (asx_json_type_of(src, events) != ASX_JSON_ARRAY) return canon_fail("trace is not an array");
    count = asx_json_count(src, events);
    if (count > ASX_CANON_MAX_EVENTS) {
        g_error = "too many events";
        return ASX_E_RESOURCE_EXHAUSTED;
    }

    for (i = 0, ev = asx_json_item(src, events, 0); i < count; i++, ev = src->nodes[ev].next) {
        res_access fp[MAX_ACCESSES];
        uint32_t n;
        uint32_t a;
        uint32_t layer = 0;
        asx_json_out out;

        st = footprint(src, ev, fp, &n);
        if (st != ASX_OK) return st;
        for (a = 0; a < n; a++) {
            res_state *r = res_lookup(&res_count, &fp[a]);
            int64_t dep;
            if (r == NULL) {
                g_error = "too many resources";
                return ASX_E_RESOURCE_EXHAUSTED;
            }
            dep = fp[a].write ? r->any : r->write;
            if (dep >= 0 && (uint32_t)dep + 1u > layer) layer = (uint32_t)dep + 1u;
        }
        for (a = 0; a < n; a++) {
            res_state *r = res_lookup(&res_count, &fp[a]);
            if (r == NULL) {
                g_error = "too many resources";
                return ASX_E_RESOURCE_EXHAUSTED;
            }
            if (r->any < (int64_t)layer) r->any = (int64_t)layer;
            if (fp[a].write && r->write < (int64_t)layer) r->write = (int64_t)layer;
        }
        g_layer_of[i] = layer;
        g_event_node[i] = ev;
        if (layer > max_layer) max_layer = layer;

        asx_json_out_init(&out, &g_bytes[bytes_used], CANON_BYTES_CAP - bytes_used);
        st = asx_json_write_canonical(src, ev, &out);
        if (st == ASX_OK) st = asx_json_out_finish(&out);
        if (st != ASX_OK) {
            g_error = "canonical event bytes exceed the canonicalizer buffer";
            return st == ASX_E_BUFFER_TOO_SMALL ? ASX_E_RESOURCE_EXHAUSTED : st;
        }
        g_bytes_off[i] = bytes_used;
        g_bytes_len[i] = (uint32_t)out.len;
        bytes_used += (uint32_t)out.len + 1u; /* keep the terminator */
    }

    layers = asx_json_new_array(dst);
    if (count == 0u) {
        *out_layers = layers;
        return asx_json_doc_ok(dst) ? ASX_OK : ASX_E_RESOURCE_EXHAUSTED;
    }
    for (i = 0; i <= max_layer; i++) {
        uint32_t layer_arr = asx_json_new_array(dst);
        uint32_t m = 0;
        uint32_t j;
        for (j = 0; j < count; j++) {
            if (g_layer_of[j] != i) continue;
            /* Insertion sort by canonical bytes; equal events keep stream
             * order, which cannot be observed since their bytes are equal. */
            {
                uint32_t pos = m;
                while (pos > 0u && bytes_less(j, g_order[pos - 1u])) {
                    g_order[pos] = g_order[pos - 1u];
                    pos--;
                }
                g_order[pos] = j;
                m++;
            }
        }
        for (j = 0; j < m; j++) {
            asx_json_push(dst, layer_arr, asx_json_copy(dst, src, g_event_node[g_order[j]]));
        }
        asx_json_push(dst, layers, layer_arr);
    }
    if (!asx_json_doc_ok(dst)) {
        g_error = "output document exhausted";
        return ASX_E_RESOURCE_EXHAUSTED;
    }
    *out_layers = layers;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Digest                                                              */
/* ------------------------------------------------------------------ */

static void digest_flush(void *ctx, const char *bytes, size_t len) {
    asx_sha256_update((asx_sha256_ctx *)ctx, bytes, len);
}

asx_status asx_canon_digest(const asx_json_doc *doc, uint32_t node,
                            char out[ASX_CANON_DIGEST_LEN]) {
    static const char hex[] = "0123456789abcdef";
    asx_sha256_ctx sha;
    asx_json_out stream;
    char chunk[4096];
    uint8_t hash[ASX_SHA256_DIGEST_SIZE];
    asx_status st;
    uint32_t i;
    if (doc == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    asx_sha256_init(&sha);
    asx_json_out_init_stream(&stream, chunk, sizeof(chunk), digest_flush, &sha);
    st = asx_json_write_canonical(doc, node, &stream);
    if (st == ASX_OK) st = asx_json_out_finish(&stream);
    if (st != ASX_OK) return st;
    asx_sha256_final(&sha, hash);
    memcpy(out, "sha256:", 7);
    for (i = 0; i < ASX_SHA256_DIGEST_SIZE; i++) {
        out[7u + 2u * i] = hex[hash[i] >> 4];
        out[8u + 2u * i] = hex[hash[i] & 0x0Fu];
    }
    out[7u + 2u * ASX_SHA256_DIGEST_SIZE] = '\0';
    return ASX_OK;
}
