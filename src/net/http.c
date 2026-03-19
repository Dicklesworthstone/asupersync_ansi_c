/*
 * http.c — HTTP request/response types, connection pool, and version support
 *
 * Deterministic in-memory HTTP surface. All operations are value-type
 * manipulations with no real network I/O.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/net/http.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Bounded string helpers                                              */
/* ------------------------------------------------------------------ */

static size_t http_bounded_strlen(const char *str, size_t max) {
    size_t len;

    if (str == NULL) return 0u;
    for (len = 0u; len < max; len++) {
        if (str[len] == '\0') return len;
    }
    return max;
}

static int http_strcasecmp(const char *a, const char *b) {
    if (a == NULL || b == NULL) return 1;
    for (;;) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + ('a' - 'A'));
        if (ca != cb) return 1;
        if (ca == '\0') return 0;
    }
}

/* ------------------------------------------------------------------ */
/* HTTP method                                                         */
/* ------------------------------------------------------------------ */

static const char *const g_method_names[] = {
    "GET", "POST", "PUT", "DELETE", "PATCH", "HEAD", "OPTIONS", "CONNECT", "TRACE"
};

const char *asx_http_method_str(asx_http_method method) {
    if ((unsigned)method > 8u) return "UNKNOWN";
    return g_method_names[(unsigned)method];
}

/* ------------------------------------------------------------------ */
/* HTTP status                                                         */
/* ------------------------------------------------------------------ */

const char *asx_http_status_reason(asx_http_status code) {
    switch (code) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default: return "Unknown";
    }
}

int asx_http_status_is_info(asx_http_status code) {
    return code >= 100u && code < 200u;
}

int asx_http_status_is_success(asx_http_status code) {
    return code >= 200u && code < 300u;
}

int asx_http_status_is_redirect(asx_http_status code) {
    return code >= 300u && code < 400u;
}

int asx_http_status_is_client_error(asx_http_status code) {
    return code >= 400u && code < 500u;
}

int asx_http_status_is_server_error(asx_http_status code) {
    return code >= 500u && code < 600u;
}

/* ------------------------------------------------------------------ */
/* HTTP version                                                        */
/* ------------------------------------------------------------------ */

const char *asx_http_version_str(asx_http_version ver) {
    switch (ver) {
    case ASX_HTTP_VERSION_1_0: return "HTTP/1.0";
    case ASX_HTTP_VERSION_1_1: return "HTTP/1.1";
    case ASX_HTTP_VERSION_2:   return "HTTP/2";
    case ASX_HTTP_VERSION_3:   return "HTTP/3";
    }
    return "HTTP/unknown";
}

/* ------------------------------------------------------------------ */
/* HTTP headers                                                        */
/* ------------------------------------------------------------------ */

void asx_http_headers_init(asx_http_headers *hdrs) {
    if (hdrs == NULL) return;
    memset(hdrs, 0, sizeof(*hdrs));
}

asx_status asx_http_headers_add(asx_http_headers *hdrs, const char *name, const char *value) {
    size_t name_len, value_len;
    asx_http_header *h;

    if (hdrs == NULL || name == NULL || value == NULL) return ASX_E_INVALID_ARGUMENT;
    if (hdrs->count >= ASX_HTTP_MAX_HEADERS) return ASX_E_RESOURCE_EXHAUSTED;

    name_len = http_bounded_strlen(name, ASX_HTTP_HEADER_NAME_MAX);
    value_len = http_bounded_strlen(value, ASX_HTTP_HEADER_VALUE_MAX);
    if (name_len == 0u || name_len >= ASX_HTTP_HEADER_NAME_MAX) return ASX_E_INVALID_ARGUMENT;
    if (value_len >= ASX_HTTP_HEADER_VALUE_MAX) return ASX_E_BUFFER_TOO_SMALL;

    h = &hdrs->entries[hdrs->count];
    memcpy(h->name, name, name_len + 1u);
    memcpy(h->value, value, value_len + 1u);
    hdrs->count++;
    return ASX_OK;
}

const char *asx_http_headers_get(const asx_http_headers *hdrs, const char *name) {
    uint32_t i;

    if (hdrs == NULL || name == NULL) return NULL;
    for (i = 0u; i < hdrs->count; i++) {
        if (http_strcasecmp(hdrs->entries[i].name, name) == 0) {
            return hdrs->entries[i].value;
        }
    }
    return NULL;
}

uint32_t asx_http_headers_remove(asx_http_headers *hdrs, const char *name) {
    uint32_t i, removed, write;

    if (hdrs == NULL || name == NULL) return 0u;
    removed = 0u;
    write = 0u;
    for (i = 0u; i < hdrs->count; i++) {
        if (http_strcasecmp(hdrs->entries[i].name, name) == 0) {
            removed++;
        } else {
            if (write != i) hdrs->entries[write] = hdrs->entries[i];
            write++;
        }
    }
    hdrs->count = write;
    return removed;
}

/* ------------------------------------------------------------------ */
/* HTTP body                                                           */
/* ------------------------------------------------------------------ */

void asx_http_body_init(asx_http_body *body) {
    if (body == NULL) return;
    memset(body, 0, sizeof(*body));
    body->kind = ASX_HTTP_BODY_EMPTY;
}

asx_status asx_http_body_set_bytes(asx_http_body *body, const void *data, uint32_t len) {
    if (body == NULL) return ASX_E_INVALID_ARGUMENT;
    if (len > ASX_HTTP_BODY_MAX) return ASX_E_BUFFER_TOO_SMALL;
    body->kind = ASX_HTTP_BODY_BYTES;
    body->len = len;
    body->content_length = len;
    if (len > 0u && data != NULL) memcpy(body->data, data, len);
    return ASX_OK;
}

