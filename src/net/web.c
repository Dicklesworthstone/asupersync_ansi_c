/*
 * web.c — web router, middleware, extraction, session, SSE, multipart, security
 *
 * Deterministic in-memory web application surface.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/net/web.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Bounded string helpers                                              */
/* ------------------------------------------------------------------ */

static size_t web_bounded_strlen(const char *str, size_t max) {
    size_t len;

    if (str == NULL) return 0u;
    for (len = 0u; len < max; len++) {
        if (str[len] == '\0') return len;
    }
    return max;
}

static int web_path_match(const char *pattern, const char *uri) {
    size_t plen;

    if (pattern == NULL || uri == NULL) return 0;
    plen = web_bounded_strlen(pattern, ASX_WEB_PATH_MAX);
    /* Match up to the query string delimiter */
    {
        size_t i;
        for (i = 0u; i < plen; i++) {
            if (uri[i] == '\0' || uri[i] == '?') return 0;
            if (uri[i] != pattern[i]) return 0;
        }
        /* Pattern must match fully; uri may continue with '?' or end */
        return uri[plen] == '\0' || uri[plen] == '?';
    }
}

/* ------------------------------------------------------------------ */
/* Router                                                              */
/* ------------------------------------------------------------------ */

void asx_web_router_init(asx_web_router *router) {
    if (router == NULL) return;
    memset(router, 0, sizeof(*router));
}

asx_status asx_web_router_add(asx_web_router *router, asx_http_method method, const char *path,
                               asx_web_handler_fn handler, void *user_data) {
    size_t len;
    asx_web_route *r;

    if (router == NULL || path == NULL || handler == NULL) return ASX_E_INVALID_ARGUMENT;
    if (router->count >= ASX_WEB_MAX_ROUTES) return ASX_E_RESOURCE_EXHAUSTED;

    len = web_bounded_strlen(path, ASX_WEB_PATH_MAX);
    if (len == 0u || len >= ASX_WEB_PATH_MAX) return ASX_E_INVALID_ARGUMENT;

    r = &router->routes[router->count];
    memset(r, 0, sizeof(*r));
    r->method = method;
    memcpy(r->path, path, len + 1u);
    r->handler = handler;
    r->user_data = user_data;
    r->active = 1u;
    router->count++;
    return ASX_OK;
}

void asx_web_router_set_not_found(asx_web_router *router, asx_web_handler_fn handler,
                                   void *user_data) {
    if (router == NULL) return;
    router->not_found_handler = handler;
    router->not_found_user_data = user_data;
}

