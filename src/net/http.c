/*
 * http.c — HTTP request/response types, connection pool, and version support
 *
 * Deterministic in-memory HTTP surface. All operations are value-type
 * manipulations with no real network I/O.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/net/http.h>
#include <stdio.h>
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

static asx_status http_copy_str(char *dst, uint32_t dst_size, const char *src) {
    size_t len;

    if (dst == NULL || dst_size == 0u || src == NULL) return ASX_E_INVALID_ARGUMENT;
    len = http_bounded_strlen(src, dst_size);
    if (len >= (size_t)dst_size) return ASX_E_BUFFER_TOO_SMALL;
    memcpy(dst, src, len + 1u);
    return ASX_OK;
}

static uint32_t http_copy_until(char *dst, uint32_t dst_size, const char *src, char stop_a,
                                char stop_b) {
    uint32_t len = 0u;

    if (dst == NULL || dst_size == 0u || src == NULL) return 0u;
    while (src[len] != '\0' && src[len] != stop_a && src[len] != stop_b) {
        if (len + 1u >= dst_size) return 0u;
        dst[len] = src[len];
        len++;
    }
    dst[len] = '\0';
    return len;
}

static const char *http_find_char(const char *text, char needle) {
    if (text == NULL) return NULL;
    while (*text != '\0') {
        if (*text == needle) return text;
        text++;
    }
    return NULL;
}

static int http_starts_with(const char *text, const char *prefix) {
    size_t i;

    if (text == NULL || prefix == NULL) return 0;
    for (i = 0u; prefix[i] != '\0'; i++) {
        if (text[i] != prefix[i]) return 0;
    }
    return 1;
}

static const char *http_skip_slash(const char *text) {
    while (text != NULL && *text == '/') text++;
    return text;
}

static int http_is_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static uint8_t http_hex_value(char c) {
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'a' && c <= 'f') return (uint8_t)(10 + (c - 'a'));
    return (uint8_t)(10 + (c - 'A'));
}

static asx_status http_parse_hex_tag(const char *text, asx_auth_tag *out_tag) {
    uint32_t i;

    if (text == NULL || out_tag == NULL) return ASX_E_INVALID_ARGUMENT;
    if (http_bounded_strlen(text, (ASX_AUTH_TAG_SIZE * 2u) + 1u) != ASX_AUTH_TAG_SIZE * 2u) {
        return ASX_E_INVALID_ARGUMENT;
    }
    for (i = 0u; i < ASX_AUTH_TAG_SIZE; i++) {
        char hi = text[i * 2u];
        char lo = text[(i * 2u) + 1u];
        if (!http_is_hex(hi) || !http_is_hex(lo)) return ASX_E_INVALID_ARGUMENT;
        out_tag->bytes[i] = (uint8_t)((http_hex_value(hi) << 4u) | http_hex_value(lo));
    }
    return ASX_OK;
}

static void http_trim_ws_span(const char **start, const char **end) {
    while (*start < *end &&
           (**start == ' ' || **start == '\t' || **start == '\r' || **start == '\n')) {
        (*start)++;
    }
    while (*end > *start && ((*(*end - 1) == ' ') || (*(*end - 1) == '\t') ||
                             (*(*end - 1) == '\r') || (*(*end - 1) == '\n'))) {
        (*end)--;
    }
}

static asx_status http_copy_span(char *dst, uint32_t dst_size, const char *start, const char *end) {
    size_t len;

    if (dst == NULL || dst_size == 0u || start == NULL || end == NULL || end < start) {
        return ASX_E_INVALID_ARGUMENT;
    }
    len = (size_t)(end - start);
    if (len + 1u > dst_size) return ASX_E_BUFFER_TOO_SMALL;
    if (len > 0u) memcpy(dst, start, len);
    dst[len] = '\0';
    return ASX_OK;
}

static const uint8_t *http_memmem(const uint8_t *haystack, size_t haystack_len, const char *needle,
                                  size_t needle_len) {
    size_t i;

    if (haystack == NULL || needle == NULL) return NULL;
    if (needle_len == 0u) return haystack;
    if (haystack_len < needle_len) return NULL;

    for (i = 0u; i + needle_len <= haystack_len; i++) {
        if (memcmp(haystack + i, needle, needle_len) == 0) return haystack + i;
    }
    return NULL;
}

static const uint8_t *http_memchr_byte(const uint8_t *haystack, size_t haystack_len,
                                       uint8_t needle) {
    size_t i;

    if (haystack == NULL) return NULL;
    for (i = 0u; i < haystack_len; i++) {
        if (haystack[i] == needle) return haystack + i;
    }
    return NULL;
}

static const char *http_content_type_for_path(const char *path) {
    const char *dot;

    if (path == NULL) return "application/octet-stream";
    dot = strrchr(path, '.');
    if (dot == NULL) return "application/octet-stream";
    if (strcmp(dot, ".html") == 0) return "text/html";
    if (strcmp(dot, ".css") == 0) return "text/css";
    if (strcmp(dot, ".js") == 0) return "application/javascript";
    if (strcmp(dot, ".json") == 0) return "application/json";
    if (strcmp(dot, ".txt") == 0) return "text/plain";
    if (strcmp(dot, ".svg") == 0) return "image/svg+xml";
    return "application/octet-stream";
}

static asx_status http_path_params_add(asx_http_path_params *params, const char *name_start,
                                       const char *name_end, const char *value_start,
                                       const char *value_end) {
    asx_http_path_param *entry;
    asx_status st;

    if (params == NULL || name_start == NULL || name_end == NULL || value_start == NULL ||
        value_end == NULL || name_end < name_start || value_end < value_start) {
        return ASX_E_INVALID_ARGUMENT;
    }
    if (params->count >= ASX_HTTP_MAX_PATH_PARAMS) return ASX_E_RESOURCE_EXHAUSTED;
    entry = &params->entries[params->count];
    st = http_copy_span(entry->name, ASX_HTTP_PARAM_NAME_MAX, name_start, name_end);
    if (st != ASX_OK) return st;
    st = http_copy_span(entry->value, ASX_HTTP_PARAM_VALUE_MAX, value_start, value_end);
    if (st != ASX_OK) return st;
    params->count++;
    return ASX_OK;
}

static asx_status http_route_match(const char *pattern, const char *uri,
                                   asx_http_path_params *out_params) {
    char path[ASX_HTTP_URI_MAX];
    const char *pp;
    const char *up;

    if (pattern == NULL || uri == NULL || out_params == NULL) return ASX_E_INVALID_ARGUMENT;
    memset(out_params, 0, sizeof(*out_params));
    if (http_copy_until(path, sizeof(path), uri, '?', '#') == 0u && uri[0] != '\0') {
        return ASX_E_BUFFER_TOO_SMALL;
    }

    pp = http_skip_slash(pattern);
    up = http_skip_slash(path);
    for (;;) {
        const char *pp_end;
        const char *up_end;

        if (*pp == '\0' && *up == '\0') return ASX_OK;
        if (*pp == '\0' || *up == '\0') return ASX_E_NOT_FOUND;

        pp_end = pp;
        while (*pp_end != '\0' && *pp_end != '/') pp_end++;
        up_end = up;
        while (*up_end != '\0' && *up_end != '/') up_end++;

        if (*pp == ':') {
            asx_status st = http_path_params_add(out_params, pp + 1, pp_end, up, up_end);
            if (st != ASX_OK) return st;
        } else {
            size_t plen = (size_t)(pp_end - pp);
            size_t ulen = (size_t)(up_end - up);
            if (plen != ulen || memcmp(pp, up, plen) != 0) return ASX_E_NOT_FOUND;
        }

        if (*pp_end == '\0' && *up_end == '\0') return ASX_OK;
        if (*pp_end == '\0' || *up_end == '\0') return ASX_E_NOT_FOUND;
        pp = pp_end + 1;
        up = up_end + 1;
    }
}

/* ------------------------------------------------------------------ */
/* HTTP method                                                         */
/* ------------------------------------------------------------------ */