uint32_t asx_http_body_len(const asx_http_body *body) {
    if (body == NULL) return 0u;
    return body->len;
}

int asx_http_body_is_empty(const asx_http_body *body) {
    if (body == NULL) return 1;
    return body->kind == ASX_HTTP_BODY_EMPTY || body->len == 0u;
}

/* ------------------------------------------------------------------ */
/* HTTP request                                                        */
/* ------------------------------------------------------------------ */

void asx_http_request_init(asx_http_request *req, asx_http_method method, const char *uri) {
    size_t len;

    if (req == NULL) return;
    memset(req, 0, sizeof(*req));
    req->method = method;
    req->version = ASX_HTTP_VERSION_1_1;
    asx_http_headers_init(&req->headers);
    asx_http_body_init(&req->body);
    if (uri != NULL) {
        len = http_bounded_strlen(uri, ASX_HTTP_URI_MAX);
        if (len < ASX_HTTP_URI_MAX) memcpy(req->uri, uri, len + 1u);
    }
}

/* ------------------------------------------------------------------ */
/* HTTP response                                                       */
/* ------------------------------------------------------------------ */

void asx_http_response_init(asx_http_response *resp, asx_http_status status) {
    if (resp == NULL) return;
    memset(resp, 0, sizeof(*resp));
    resp->status = status;
    resp->version = ASX_HTTP_VERSION_1_1;
    asx_http_headers_init(&resp->headers);
    asx_http_body_init(&resp->body);
}

/* ------------------------------------------------------------------ */
/* HTTP connection pool                                                */
/* ------------------------------------------------------------------ */

void asx_http_pool_init(asx_http_pool *pool, uint32_t max_connections) {
    if (pool == NULL) return;
    memset(pool, 0, sizeof(*pool));
    pool->max_connections = max_connections > ASX_HTTP_POOL_MAX_CONNECTIONS
                                ? ASX_HTTP_POOL_MAX_CONNECTIONS
                                : max_connections;
    pool->next_id = 1u;
}

asx_status asx_http_pool_acquire(asx_http_pool *pool, asx_http_pool_conn *out) {
    uint32_t i;

    if (pool == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;

    /* Try to reuse an idle connection */
    for (i = 0u; i < ASX_HTTP_POOL_MAX_CONNECTIONS; i++) {
        if (pool->connections[i].state == ASX_HTTP_POOL_CONN_IDLE &&
            pool->connections[i].id != 0u) {
            pool->connections[i].state = ASX_HTTP_POOL_CONN_ACTIVE;
            pool->active_count++;
            if (pool->idle_count > 0u) pool->idle_count--;
            *out = pool->connections[i];
            return ASX_OK;
        }
    }

    /* Create a new connection */
    if (pool->active_count + pool->idle_count >= pool->max_connections)
        return ASX_E_RESOURCE_EXHAUSTED;

    for (i = 0u; i < ASX_HTTP_POOL_MAX_CONNECTIONS; i++) {
        if (pool->connections[i].id == 0u) {
            pool->connections[i].id = pool->next_id++;
            pool->connections[i].version = ASX_HTTP_VERSION_1_1;
            pool->connections[i].state = ASX_HTTP_POOL_CONN_ACTIVE;
            pool->connections[i].requests_served = 0u;
            pool->active_count++;
            *out = pool->connections[i];
            return ASX_OK;
        }
    }
    return ASX_E_RESOURCE_EXHAUSTED;
}

asx_status asx_http_pool_release(asx_http_pool *pool, uint32_t conn_id) {
    uint32_t i;

    if (pool == NULL) return ASX_E_INVALID_ARGUMENT;
    for (i = 0u; i < ASX_HTTP_POOL_MAX_CONNECTIONS; i++) {
        if (pool->connections[i].id == conn_id &&
            pool->connections[i].state == ASX_HTTP_POOL_CONN_ACTIVE) {
            pool->connections[i].state = ASX_HTTP_POOL_CONN_IDLE;
            pool->connections[i].requests_served++;
            if (pool->active_count > 0u) pool->active_count--;
            pool->idle_count++;
            return ASX_OK;
        }
    }
    return ASX_E_NOT_FOUND;
}

asx_status asx_http_pool_close(asx_http_pool *pool, uint32_t conn_id) {
    uint32_t i;

    if (pool == NULL) return ASX_E_INVALID_ARGUMENT;
    for (i = 0u; i < ASX_HTTP_POOL_MAX_CONNECTIONS; i++) {
        if (pool->connections[i].id == conn_id) {
            if (pool->connections[i].state == ASX_HTTP_POOL_CONN_ACTIVE) {
                if (pool->active_count > 0u) pool->active_count--;
            } else if (pool->connections[i].state == ASX_HTTP_POOL_CONN_IDLE) {
                if (pool->idle_count > 0u) pool->idle_count--;
            }
            memset(&pool->connections[i], 0, sizeof(pool->connections[i]));
            return ASX_OK;
        }
    }
    return ASX_E_NOT_FOUND;
}

uint32_t asx_http_pool_active_count(const asx_http_pool *pool) {
    if (pool == NULL) return 0u;
    return pool->active_count;
}

uint32_t asx_http_pool_idle_count(const asx_http_pool *pool) {
    if (pool == NULL) return 0u;
    return pool->idle_count;
}

void asx_http_pool_reset(asx_http_pool *pool) {
    uint32_t max;

    if (pool == NULL) return;
    max = pool->max_connections;
    memset(pool, 0, sizeof(*pool));
    pool->max_connections = max;
    pool->next_id = 1u;
}