asx_status asx_web_router_dispatch(const asx_web_router *router, const asx_http_request *req,
                                    asx_http_response *resp) {
    uint32_t i;
    int path_matched;

    if (router == NULL || req == NULL || resp == NULL) return ASX_E_INVALID_ARGUMENT;

    path_matched = 0;
    for (i = 0u; i < router->count; i++) {
        if (!router->routes[i].active) continue;
        if (web_path_match(router->routes[i].path, req->uri)) {
            path_matched = 1;
            if (router->routes[i].method == req->method) {
                return router->routes[i].handler(req, resp, router->routes[i].user_data);
            }
        }
    }

    if (path_matched) {
        /* Path matched but method didn't — 405 */
        asx_http_response_init(resp, ASX_HTTP_405_METHOD_NOT_ALLOWED);
        return ASX_OK;
    }

    /* No route matched — 404 */
    if (router->not_found_handler != NULL) {
        return router->not_found_handler(req, resp, router->not_found_user_data);
    }
    asx_http_response_init(resp, ASX_HTTP_404_NOT_FOUND);
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Middleware                                                           */
/* ------------------------------------------------------------------ */

void asx_web_middleware_init(asx_web_middleware_stack *stack, asx_web_handler_fn inner,
                              void *inner_data) {
    if (stack == NULL) return;
    memset(stack, 0, sizeof(*stack));
    stack->inner = inner;
    stack->inner_data = inner_data;
}

asx_status asx_web_middleware_add(asx_web_middleware_stack *stack, asx_web_middleware_fn fn,
                                   void *user_data) {
    if (stack == NULL || fn == NULL) return ASX_E_INVALID_ARGUMENT;
    if (stack->count >= ASX_WEB_MAX_MIDDLEWARE) return ASX_E_RESOURCE_EXHAUSTED;
    stack->layers[stack->count].fn = fn;
    stack->layers[stack->count].user_data = user_data;
    stack->count++;
    return ASX_OK;
}

/* Execute middleware chain: layer[count-1] wraps ... wraps layer[0] wraps inner. */
typedef struct {
    const asx_web_middleware_stack *stack;
    uint32_t index;
} mw_chain_ctx;

static asx_status mw_chain_next(const asx_http_request *req, asx_http_response *resp, void *ctx);

static asx_status mw_chain_next(const asx_http_request *req, asx_http_response *resp, void *ctx) {
    mw_chain_ctx *c = (mw_chain_ctx *)ctx;
    const asx_web_middleware_stack *stack;
    uint32_t idx;

    if (c == NULL) return ASX_E_INVALID_ARGUMENT;
    stack = c->stack;
    idx = c->index;

    if (idx >= stack->count) {
        /* All middleware exhausted, call inner handler */
        if (stack->inner != NULL) {
            return stack->inner(req, resp, stack->inner_data);
        }
        return ASX_OK;
    }

    /* Advance to next layer */
    {
        mw_chain_ctx next_ctx;
        next_ctx.stack = stack;
        next_ctx.index = idx + 1u;
        return stack->layers[idx].fn(req, resp, stack->layers[idx].user_data, mw_chain_next,
                                     &next_ctx);
    }
}

asx_status asx_web_middleware_run(const asx_web_middleware_stack *stack,
                                   const asx_http_request *req, asx_http_response *resp) {
    mw_chain_ctx ctx;

    if (stack == NULL || req == NULL || resp == NULL) return ASX_E_INVALID_ARGUMENT;
    ctx.stack = stack;
    ctx.index = 0u;
    return mw_chain_next(req, resp, &ctx);
}

/* ------------------------------------------------------------------ */
/* Request extraction                                                  */
/* ------------------------------------------------------------------ */

asx_status asx_web_query_parse(asx_web_query *out, const char *uri) {
    const char *qstart;
    const char *p;
    const char *key_start;
    const char *val_start;
    size_t key_len, val_len;

    if (out == NULL || uri == NULL) return ASX_E_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    /* Find '?' */
    qstart = NULL;
    for (p = uri; *p != '\0'; p++) {
        if (*p == '?') {
            qstart = p + 1;
            break;
        }
    }
    if (qstart == NULL || *qstart == '\0') return ASX_OK; /* No query string */

    p = qstart;
    while (*p != '\0' && out->count < ASX_WEB_MAX_QUERY_PARAMS) {
        key_start = p;
        val_start = NULL;

        /* Scan to '=' or '&' or end */
        while (*p != '\0' && *p != '=' && *p != '&') p++;

        if (*p == '=') {
            key_len = (size_t)(p - key_start);
            p++; /* skip '=' */
            val_start = p;
            while (*p != '\0' && *p != '&') p++;
            val_len = (size_t)(p - val_start);
        } else {
            key_len = (size_t)(p - key_start);
            val_len = 0u;
        }

        if (key_len > 0u && key_len < ASX_WEB_PARAM_KEY_MAX) {
            asx_web_query_param *param = &out->params[out->count];
            memcpy(param->key, key_start, key_len);
            param->key[key_len] = '\0';
            if (val_start != NULL && val_len < ASX_WEB_PARAM_VALUE_MAX) {
                memcpy(param->value, val_start, val_len);
                param->value[val_len] = '\0';
            }
            out->count++;
        }

        if (*p == '&') p++;
    }
    return ASX_OK;
}

const char *asx_web_query_get(const asx_web_query *query, const char *key) {
    uint32_t i;

    if (query == NULL || key == NULL) return NULL;
    for (i = 0u; i < query->count; i++) {
        if (strcmp(query->params[i].key, key) == 0) {
            return query->params[i].value;
        }
    }
    return NULL;
}

const char *asx_web_extract_header(const asx_http_request *req, const char *name) {
    if (req == NULL) return NULL;
    return asx_http_headers_get(&req->headers, name);
}

const char *asx_web_extract_content_type(const asx_http_request *req) {
    return asx_web_extract_header(req, "Content-Type");
}

/* ------------------------------------------------------------------ */
/* Session                                                             */
/* ------------------------------------------------------------------ */

void asx_web_session_store_init(asx_web_session_store *store) {
    if (store == NULL) return;
    memset(store, 0, sizeof(*store));
    store->next_seq = 1u;
}

static void session_generate_id(char *out, uint32_t capacity, uint32_t seq) {
    /* Deterministic hex-encoded session ID from sequence number */
    static const char hex[] = "0123456789abcdef";
    uint32_t i;
    uint32_t val;

    if (out == NULL || capacity == 0u) return;
    memset(out, '0', capacity - 1u < ASX_WEB_SESSION_ID_LEN ? capacity - 1u
                                                              : ASX_WEB_SESSION_ID_LEN);
    val = seq;
    /* Write hex digits right-to-left */
    i = ASX_WEB_SESSION_ID_LEN < capacity ? ASX_WEB_SESSION_ID_LEN : capacity - 1u;
    out[i] = '\0';
    while (i > 0u && val > 0u) {
        i--;
        out[i] = hex[val & 0xFu];
        val >>= 4;
    }
}

asx_status asx_web_session_create(asx_web_session_store *store, char *out_id,
                                    uint32_t id_capacity) {
    uint32_t idx;
    asx_web_session *s;

    if (store == NULL || out_id == NULL) return ASX_E_INVALID_ARGUMENT;
    if (id_capacity <= ASX_WEB_SESSION_ID_LEN) return ASX_E_BUFFER_TOO_SMALL;

    for (idx = 0u; idx < ASX_WEB_MAX_SESSIONS; idx++) {
        if (!store->sessions[idx].active) break;
    }
    if (idx >= ASX_WEB_MAX_SESSIONS) return ASX_E_RESOURCE_EXHAUSTED;

    s = &store->sessions[idx];
    memset(s, 0, sizeof(*s));
    session_generate_id(s->id, sizeof(s->id), store->next_seq++);
    s->active = 1u;

    memcpy(out_id, s->id, ASX_WEB_SESSION_ID_LEN + 1u);
    return ASX_OK;
}

asx_web_session *asx_web_session_get(asx_web_session_store *store, const char *id) {
    uint32_t idx;

    if (store == NULL || id == NULL) return NULL;
    for (idx = 0u; idx < ASX_WEB_MAX_SESSIONS; idx++) {
        if (store->sessions[idx].active && strcmp(store->sessions[idx].id, id) == 0) {
            return &store->sessions[idx];
        }
    }
    return NULL;
}

asx_status asx_web_session_set_data(asx_web_session *session, const void *data, uint32_t len) {
    if (session == NULL) return ASX_E_INVALID_ARGUMENT;
    if (len > ASX_WEB_SESSION_MAX_DATA) return ASX_E_BUFFER_TOO_SMALL;
    if (len > 0u && data == NULL) return ASX_E_INVALID_ARGUMENT;
    memset(session->data, 0, sizeof(session->data));
    session->data_len = len;
    if (len > 0u) memcpy(session->data, data, len);
    return ASX_OK;
}

asx_status asx_web_session_destroy(asx_web_session_store *store, const char *id) {
    uint32_t idx;

    if (store == NULL || id == NULL) return ASX_E_INVALID_ARGUMENT;
    for (idx = 0u; idx < ASX_WEB_MAX_SESSIONS; idx++) {
        if (store->sessions[idx].active && strcmp(store->sessions[idx].id, id) == 0) {
            memset(&store->sessions[idx], 0, sizeof(store->sessions[idx]));
            return ASX_OK;
        }
    }
    return ASX_E_NOT_FOUND;
}

/* ------------------------------------------------------------------ */
/* Server-Sent Events                                                  */
/* ------------------------------------------------------------------ */

void asx_web_sse_init(asx_web_sse_stream *stream) {
    if (stream == NULL) return;
    memset(stream, 0, sizeof(*stream));
    stream->next_id = 1u;
}

asx_status asx_web_sse_push(asx_web_sse_stream *stream, const char *event_name,
                              const char *data) {
    uint32_t tail;
    asx_web_sse_event *evt;
    size_t elen, dlen;

    if (stream == NULL || data == NULL) return ASX_E_INVALID_ARGUMENT;
    if (stream->closed) return ASX_E_INVALID_STATE;
    if (stream->count >= ASX_WEB_SSE_MAX_EVENTS) return ASX_E_WOULD_BLOCK;

    dlen = web_bounded_strlen(data, ASX_WEB_SSE_DATA_MAX);
    if (dlen >= ASX_WEB_SSE_DATA_MAX) return ASX_E_BUFFER_TOO_SMALL;

    tail = (stream->head + stream->count) % ASX_WEB_SSE_MAX_EVENTS;
    evt = &stream->events[tail];
    memset(evt, 0, sizeof(*evt));

    if (event_name != NULL) {
        elen = web_bounded_strlen(event_name, ASX_WEB_SSE_EVENT_NAME_MAX);
        if (elen >= ASX_WEB_SSE_EVENT_NAME_MAX) return ASX_E_BUFFER_TOO_SMALL;
        memcpy(evt->event, event_name, elen + 1u);
    }
    memcpy(evt->data, data, dlen + 1u);
    evt->id = stream->next_id++;
    stream->count++;
    return ASX_OK;
}

asx_status asx_web_sse_poll(asx_web_sse_stream *stream, asx_web_sse_event *out) {
    if (stream == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    if (stream->count == 0u) return ASX_E_PENDING;

    *out = stream->events[stream->head];
    stream->head = (stream->head + 1u) % ASX_WEB_SSE_MAX_EVENTS;
    stream->count--;
    return ASX_OK;
}

void asx_web_sse_close(asx_web_sse_stream *stream) {
    if (stream == NULL) return;
    stream->closed = 1u;
}

int asx_web_sse_is_open(const asx_web_sse_stream *stream) {
    if (stream == NULL) return 0;
    return !stream->closed;
}

/* ------------------------------------------------------------------ */
/* Multipart                                                           */
/* ------------------------------------------------------------------ */

void asx_web_multipart_init(asx_web_multipart *mp) {
    if (mp == NULL) return;
    memset(mp, 0, sizeof(*mp));
}

asx_status asx_web_multipart_add_field(asx_web_multipart *mp, const char *name, const void *data,
                                         uint32_t len) {
    size_t nlen;
    asx_web_multipart_part *p;

    if (mp == NULL || name == NULL) return ASX_E_INVALID_ARGUMENT;
    if (mp->count >= ASX_WEB_MAX_MULTIPART_PARTS) return ASX_E_RESOURCE_EXHAUSTED;
    if (len > ASX_WEB_MULTIPART_DATA_MAX) return ASX_E_BUFFER_TOO_SMALL;
    nlen = web_bounded_strlen(name, ASX_WEB_MULTIPART_NAME_MAX);
    if (nlen == 0u || nlen >= ASX_WEB_MULTIPART_NAME_MAX) return ASX_E_INVALID_ARGUMENT;

    p = &mp->parts[mp->count];
    memset(p, 0, sizeof(*p));
    memcpy(p->name, name, nlen + 1u);
    p->data_len = len;
    p->is_file = 0u;
    if (len > 0u && data != NULL) memcpy(p->data, data, len);
    mp->count++;
    return ASX_OK;
}

asx_status asx_web_multipart_add_file(asx_web_multipart *mp, const char *name,
                                        const char *filename, const char *content_type,
                                        const void *data, uint32_t len) {
    size_t nlen, flen, clen;
    asx_web_multipart_part *p;

    if (mp == NULL || name == NULL || filename == NULL) return ASX_E_INVALID_ARGUMENT;
    if (mp->count >= ASX_WEB_MAX_MULTIPART_PARTS) return ASX_E_RESOURCE_EXHAUSTED;
    if (len > ASX_WEB_MULTIPART_DATA_MAX) return ASX_E_BUFFER_TOO_SMALL;

    nlen = web_bounded_strlen(name, ASX_WEB_MULTIPART_NAME_MAX);
    flen = web_bounded_strlen(filename, ASX_WEB_MULTIPART_FILENAME_MAX);
    if (nlen == 0u || nlen >= ASX_WEB_MULTIPART_NAME_MAX) return ASX_E_INVALID_ARGUMENT;
    if (flen == 0u || flen >= ASX_WEB_MULTIPART_FILENAME_MAX) return ASX_E_INVALID_ARGUMENT;

    p = &mp->parts[mp->count];
    memset(p, 0, sizeof(*p));
    memcpy(p->name, name, nlen + 1u);
    memcpy(p->filename, filename, flen + 1u);
    if (content_type != NULL) {
        clen = web_bounded_strlen(content_type, ASX_HTTP_HEADER_VALUE_MAX);
        if (clen >= ASX_HTTP_HEADER_VALUE_MAX) return ASX_E_BUFFER_TOO_SMALL;
        memcpy(p->content_type, content_type, clen + 1u);
    }
    p->data_len = len;
    p->is_file = 1u;
    if (len > 0u && data != NULL) memcpy(p->data, data, len);
    mp->count++;
    return ASX_OK;
}

const asx_web_multipart_part *asx_web_multipart_get(const asx_web_multipart *mp,
                                                      const char *name) {
    uint32_t i;

    if (mp == NULL || name == NULL) return NULL;
    for (i = 0u; i < mp->count; i++) {
        if (strcmp(mp->parts[i].name, name) == 0) return &mp->parts[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* CORS                                                                */
/* ------------------------------------------------------------------ */

void asx_web_cors_config_init(asx_web_cors_config *cfg) {
    if (cfg == NULL) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->allow_credentials = 0u;
    cfg->max_age_seconds = 86400u;
}

asx_status asx_web_cors_add_origin(asx_web_cors_config *cfg, const char *origin) {
    size_t len;

    if (cfg == NULL || origin == NULL) return ASX_E_INVALID_ARGUMENT;
    if (cfg->origin_count >= ASX_WEB_CORS_MAX_ORIGINS) return ASX_E_RESOURCE_EXHAUSTED;
    len = web_bounded_strlen(origin, ASX_WEB_CORS_ORIGIN_MAX);
    if (len == 0u || len >= ASX_WEB_CORS_ORIGIN_MAX) return ASX_E_INVALID_ARGUMENT;
    memcpy(cfg->allowed_origins[cfg->origin_count], origin, len + 1u);
    cfg->origin_count++;
    return ASX_OK;
}

int asx_web_cors_check_origin(const asx_web_cors_config *cfg, const char *origin) {
    uint32_t i;

    if (cfg == NULL || origin == NULL) return 0;
    if (cfg->origin_count == 0u) return 1; /* No restrictions = allow all */
    for (i = 0u; i < cfg->origin_count; i++) {
        if (strcmp(cfg->allowed_origins[i], origin) == 0) return 1;
        if (strcmp(cfg->allowed_origins[i], "*") == 0) return 1;
    }
    return 0;
}

asx_status asx_web_cors_apply(const asx_web_cors_config *cfg, const char *request_origin,
                                asx_http_response *resp) {
    if (cfg == NULL || resp == NULL) return ASX_E_INVALID_ARGUMENT;
    if (request_origin == NULL) return ASX_OK; /* No origin header = not a CORS request */

    if (!asx_web_cors_check_origin(cfg, request_origin)) {
        return ASX_OK; /* Origin not allowed — don't add CORS headers */
    }

    (void)asx_http_headers_add(&resp->headers, "Access-Control-Allow-Origin", request_origin);
    if (cfg->allow_credentials) {
        (void)asx_http_headers_add(&resp->headers, "Access-Control-Allow-Credentials", "true");
    }
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* CSRF                                                                */
/* ------------------------------------------------------------------ */

void asx_web_csrf_init(asx_web_csrf *csrf, uint32_t seed) {
    static const char hex[] = "0123456789abcdef";
    uint32_t i;
    uint32_t val;

    if (csrf == NULL) return;
    memset(csrf, 0, sizeof(*csrf));
    csrf->seq = seed;

    /* Generate deterministic token from seed using simple hash expansion */
    val = seed;
    for (i = 0u; i < ASX_WEB_CSRF_TOKEN_LEN; i++) {
        val = val * 2654435761u + (uint32_t)i; /* Knuth multiplicative hash */
        csrf->token[i] = hex[(val >> 28) & 0xFu];
    }
    csrf->token[ASX_WEB_CSRF_TOKEN_LEN] = '\0';
}

const char *asx_web_csrf_token(const asx_web_csrf *csrf) {
    if (csrf == NULL) return "";
    return csrf->token;
}

int asx_web_csrf_validate(const asx_web_csrf *csrf, const char *submitted) {
    if (csrf == NULL || submitted == NULL) return 0;
    return strcmp(csrf->token, submitted) == 0;
}

/* ------------------------------------------------------------------ */
/* Static file serving                                                 */
/* ------------------------------------------------------------------ */

void asx_web_static_init(asx_web_static_files *sf) {
    if (sf == NULL) return;
    memset(sf, 0, sizeof(*sf));
}

asx_status asx_web_static_add(asx_web_static_files *sf, const char *path,
                                const char *content_type, const uint8_t *data, uint32_t data_len) {
    size_t plen, clen;
    asx_web_static_entry *e;

    if (sf == NULL || path == NULL || content_type == NULL) return ASX_E_INVALID_ARGUMENT;
    if (sf->count >= ASX_WEB_MAX_STATIC_FILES) return ASX_E_RESOURCE_EXHAUSTED;

    plen = web_bounded_strlen(path, ASX_WEB_PATH_MAX);
    clen = web_bounded_strlen(content_type, ASX_HTTP_HEADER_VALUE_MAX);
    if (plen == 0u || plen >= ASX_WEB_PATH_MAX) return ASX_E_INVALID_ARGUMENT;
    if (clen == 0u || clen >= ASX_HTTP_HEADER_VALUE_MAX) return ASX_E_INVALID_ARGUMENT;

    e = &sf->files[sf->count];
    memset(e, 0, sizeof(*e));
    memcpy(e->path, path, plen + 1u);
    memcpy(e->content_type, content_type, clen + 1u);
    e->data = data;
    e->data_len = data_len;
    sf->count++;
    return ASX_OK;
}

const asx_web_static_entry *asx_web_static_lookup(const asx_web_static_files *sf,
                                                    const char *path) {
    uint32_t i;

    if (sf == NULL || path == NULL) return NULL;
    for (i = 0u; i < sf->count; i++) {
        if (strcmp(sf->files[i].path, path) == 0) return &sf->files[i];
    }
    return NULL;
}

asx_status asx_web_static_serve(const asx_web_static_files *sf, const char *path,
                                  asx_http_response *resp) {
    const asx_web_static_entry *entry;

    if (sf == NULL || path == NULL || resp == NULL) return ASX_E_INVALID_ARGUMENT;

    entry = asx_web_static_lookup(sf, path);
    if (entry == NULL) {
        asx_http_response_init(resp, ASX_HTTP_404_NOT_FOUND);
        return ASX_OK;
    }

    asx_http_response_init(resp, ASX_HTTP_200_OK);
    (void)asx_http_headers_add(&resp->headers, "Content-Type", entry->content_type);
    if (entry->data != NULL && entry->data_len > 0u) {
        (void)asx_http_body_set_bytes(&resp->body, entry->data, entry->data_len);
    }
    return ASX_OK;
}