static const char *const g_method_names[] = {"GET",  "POST",    "PUT",     "DELETE", "PATCH",
                                             "HEAD", "OPTIONS", "CONNECT", "TRACE"};

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

int asx_http_status_is_info(asx_http_status code) { return code >= 100u && code < 200u; }

int asx_http_status_is_success(asx_http_status code) { return code >= 200u && code < 300u; }

int asx_http_status_is_redirect(asx_http_status code) { return code >= 300u && code < 400u; }

int asx_http_status_is_client_error(asx_http_status code) { return code >= 400u && code < 500u; }

int asx_http_status_is_server_error(asx_http_status code) { return code >= 500u && code < 600u; }

/* ------------------------------------------------------------------ */
/* HTTP version                                                        */
/* ------------------------------------------------------------------ */

const char *asx_http_version_str(asx_http_version ver) {
    switch (ver) {
    case ASX_HTTP_VERSION_1_0: return "HTTP/1.0";
    case ASX_HTTP_VERSION_1_1: return "HTTP/1.1";
    case ASX_HTTP_VERSION_2: return "HTTP/2";
    case ASX_HTTP_VERSION_3: return "HTTP/3";
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
        if (http_strcasecmp(hdrs->entries[i].name, name) == 0) { return hdrs->entries[i].value; }
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
    if (len > 0u && data == NULL) return ASX_E_INVALID_ARGUMENT;
    memset(body->data, 0, sizeof(body->data));
    body->kind = ASX_HTTP_BODY_BYTES;
    body->len = len;
    body->content_length = len;
    if (len > 0u) memcpy(body->data, data, len);
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
/* Web router and helpers                                              */
/* ------------------------------------------------------------------ */

void asx_http_route_policy_init(asx_http_route_policy *policy) {
    if (policy == NULL) return;
    memset(policy, 0, sizeof(*policy));
}

void asx_http_router_init(asx_http_router *router) {
    if (router == NULL) return;
    memset(router, 0, sizeof(*router));
}

asx_status asx_http_router_add_route(asx_http_router *router, asx_http_method method,
                                     const char *pattern, asx_http_handler_fn handler,
                                     void *handler_user_data, const asx_http_route_policy *policy,
                                     asx_http_route **out_route) {
    asx_http_route *route;
    asx_status st;

    if (out_route != NULL) *out_route = NULL;
    if (router == NULL || pattern == NULL || handler == NULL) return ASX_E_INVALID_ARGUMENT;
    if (router->count >= ASX_HTTP_MAX_ROUTES) return ASX_E_RESOURCE_EXHAUSTED;

    route = &router->routes[router->count];
    memset(route, 0, sizeof(*route));
    route->method = method;
    route->handler = handler;
    route->handler_user_data = handler_user_data;
    st = http_copy_str(route->pattern, ASX_HTTP_URI_MAX, pattern);
    if (st != ASX_OK) return st;
    if (policy != NULL) route->policy = *policy;
    router->count++;
    if (out_route != NULL) *out_route = route;
    return ASX_OK;
}

asx_status asx_http_route_add_middleware(asx_http_route *route, asx_http_middleware_fn fn,
                                         void *user_data) {
    if (route == NULL || fn == NULL) return ASX_E_INVALID_ARGUMENT;
    if (route->middleware_count >= ASX_HTTP_MAX_ROUTE_MIDDLEWARE) {
        return ASX_E_RESOURCE_EXHAUSTED;
    }
    route->middleware[route->middleware_count].fn = fn;
    route->middleware[route->middleware_count].user_data = user_data;
    route->middleware_count++;
    return ASX_OK;
}

asx_status asx_http_request_path_param(const asx_http_request_context *ctx, const char *name,
                                       char *out, uint32_t out_size) {
    uint32_t i;

    if (ctx == NULL || name == NULL || out == NULL || out_size == 0u) {
        return ASX_E_INVALID_ARGUMENT;
    }
    for (i = 0u; i < ctx->path_params.count; i++) {
        if (strcmp(ctx->path_params.entries[i].name, name) == 0) {
            return http_copy_str(out, out_size, ctx->path_params.entries[i].value);
        }
    }
    return ASX_E_NOT_FOUND;
}

asx_status asx_http_request_query_param(const asx_http_request *req, const char *name, char *out,
                                        uint32_t out_size) {
    const char *query;
    size_t name_len;

    if (req == NULL || name == NULL || out == NULL || out_size == 0u) {
        return ASX_E_INVALID_ARGUMENT;
    }
    query = http_find_char(req->uri, '?');
    if (query == NULL) return ASX_E_NOT_FOUND;
    query++;
    name_len = strlen(name);

    while (*query != '\0' && *query != '#') {
        const char *pair_end = query;
        const char *eq = NULL;
        while (*pair_end != '\0' && *pair_end != '&' && *pair_end != '#') {
            if (*pair_end == '=' && eq == NULL) eq = pair_end;
            pair_end++;
        }
        if (eq != NULL && (size_t)(eq - query) == name_len && memcmp(query, name, name_len) == 0) {
            return http_copy_span(out, out_size, eq + 1, pair_end);
        }
        query = (*pair_end == '&') ? pair_end + 1 : pair_end;
    }
    return ASX_E_NOT_FOUND;
}

asx_status asx_http_request_cookie(const asx_http_request *req, const char *name, char *out,
                                   uint32_t out_size) {
    const char *cookie;
    size_t name_len;

    if (req == NULL || name == NULL || out == NULL || out_size == 0u) {
        return ASX_E_INVALID_ARGUMENT;
    }
    cookie = asx_http_headers_get(&req->headers, "Cookie");
    if (cookie == NULL) return ASX_E_NOT_FOUND;
    name_len = strlen(name);

    while (*cookie != '\0') {
        const char *entry = cookie;
        const char *eq;
        const char *end;

        while (*entry == ' ' || *entry == ';') entry++;
        eq = entry;
        while (*eq != '\0' && *eq != '=' && *eq != ';') eq++;
        end = eq;
        while (*end != '\0' && *end != ';') end++;

        if (*eq == '=' && (size_t)(eq - entry) == name_len && memcmp(entry, name, name_len) == 0) {
            return http_copy_span(out, out_size, eq + 1, end);
        }
        cookie = (*end == ';') ? end + 1 : end;
    }
    return ASX_E_NOT_FOUND;
}

asx_status asx_http_request_verify_body_auth(const asx_http_request *req,
                                             asx_security_context *security, const char *purpose,
                                             int *out_verified) {
    const char *header;
    asx_auth_tag tag;
    asx_security_context derived;
    asx_security_context *ctx;
    asx_status st;
    int verified = 0;

    if (out_verified != NULL) *out_verified = 0;
    if (req == NULL || security == NULL) return ASX_E_INVALID_ARGUMENT;

    header = asx_http_headers_get(&req->headers, "X-ASX-Auth");
    if (header == NULL) return ASX_E_PERMISSION_DENIED;
    st = http_parse_hex_tag(header, &tag);
    if (st != ASX_OK) return ASX_E_PERMISSION_DENIED;

    ctx = security;
    if (purpose != NULL && purpose[0] != '\0') {
        asx_security_context_derive(&derived, security, (const uint8_t *)purpose, strlen(purpose));
        ctx = &derived;
    }

    st = asx_security_context_verify(ctx, req->body.data, req->body.len, &tag, &verified);
    if (st != ASX_OK || !verified) return ASX_E_PERMISSION_DENIED;
    if (out_verified != NULL) *out_verified = 1;
    return ASX_OK;
}

asx_status asx_http_response_set_session_cookie(asx_http_response *resp, const char *name,
                                                const char *value, int secure, int http_only) {
    char cookie[ASX_HTTP_HEADER_VALUE_MAX];
    int written;

    if (resp == NULL || name == NULL || value == NULL) return ASX_E_INVALID_ARGUMENT;
    written = snprintf(cookie, sizeof(cookie), "%s=%s; Path=/%s%s", name, value,
                       http_only ? "; HttpOnly" : "", secure ? "; Secure" : "");
    if (written < 0 || (size_t)written >= sizeof(cookie)) return ASX_E_BUFFER_TOO_SMALL;
    return asx_http_headers_add(&resp->headers, "Set-Cookie", cookie);
}

asx_status asx_http_response_set_sse(asx_http_response *resp, const char *event, const char *id,
                                     const char *data) {
    char payload[ASX_HTTP_BODY_MAX];
    int written;
    asx_status st;

    if (resp == NULL || data == NULL) return ASX_E_INVALID_ARGUMENT;
    written =
        snprintf(payload, sizeof(payload), "%s%s%s%sdata: %s\n\n",
                 (id != NULL && id[0] != '\0') ? "id: " : "",
                 (id != NULL && id[0] != '\0') ? id : "", (id != NULL && id[0] != '\0') ? "\n" : "",
                 (event != NULL && event[0] != '\0') ? "" : "", data);
    if (event != NULL && event[0] != '\0') {
        written = snprintf(payload, sizeof(payload), "%s%s%s%s%sdata: %s\n\n",
                           (id != NULL && id[0] != '\0') ? "id: " : "",
                           (id != NULL && id[0] != '\0') ? id : "",
                           (id != NULL && id[0] != '\0') ? "\n" : "", "event: ", event, data);
    }
    if (written < 0 || (size_t)written >= sizeof(payload)) return ASX_E_BUFFER_TOO_SMALL;

    asx_http_response_init(resp, ASX_HTTP_200_OK);
    st = asx_http_headers_add(&resp->headers, "Content-Type", "text/event-stream");
    if (st != ASX_OK) return st;
    st = asx_http_headers_add(&resp->headers, "Cache-Control", "no-cache");
    if (st != ASX_OK) return st;
    return asx_http_body_set_bytes(&resp->body, payload, (uint32_t)written);
}

asx_status asx_http_serve_static(asx_http_response *resp, const char *root, const char *uri) {
    char path_only[ASX_HTTP_URI_MAX];
    char full_path[ASX_FS_PATH_MAX];
    asx_fs_path fs_path;
    asx_file_handle file;
    asx_buf_mut dst;
    uint32_t bytes_read = 0u;
    asx_status st;
    asx_status close_st;

    if (resp == NULL || root == NULL || uri == NULL) return ASX_E_INVALID_ARGUMENT;
    if (http_copy_until(path_only, sizeof(path_only), uri, '?', '#') == 0u && uri[0] != '\0') {
        return ASX_E_BUFFER_TOO_SMALL;
    }
    if (strstr(path_only, "..") != NULL) {
        asx_http_response_init(resp, ASX_HTTP_403_FORBIDDEN);
        return ASX_E_PERMISSION_DENIED;
    }
    if (path_only[0] == '\0') { http_copy_str(path_only, sizeof(path_only), "/"); }
    if (strcmp(path_only, "/") == 0) {
        st = http_copy_str(path_only, sizeof(path_only), "/index.html");
        if (st != ASX_OK) return st;
    }

    if (snprintf(full_path, sizeof(full_path), "%s%s", root, path_only) >= (int)sizeof(full_path)) {
        return ASX_E_BUFFER_TOO_SMALL;
    }
    st = asx_fs_path_from_cstr(&fs_path, full_path);
    if (st != ASX_OK) return st;
    st = asx_fs_file_open(&file, &fs_path, ASX_FS_OPEN_READ);
    if (st != ASX_OK) {
        asx_http_response_init(resp, ASX_HTTP_404_NOT_FOUND);
        return st;
    }

    asx_buf_mut_init(&dst);
    st = asx_fs_file_poll_read(file, &dst, &bytes_read);
    close_st = asx_fs_file_close(file);
    if (st != ASX_OK) {
        asx_http_response_init(resp, ASX_HTTP_404_NOT_FOUND);
        return st;
    }
    if (close_st != ASX_OK) return close_st;

    asx_http_response_init(resp, ASX_HTTP_200_OK);
    st =
        asx_http_headers_add(&resp->headers, "Content-Type", http_content_type_for_path(path_only));
    if (st != ASX_OK) return st;
    return asx_http_body_set_bytes(&resp->body, asx_buf_mut_freeze(&dst).ptr, bytes_read);
}

asx_status asx_http_parse_multipart(const asx_http_request *req,
                                    asx_http_multipart_form *out_form) {
    const char *content_type;
    const char *boundary_key = "boundary=";
    const char *boundary_pos;
    char boundary[ASX_HTTP_HEADER_VALUE_MAX];
    char marker[ASX_HTTP_HEADER_VALUE_MAX];
    const uint8_t *cursor;
    const uint8_t *body;
    const uint8_t *body_end;
    size_t marker_len;
    size_t boundary_len;

    if (req == NULL || out_form == NULL) return ASX_E_INVALID_ARGUMENT;
    memset(out_form, 0, sizeof(*out_form));
    content_type = asx_http_headers_get(&req->headers, "Content-Type");
    if (content_type == NULL || !http_starts_with(content_type, "multipart/form-data")) {
        return ASX_E_INVALID_ARGUMENT;
    }
    boundary_pos = strstr(content_type, boundary_key);
    if (boundary_pos == NULL) return ASX_E_INVALID_ARGUMENT;
    boundary_len = http_copy_until(boundary, (uint32_t)sizeof(boundary),
                                   boundary_pos + strlen(boundary_key), ';', '\r');
    if (boundary_len == 0u || boundary_len >= sizeof(boundary)) { return ASX_E_BUFFER_TOO_SMALL; }
    if (snprintf(marker, sizeof(marker), "--%s", boundary) >= (int)sizeof(marker)) {
        return ASX_E_BUFFER_TOO_SMALL;
    }
    marker_len = strlen(marker);

    body = req->body.data;
    body_end = body + req->body.len;
    cursor = body;

    while (cursor < body_end) {
        const uint8_t *part_start =
            http_memmem(cursor, (size_t)(body_end - cursor), marker, marker_len);
        const uint8_t *header_start;
        const uint8_t *content_start;
        const uint8_t *content_scan_end;
        const uint8_t *part_end;
        asx_http_multipart_part *part;
        const uint8_t *disposition;
        const uint8_t *name_pos;
        const uint8_t *filename_pos;
        const uint8_t *type_pos;

        if (part_start == NULL) break;
        part_start += marker_len;
        if ((size_t)(body_end - part_start) >= 2u && part_start[0] == '-' && part_start[1] == '-') {
            break;
        }
        if ((size_t)(body_end - part_start) >= 2u && part_start[0] == '\r' &&
            part_start[1] == '\n') {
            part_start += 2;
        }
        header_start = part_start;
        content_start =
            http_memmem(header_start, (size_t)(body_end - header_start), "\r\n\r\n", 4u);
        if (content_start == NULL) return ASX_E_INVALID_ARGUMENT;
        content_start += 4;
        part_end =
            http_memmem(content_start, (size_t)(body_end - content_start), marker, marker_len);
        if (part_end == NULL) return ASX_E_INVALID_ARGUMENT;
        if (out_form->count >= ASX_HTTP_MULTIPART_PARTS_MAX) return ASX_E_RESOURCE_EXHAUSTED;

        part = &out_form->parts[out_form->count];
        memset(part, 0, sizeof(*part));
        content_scan_end = content_start - 4;
        disposition = http_memmem(header_start, (size_t)(content_scan_end - header_start),
                                  "Content-Disposition:", 20u);
        if (disposition == NULL) return ASX_E_INVALID_ARGUMENT;
        name_pos =
            http_memmem(disposition, (size_t)(content_scan_end - disposition), "name=\"", 6u);
        if (name_pos == NULL) return ASX_E_INVALID_ARGUMENT;
        name_pos += 6;
        {
            const uint8_t *name_end =
                http_memchr_byte(name_pos, (size_t)(content_scan_end - name_pos), '"');
            if (name_end == NULL) return ASX_E_INVALID_ARGUMENT;
            if (http_copy_span(part->name, sizeof(part->name), (const char *)name_pos,
                               (const char *)name_end) != ASX_OK) {
                return ASX_E_BUFFER_TOO_SMALL;
            }
        }
        filename_pos =
            http_memmem(disposition, (size_t)(content_scan_end - disposition), "filename=\"", 10u);
        if (filename_pos != NULL) {
            const uint8_t *filename_end;
            filename_pos += 10;
            filename_end =
                http_memchr_byte(filename_pos, (size_t)(content_scan_end - filename_pos), '"');
            if (filename_end == NULL) return ASX_E_INVALID_ARGUMENT;
            if (http_copy_span(part->filename, sizeof(part->filename), (const char *)filename_pos,
                               (const char *)filename_end) != ASX_OK) {
                return ASX_E_BUFFER_TOO_SMALL;
            }
        }
        type_pos = http_memmem(header_start, (size_t)(content_scan_end - header_start),
                               "Content-Type:", 13u);
        if (type_pos != NULL) {
            const uint8_t *line_end =
                http_memmem(type_pos, (size_t)(content_scan_end - type_pos), "\r\n", 2u);
            const char *value_start = (const char *)(type_pos + 13u);
            const char *value_end =
                (line_end != NULL) ? (const char *)line_end : (const char *)content_scan_end;
            http_trim_ws_span(&value_start, &value_end);
            if (http_copy_span(part->content_type, sizeof(part->content_type), value_start,
                               value_end) != ASX_OK) {
                return ASX_E_BUFFER_TOO_SMALL;
            }
        }

        while (part_end > content_start && (part_end[-1] == '\n' || part_end[-1] == '\r')) {
            part_end--;
        }
        if ((size_t)(part_end - content_start) > ASX_HTTP_MULTIPART_DATA_MAX) {
            return ASX_E_BUFFER_TOO_SMALL;
        }
        part->data_len = (uint32_t)(part_end - content_start);
        if (part->data_len > 0u) memcpy(part->data, content_start, part->data_len);
        out_form->count++;
        cursor = part_end;
    }

    return out_form->count > 0u ? ASX_OK : ASX_E_NOT_FOUND;
}

asx_status asx_http_router_dispatch(asx_http_router *router, const asx_http_request *req,
                                    asx_http_response *resp, asx_session_pair *session,
                                    asx_security_context *security,
                                    asx_http_request_context *out_ctx) {
    uint32_t i;

    if (router == NULL || req == NULL || resp == NULL) return ASX_E_INVALID_ARGUMENT;
    for (i = 0u; i < router->count; i++) {
        asx_http_route *route = &router->routes[i];
        asx_http_request_context ctx;
        asx_status st;
        uint32_t j;

        if (route->method != req->method) continue;
        memset(&ctx, 0, sizeof(ctx));
        ctx.request = req;
        ctx.session = session;
        ctx.security = security;
        st = http_copy_str(ctx.route_pattern, sizeof(ctx.route_pattern), route->pattern);
        if (st != ASX_OK) return st;
        st = http_route_match(route->pattern, req->uri, &ctx.path_params);
        if (st != ASX_OK) continue;

        if (route->policy.require_session && session == NULL) {
            asx_http_response_init(resp, ASX_HTTP_401_UNAUTHORIZED);
            return ASX_OK;
        }
        if (route->policy.require_security) {
            int verified = 0;
            st = asx_http_request_verify_body_auth(req, security, route->policy.security_purpose,
                                                   &verified);
            if (st != ASX_OK || !verified) {
                asx_http_response_init(resp, ASX_HTTP_401_UNAUTHORIZED);
                return ASX_OK;
            }
            ctx.security_verified = 1u;
        }

        for (j = 0u; j < route->middleware_count; j++) {
            asx_http_middleware_result result = ASX_HTTP_MIDDLEWARE_CONTINUE;
            st = route->middleware[j].fn(&ctx, resp, route->middleware[j].user_data, &result);
            if (st != ASX_OK) return st;
            if (result == ASX_HTTP_MIDDLEWARE_RESPOND) {
                if (out_ctx != NULL) *out_ctx = ctx;
                return ASX_OK;
            }
        }

        st = route->handler(&ctx, resp, route->handler_user_data);
        if (st != ASX_OK) return st;
        if (out_ctx != NULL) *out_ctx = ctx;
        return ASX_OK;
    }

    asx_http_response_init(resp, ASX_HTTP_404_NOT_FOUND);
    return ASX_E_NOT_FOUND;
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
