/*
 * http.c — HTTP types, router, and the RFC 9112 HTTP/1.1 wire protocol
 *
 * Value-type request/response helpers and the router are pure in-memory
 * manipulations. The wire protocol layer adds a sans-IO incremental parser,
 * a serializer, and keep-alive connection state machines that drive the
 * asx_tcp_stream poll API. Every buffer is caller-owned and fixed-size.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/fs/fs.h>
#include <asx/net/http.h>
#include <asx/runtime/browser_boundary.h>
#include <asx/runtime/runtime.h>
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

static char http_ascii_lower(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c + ('a' - 'A'));
    return c;
}

/* Case-insensitive comparison of a (start, len) span against a NUL-terminated literal. */
static int http_span_ieq(const char *span, size_t len, const char *lit) {
    size_t i;

    if (span == NULL || lit == NULL) return 0;
    for (i = 0u; i < len; i++) {
        if (lit[i] == '\0') return 0;
        if (http_ascii_lower(span[i]) != http_ascii_lower(lit[i])) return 0;
    }
    return lit[len] == '\0';
}

/* Iterate the non-empty, OWS-trimmed members of a comma-separated list
 * (RFC 9110 section 5.6.1). Returns 1 with the member span, 0 at the end. */
static int http_list_next(const char **cursor, const char **tok, size_t *tok_len) {
    const char *p;

    if (cursor == NULL || tok == NULL || tok_len == NULL) return 0;
    p = *cursor;
    while (p != NULL && *p != '\0') {
        const char *start = p;
        const char *end;

        while (*p != '\0' && *p != ',') p++;
        end = p;
        if (*p == ',') p++;
        while (start < end && (*start == ' ' || *start == '\t')) start++;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
        if (end > start) {
            *tok = start;
            *tok_len = (size_t)(end - start);
            *cursor = p;
            return 1;
        }
    }
    *cursor = p;
    return 0;
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

#if ASX_HAS_NATIVE_RUNTIME_SURFACES
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
#endif

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

/* RFC 9110 section 15 registered reason phrases; NULL when unregistered. */
static const char *http_reason_or_null(asx_http_status code) {
    switch (code) {
    case 100: return "Continue";
    case 101: return "Switching Protocols";
    case 103: return "Early Hints";
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 203: return "Non-Authoritative Information";
    case 204: return "No Content";
    case 205: return "Reset Content";
    case 206: return "Partial Content";
    case 300: return "Multiple Choices";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 304: return "Not Modified";
    case 305: return "Use Proxy";
    case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 402: return "Payment Required";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";
    case 407: return "Proxy Authentication Required";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 410: return "Gone";
    case 411: return "Length Required";
    case 412: return "Precondition Failed";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 415: return "Unsupported Media Type";
    case 416: return "Range Not Satisfiable";
    case 417: return "Expectation Failed";
    case 421: return "Misdirected Request";
    case 422: return "Unprocessable Content";
    case 426: return "Upgrade Required";
    case 428: return "Precondition Required";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 451: return "Unavailable For Legal Reasons";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    case 511: return "Network Authentication Required";
    default: return NULL;
    }
}

const char *asx_http_status_reason(asx_http_status code) {
    const char *reason = http_reason_or_null(code);
    return reason != NULL ? reason : "Unknown";
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

int asx_http_header_has_token(const char *value, const char *token) {
    const char *cursor = value;
    const char *tok;
    size_t tok_len;

    if (value == NULL || token == NULL) return 0;
    while (http_list_next(&cursor, &tok, &tok_len)) {
        if (http_span_ieq(tok, tok_len, token)) return 1;
    }
    return 0;
}

int asx_http_headers_has_token(const asx_http_headers *hdrs, const char *name, const char *token) {
    uint32_t i;

    if (hdrs == NULL || name == NULL || token == NULL) return 0;
    for (i = 0u; i < hdrs->count && i < ASX_HTTP_MAX_HEADERS; i++) {
        if (http_strcasecmp(hdrs->entries[i].name, name) == 0 &&
            asx_http_header_has_token(hdrs->entries[i].value, token)) {
            return 1;
        }
    }
    return 0;
}

int asx_http_headers_keep_alive(const asx_http_headers *hdrs, asx_http_version version) {
    if (asx_http_headers_has_token(hdrs, "Connection", "close")) return 0;
    if (version == ASX_HTTP_VERSION_1_0) {
        return asx_http_headers_has_token(hdrs, "Connection", "keep-alive");
    }
    return version == ASX_HTTP_VERSION_1_1;
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

asx_status asx_http_body_append(asx_http_body *body, const void *data, uint32_t len) {
    if (body == NULL) return ASX_E_INVALID_ARGUMENT;
    if (len > 0u && data == NULL) return ASX_E_INVALID_ARGUMENT;
    if (body->len > ASX_HTTP_BODY_MAX) return ASX_E_INVALID_STATE;
    if (len > ASX_HTTP_BODY_MAX - body->len) return ASX_E_BUFFER_TOO_SMALL;
    if (len == 0u) return ASX_OK;
    memcpy(body->data + body->len, data, len);
    body->len += len;
    body->content_length = body->len;
    if (body->kind == ASX_HTTP_BODY_EMPTY) body->kind = ASX_HTTP_BODY_BYTES;
    return ASX_OK;
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

#if ASX_HAS_NATIVE_RUNTIME_SURFACES
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
#endif

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

/* ================================================================== */
/* HTTP/1.1 wire protocol (RFC 9112)                                   */
/* ================================================================== */

/* ------------------------------------------------------------------ */
/* Character classes                                                   */
/* ------------------------------------------------------------------ */

/* tchar (RFC 9110 section 5.6.2). */
static int http_is_tchar(uint8_t c) {
    if (c >= '0' && c <= '9') return 1;
    if (c >= 'a' && c <= 'z') return 1;
    if (c >= 'A' && c <= 'Z') return 1;
    switch (c) {
    case '!':
    case '#':
    case '$':
    case '%':
    case '&':
    case '\'':
    case '*':
    case '+':
    case '-':
    case '.':
    case '^':
    case '_':
    case '`':
    case '|':
    case '~': return 1;
    default: return 0;
    }
}

/* field-vchar / SP / HTAB / obs-text: everything but CTLs (other than HTAB) and DEL. */
static int http_is_field_char(uint8_t c) { return c == '\t' || (c >= 0x20u && c != 0x7fu); }

/* request-target bytes: visible ASCII only (no SP, CTL, DEL, or non-ASCII). */
static int http_is_target_char(uint8_t c) { return c >= 0x21u && c <= 0x7eu; }

static int http_is_ows(uint8_t c) { return c == ' ' || c == '\t'; }

static int http_is_digit(uint8_t c) { return c >= '0' && c <= '9'; }

static int http_hex_digit(uint8_t c, uint8_t *out) {
    if (c >= '0' && c <= '9') {
        *out = (uint8_t)(c - '0');
        return 1;
    }
    if (c >= 'a' && c <= 'f') {
        *out = (uint8_t)(10u + (uint8_t)(c - 'a'));
        return 1;
    }
    if (c >= 'A' && c <= 'F') {
        *out = (uint8_t)(10u + (uint8_t)(c - 'A'));
        return 1;
    }
    return 0;
}

static int http_cstr_ieq(const char *a, const char *b) { return http_strcasecmp(a, b) == 0; }

/* Fields RFC 9110 section 6.5.1 forbids in a trailer section (framing,
 * routing, request modifiers, authentication, payload processing, caching). */
static int http_is_forbidden_trailer(const char *name) {
    static const char *const forbidden[] = {"age",
                                            "authorization",
                                            "cache-control",
                                            "connection",
                                            "content-encoding",
                                            "content-length",
                                            "content-range",
                                            "content-type",
                                            "cookie",
                                            "date",
                                            "expect",
                                            "expires",
                                            "host",
                                            "keep-alive",
                                            "max-forwards",
                                            "pragma",
                                            "proxy-authenticate",
                                            "proxy-authorization",
                                            "proxy-connection",
                                            "range",
                                            "retry-after",
                                            "set-cookie",
                                            "te",
                                            "trailer",
                                            "transfer-encoding",
                                            "upgrade",
                                            "vary",
                                            "warning",
                                            "www-authenticate"};
    size_t i;

    for (i = 0u; i < sizeof(forbidden) / sizeof(forbidden[0]); i++) {
        if (http_cstr_ieq(name, forbidden[i])) return 1;
    }
    return 0;
}

static int http_method_from_token(const uint8_t *tok, uint32_t len, asx_http_method *out) {
    uint32_t i;

    for (i = 0u; i < (uint32_t)(sizeof(g_method_names) / sizeof(g_method_names[0])); i++) {
        size_t name_len = strlen(g_method_names[i]);
        if (name_len == (size_t)len && memcmp(tok, g_method_names[i], name_len) == 0) {
            *out = (asx_http_method)i;
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Limits and error taxonomy                                           */
/* ------------------------------------------------------------------ */

void asx_http_limits_init(asx_http_limits *limits) {
    if (limits == NULL) return;
    memset(limits, 0, sizeof(*limits));
    limits->max_start_line = ASX_HTTP_DEFAULT_MAX_START_LINE;
    limits->max_header_count = ASX_HTTP_PARSER_MAX_FIELDS;
    limits->max_header_bytes = ASX_HTTP_DEFAULT_MAX_HEADER_BYTES;
    limits->max_chunk_line = ASX_HTTP_DEFAULT_MAX_CHUNK_LINE;
    limits->max_body = ASX_HTTP_DEFAULT_MAX_BODY;
    limits->require_host = 1u;
}

asx_http_status asx_http_parse_error_status(asx_http_parse_error err) {
    switch (err) {
    case ASX_HTTP_PERR_NONE: return 0u;
    case ASX_HTTP_PERR_START_LINE_TOO_LONG: return ASX_HTTP_414_URI_TOO_LONG;
    case ASX_HTTP_PERR_TOO_MANY_HEADERS:
    case ASX_HTTP_PERR_HEADERS_TOO_LARGE: return ASX_HTTP_431_HEADER_FIELDS_TOO_LARGE;
    case ASX_HTTP_PERR_BODY_TOO_LARGE: return ASX_HTTP_413_CONTENT_TOO_LARGE;
    case ASX_HTTP_PERR_UNKNOWN_METHOD:
    case ASX_HTTP_PERR_UNSUPPORTED_TRANSFER_CODING: return ASX_HTTP_501_NOT_IMPLEMENTED;
    case ASX_HTTP_PERR_UNSUPPORTED_VERSION: return ASX_HTTP_505_VERSION_NOT_SUPPORTED;
    case ASX_HTTP_PERR_BAD_START_LINE:
    case ASX_HTTP_PERR_BAD_TARGET:
    case ASX_HTTP_PERR_BAD_VERSION:
    case ASX_HTTP_PERR_BAD_STATUS:
    case ASX_HTTP_PERR_BAD_LINE_ENDING:
    case ASX_HTTP_PERR_OBS_FOLD:
    case ASX_HTTP_PERR_BAD_HEADER_NAME:
    case ASX_HTTP_PERR_BAD_HEADER_VALUE:
    case ASX_HTTP_PERR_MISSING_HOST:
    case ASX_HTTP_PERR_DUPLICATE_HOST:
    case ASX_HTTP_PERR_BAD_CONTENT_LENGTH:
    case ASX_HTTP_PERR_DUPLICATE_CONTENT_LENGTH:
    case ASX_HTTP_PERR_CONTENT_LENGTH_AND_CHUNKED:
    case ASX_HTTP_PERR_BAD_TRANSFER_ENCODING:
    case ASX_HTTP_PERR_BAD_CHUNK_SIZE:
    case ASX_HTTP_PERR_CHUNK_SIZE_OVERFLOW:
    case ASX_HTTP_PERR_BAD_CHUNK_EXTENSION:
    case ASX_HTTP_PERR_CHUNK_LINE_TOO_LONG:
    case ASX_HTTP_PERR_BAD_CHUNK_TERMINATOR:
    case ASX_HTTP_PERR_BAD_TRAILER:
    case ASX_HTTP_PERR_UNEXPECTED_EOF: return ASX_HTTP_400_BAD_REQUEST;
    }
    return ASX_HTTP_400_BAD_REQUEST;
}

const char *asx_http_parse_error_str(asx_http_parse_error err) {
    switch (err) {
    case ASX_HTTP_PERR_NONE: return "none";
    case ASX_HTTP_PERR_BAD_START_LINE: return "bad-start-line";
    case ASX_HTTP_PERR_UNKNOWN_METHOD: return "unknown-method";
    case ASX_HTTP_PERR_BAD_TARGET: return "bad-target";
    case ASX_HTTP_PERR_BAD_VERSION: return "bad-version";
    case ASX_HTTP_PERR_UNSUPPORTED_VERSION: return "unsupported-version";
    case ASX_HTTP_PERR_BAD_STATUS: return "bad-status";
    case ASX_HTTP_PERR_START_LINE_TOO_LONG: return "start-line-too-long";
    case ASX_HTTP_PERR_BAD_LINE_ENDING: return "bad-line-ending";
    case ASX_HTTP_PERR_OBS_FOLD: return "obs-fold";
    case ASX_HTTP_PERR_BAD_HEADER_NAME: return "bad-header-name";
    case ASX_HTTP_PERR_BAD_HEADER_VALUE: return "bad-header-value";
    case ASX_HTTP_PERR_TOO_MANY_HEADERS: return "too-many-headers";
    case ASX_HTTP_PERR_HEADERS_TOO_LARGE: return "headers-too-large";
    case ASX_HTTP_PERR_MISSING_HOST: return "missing-host";
    case ASX_HTTP_PERR_DUPLICATE_HOST: return "duplicate-host";
    case ASX_HTTP_PERR_BAD_CONTENT_LENGTH: return "bad-content-length";
    case ASX_HTTP_PERR_DUPLICATE_CONTENT_LENGTH: return "duplicate-content-length";
    case ASX_HTTP_PERR_CONTENT_LENGTH_AND_CHUNKED: return "content-length-and-transfer-encoding";
    case ASX_HTTP_PERR_BAD_TRANSFER_ENCODING: return "bad-transfer-encoding";
    case ASX_HTTP_PERR_UNSUPPORTED_TRANSFER_CODING: return "unsupported-transfer-coding";
    case ASX_HTTP_PERR_BAD_CHUNK_SIZE: return "bad-chunk-size";
    case ASX_HTTP_PERR_CHUNK_SIZE_OVERFLOW: return "chunk-size-overflow";
    case ASX_HTTP_PERR_BAD_CHUNK_EXTENSION: return "bad-chunk-extension";
    case ASX_HTTP_PERR_CHUNK_LINE_TOO_LONG: return "chunk-line-too-long";
    case ASX_HTTP_PERR_BAD_CHUNK_TERMINATOR: return "bad-chunk-terminator";
    case ASX_HTTP_PERR_BAD_TRAILER: return "bad-trailer";
    case ASX_HTTP_PERR_BODY_TOO_LARGE: return "body-too-large";
    case ASX_HTTP_PERR_UNEXPECTED_EOF: return "unexpected-eof";
    }
    return "unknown";
}

/* asx_status surfaced for a parse error. */
static asx_status http_perr_status(asx_http_parse_error err) {
    switch (err) {
    case ASX_HTTP_PERR_NONE: return ASX_OK;
    case ASX_HTTP_PERR_START_LINE_TOO_LONG:
    case ASX_HTTP_PERR_TOO_MANY_HEADERS:
    case ASX_HTTP_PERR_HEADERS_TOO_LARGE:
    case ASX_HTTP_PERR_CHUNK_LINE_TOO_LONG:
    case ASX_HTTP_PERR_BODY_TOO_LARGE: return ASX_E_RESOURCE_EXHAUSTED;
    case ASX_HTTP_PERR_UNEXPECTED_EOF: return ASX_E_DISCONNECTED;
    case ASX_HTTP_PERR_BAD_START_LINE:
    case ASX_HTTP_PERR_UNKNOWN_METHOD:
    case ASX_HTTP_PERR_BAD_TARGET:
    case ASX_HTTP_PERR_BAD_VERSION:
    case ASX_HTTP_PERR_UNSUPPORTED_VERSION:
    case ASX_HTTP_PERR_BAD_STATUS:
    case ASX_HTTP_PERR_BAD_LINE_ENDING:
    case ASX_HTTP_PERR_OBS_FOLD:
    case ASX_HTTP_PERR_BAD_HEADER_NAME:
    case ASX_HTTP_PERR_BAD_HEADER_VALUE:
    case ASX_HTTP_PERR_MISSING_HOST:
    case ASX_HTTP_PERR_DUPLICATE_HOST:
    case ASX_HTTP_PERR_BAD_CONTENT_LENGTH:
    case ASX_HTTP_PERR_DUPLICATE_CONTENT_LENGTH:
    case ASX_HTTP_PERR_CONTENT_LENGTH_AND_CHUNKED:
    case ASX_HTTP_PERR_BAD_TRANSFER_ENCODING:
    case ASX_HTTP_PERR_UNSUPPORTED_TRANSFER_CODING:
    case ASX_HTTP_PERR_BAD_CHUNK_SIZE:
    case ASX_HTTP_PERR_CHUNK_SIZE_OVERFLOW:
    case ASX_HTTP_PERR_BAD_CHUNK_EXTENSION:
    case ASX_HTTP_PERR_BAD_CHUNK_TERMINATOR:
    case ASX_HTTP_PERR_BAD_TRAILER: return ASX_E_INVALID_ARGUMENT;
    }
    return ASX_E_INVALID_ARGUMENT;
}

/* ------------------------------------------------------------------ */
/* Parser                                                              */
/* ------------------------------------------------------------------ */

/* Parser states (asx_http_parser.state). */
enum {
    HTTP_PS_START = 0,        /* expecting a start line (leading CRLFs skipped) */
    HTTP_PS_START_LINE,       /* accumulating the start line */
    HTTP_PS_HEADER_LINE,      /* accumulating a header field line */
    HTTP_PS_BODY_LENGTH,      /* Content-Length body */
    HTTP_PS_CHUNK_SIZE,       /* chunk-size hex digits */
    HTTP_PS_CHUNK_EXT,        /* chunk extensions */
    HTTP_PS_CHUNK_SIZE_LF,    /* CR seen on the chunk-size line */
    HTTP_PS_CHUNK_DATA,       /* chunk payload */
    HTTP_PS_CHUNK_DATA_CR,    /* expecting CR after chunk payload */
    HTTP_PS_CHUNK_DATA_LF,    /* expecting LF after chunk payload */
    HTTP_PS_TRAILER_LINE,     /* accumulating a trailer field line */
    HTTP_PS_BODY_CLOSE,       /* close-delimited response body */
    HTTP_PS_COMPLETE_PENDING, /* MESSAGE_COMPLETE not yet reported */
    HTTP_PS_DONE,             /* message complete; next byte starts a new one */
    HTTP_PS_UPGRADED,         /* protocol switched; no further HTTP parsing */
    HTTP_PS_ERROR             /* sticky failure */
};

/* Chunk-extension sub-states (RFC 9112 section 7.1.1:
 * chunk-ext = *( BWS ";" BWS chunk-ext-name [ BWS "=" BWS chunk-ext-val ] )). */
enum {
    HTTP_EXT_NEED_SEMI = 0, /* whitespace seen; only more BWS or ';' may follow */
    HTTP_EXT_NAME_START,    /* after ';' */
    HTTP_EXT_NAME,          /* inside a name token (line may end here) */
    HTTP_EXT_AFTER_NAME,    /* BWS after a name */
    HTTP_EXT_VALUE_START,   /* after '=' */
    HTTP_EXT_VALUE_TOKEN,   /* inside a token value (line may end here) */
    HTTP_EXT_QUOTED,        /* inside a quoted-string value */
    HTTP_EXT_QUOTED_ESCAPE, /* after a backslash in a quoted-string */
    HTTP_EXT_AFTER_QUOTED   /* closing quote seen (line may end here) */
};

static asx_status http_parser_fail(asx_http_parser *p, asx_http_parse_error err) {
    p->error = err;
    p->state = HTTP_PS_ERROR;
    return http_perr_status(err);
}

static void http_parser_begin_message(asx_http_parser *p) {
    p->state = HTTP_PS_START;
    p->head_len = 0u;
    p->line_start = 0u;
    p->line_len = 0u;
    p->section_bytes = 0u;
    p->empty_lines = 0u;
    p->header_count = 0u;
    p->trailer_count = 0u;
    p->content_length = 0u;
    p->body_remaining = 0u;
    p->body_received = 0u;
    p->chunk_size = 0u;
    p->chunk_line_len = 0u;
    p->method = ASX_HTTP_GET;
    p->version = ASX_HTTP_VERSION_1_1;
    p->status = 0u;
    p->framing = ASX_HTTP_BODY_FRAMING_NONE;
    p->cr_seen = 0u;
    p->ext_state = 0u;
    p->chunk_digits = 0u;
    p->head_done = 0u;
    p->keep_alive = 0u;
    p->expect_continue = 0u;
    p->upgrade = 0u;
}

asx_status asx_http_parser_init(asx_http_parser *p, asx_http_parse_kind kind,
                                const asx_http_limits *limits, uint8_t *head_buf,
                                uint32_t head_cap) {
    if (p == NULL || head_buf == NULL || head_cap < 16u) return ASX_E_INVALID_ARGUMENT;
    if (kind != ASX_HTTP_PARSE_REQUEST && kind != ASX_HTTP_PARSE_RESPONSE) {
        return ASX_E_INVALID_ARGUMENT;
    }
    memset(p, 0, sizeof(*p));
    p->kind = kind;
    if (limits != NULL) {
        p->limits = *limits;
    } else {
        asx_http_limits_init(&p->limits);
    }
    if (p->limits.max_header_count > ASX_HTTP_PARSER_MAX_FIELDS) {
        p->limits.max_header_count = ASX_HTTP_PARSER_MAX_FIELDS;
    }
    p->head_buf = head_buf;
    p->head_cap = head_cap;
    p->response_to = ASX_HTTP_GET;
    p->error = ASX_HTTP_PERR_NONE;
    http_parser_begin_message(p);
    return ASX_OK;
}

void asx_http_parser_reset(asx_http_parser *p) {
    if (p == NULL) return;
    http_parser_begin_message(p);
    p->error = ASX_HTTP_PERR_NONE;
    p->eof = 0u;
    p->messages_completed = 0u;
}

void asx_http_parser_set_request_method(asx_http_parser *p, asx_http_method method) {
    if (p == NULL) return;
    p->response_to = method;
}

/* Accumulate one byte of a CRLF-terminated line into head_buf.
 * Returns 1 when the line is complete, 0 when absorbed, -1 on failure. */
static int http_line_byte(asx_http_parser *p, uint8_t b, asx_http_parse_error overflow_err) {
    if (p->cr_seen) {
        p->cr_seen = 0u;
        if (b == '\n') return 1;
        (void)http_parser_fail(p, ASX_HTTP_PERR_BAD_LINE_ENDING);
        return -1;
    }
    if (b == '\r') {
        p->cr_seen = 1u;
        return 0;
    }
    if (b == '\n') {
        (void)http_parser_fail(p, ASX_HTTP_PERR_BAD_LINE_ENDING);
        return -1;
    }
    /* Keep one spare byte so a field line can be rewritten as name\0value\0. */
    if (p->head_cap - p->head_len < 2u) {
        (void)http_parser_fail(p, overflow_err);
        return -1;
    }
    p->head_buf[p->head_len++] = b;
    p->line_len++;
    return 0;
}

static asx_http_parse_error http_parse_version(const uint8_t *s, uint32_t n,
                                               asx_http_version *out) {
    if (n != 8u || memcmp(s, "HTTP/", 5u) != 0 || !http_is_digit(s[5]) || s[6] != '.' ||
        !http_is_digit(s[7])) {
        return ASX_HTTP_PERR_BAD_VERSION;
    }
    if (s[5] == '1' && s[7] == '1') {
        *out = ASX_HTTP_VERSION_1_1;
        return ASX_HTTP_PERR_NONE;
    }
    if (s[5] == '1' && s[7] == '0') {
        *out = ASX_HTTP_VERSION_1_0;
        return ASX_HTTP_PERR_NONE;
    }
    return ASX_HTTP_PERR_UNSUPPORTED_VERSION;
}

/* Validate the request-target form against the method (RFC 9112 section 3.2). */
static int http_target_form_ok(asx_http_method method, const uint8_t *t, uint32_t n) {
    uint32_t i;

    if (method == ASX_HTTP_CONNECT) {
        /* authority-form: host ":" port */
        int has_colon = 0;
        for (i = 0u; i < n; i++) {
            if (t[i] == '/' || t[i] == '?' || t[i] == '#' || t[i] == '@') return 0;
            if (t[i] == ':') has_colon = 1;
        }
        return has_colon && t[0] != ':';
    }
    if (t[0] == '/') return 1;                                     /* origin-form */
    if (n == 1u && t[0] == '*') return method == ASX_HTTP_OPTIONS; /* asterisk-form */
    /* absolute-form: scheme "://" ... */
    if (!((t[0] >= 'a' && t[0] <= 'z') || (t[0] >= 'A' && t[0] <= 'Z'))) return 0;
    for (i = 1u; i < n; i++) {
        uint8_t c = t[i];
        if (c == ':') return (n - i) > 3u && t[i + 1u] == '/' && t[i + 2u] == '/';
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || http_is_digit(c) || c == '+' ||
              c == '-' || c == '.')) {
            return 0;
        }
    }
    return 0;
}

static asx_status http_parse_request_line(asx_http_parser *p) {
    uint8_t *line = p->head_buf;
    uint32_t n = p->line_len;
    uint32_t i = 0u;
    uint32_t m_end;
    uint32_t t_start;
    uint32_t t_len;
    asx_http_parse_error verr;

    while (i < n && line[i] != ' ') {
        if (!http_is_tchar(line[i])) return http_parser_fail(p, ASX_HTTP_PERR_BAD_START_LINE);
        i++;
    }
    if (i == 0u || i >= n) return http_parser_fail(p, ASX_HTTP_PERR_BAD_START_LINE);
    m_end = i;
    i++;
    t_start = i;
    while (i < n && line[i] != ' ') {
        if (!http_is_target_char(line[i])) return http_parser_fail(p, ASX_HTTP_PERR_BAD_TARGET);
        i++;
    }
    if (i == t_start || i >= n) return http_parser_fail(p, ASX_HTTP_PERR_BAD_START_LINE);
    t_len = i - t_start;
    i++;
    verr = http_parse_version(line + i, n - i, &p->version);
    if (verr != ASX_HTTP_PERR_NONE) return http_parser_fail(p, verr);
    if (!http_method_from_token(line, m_end, &p->method)) {
        return http_parser_fail(p, ASX_HTTP_PERR_UNKNOWN_METHOD);
    }
    if (!http_target_form_ok(p->method, line + t_start, t_len)) {
        return http_parser_fail(p, ASX_HTTP_PERR_BAD_TARGET);
    }
    memmove(p->head_buf, line + t_start, t_len);
    p->head_buf[t_len] = '\0';
    p->head_len = t_len + 1u;
    return ASX_OK;
}

static asx_status http_parse_status_line(asx_http_parser *p) {
    uint8_t *line = p->head_buf;
    uint32_t n = p->line_len;
    uint32_t i;
    uint32_t reason_len = 0u;
    asx_http_parse_error verr;

    if (n < 8u) return http_parser_fail(p, ASX_HTTP_PERR_BAD_STATUS);
    verr = http_parse_version(line, 8u, &p->version);
    if (verr != ASX_HTTP_PERR_NONE) return http_parser_fail(p, verr);
    if (n < 12u || line[8] != ' ' || !http_is_digit(line[9]) || !http_is_digit(line[10]) ||
        !http_is_digit(line[11])) {
        return http_parser_fail(p, ASX_HTTP_PERR_BAD_STATUS);
    }
    p->status = (asx_http_status)(((uint32_t)(line[9] - '0') * 100u) +
                                  ((uint32_t)(line[10] - '0') * 10u) + (uint32_t)(line[11] - '0'));
    if (p->status < 100u || p->status > 599u) return http_parser_fail(p, ASX_HTTP_PERR_BAD_STATUS);
    if (n > 12u) {
        if (line[12] != ' ') return http_parser_fail(p, ASX_HTTP_PERR_BAD_STATUS);
        for (i = 13u; i < n; i++) {
            if (!http_is_field_char(line[i])) return http_parser_fail(p, ASX_HTTP_PERR_BAD_STATUS);
        }
        reason_len = n - 13u;
        memmove(p->head_buf, line + 13u, reason_len);
    }
    p->head_buf[reason_len] = '\0';
    p->head_len = reason_len + 1u;
    return ASX_OK;
}

/* Validate and store the completed field line as name\0value\0. */
static asx_status http_store_field(asx_http_parser *p, int is_trailer) {
    uint8_t *line = p->head_buf + p->line_start;
    uint32_t n = p->line_len;
    uint32_t colon = 0u;
    uint32_t vs;
    uint32_t ve;
    uint32_t i;
    uint32_t value_len;
    uint32_t slot;

    while (colon < n && line[colon] != ':') colon++;
    if (colon == 0u || colon >= n) return http_parser_fail(p, ASX_HTTP_PERR_BAD_HEADER_NAME);
    for (i = 0u; i < colon; i++) {
        /* Also rejects whitespace between name and colon (RFC 9112 section 5.1). */
        if (!http_is_tchar(line[i])) return http_parser_fail(p, ASX_HTTP_PERR_BAD_HEADER_NAME);
    }
    vs = colon + 1u;
    while (vs < n && http_is_ows(line[vs])) vs++;
    ve = n;
    while (ve > vs && http_is_ows(line[ve - 1u])) ve--;
    for (i = vs; i < ve; i++) {
        if (!http_is_field_char(line[i]))
            return http_parser_fail(p, ASX_HTTP_PERR_BAD_HEADER_VALUE);
    }
    if (is_trailer) {
        if (p->trailer_count >= p->limits.max_header_count) {
            return http_parser_fail(p, ASX_HTTP_PERR_TOO_MANY_HEADERS);
        }
    } else if (p->header_count >= p->limits.max_header_count) {
        return http_parser_fail(p, ASX_HTTP_PERR_TOO_MANY_HEADERS);
    }
    slot = p->header_count + p->trailer_count;
    if (slot >= ASX_HTTP_PARSER_MAX_FIELDS) {
        return http_parser_fail(p, ASX_HTTP_PERR_TOO_MANY_HEADERS);
    }

    value_len = ve - vs;
    line[colon] = '\0';
    if (value_len > 0u) memmove(line + colon + 1u, line + vs, value_len);
    line[colon + 1u + value_len] = '\0';
    if (is_trailer && http_is_forbidden_trailer((const char *)line)) {
        return http_parser_fail(p, ASX_HTTP_PERR_BAD_TRAILER);
    }
    p->fields[slot].name_off = p->line_start;
    p->fields[slot].value_off = p->line_start + colon + 1u;
    if (is_trailer) {
        p->trailer_count++;
    } else {
        p->header_count++;
    }
    p->head_len = p->line_start + colon + 1u + value_len + 1u;
    return ASX_OK;
}

static const char *http_field_name(const asx_http_parser *p, uint32_t idx) {
    return (const char *)(p->head_buf + p->fields[idx].name_off);
}

static const char *http_field_value(const asx_http_parser *p, uint32_t idx) {
    return (const char *)(p->head_buf + p->fields[idx].value_off);
}

/* Content-Length = 1*DIGIT (no sign, no list, no internal whitespace). */
static int http_parse_content_length(const char *value, uint64_t *out) {
    uint64_t v = 0u;
    const char *c = value;

    if (c == NULL || *c == '\0') return 0;
    while (*c != '\0') {
        uint64_t d;
        if (*c < '0' || *c > '9') return 0;
        d = (uint64_t)(*c - '0');
        if (v > (UINT64_MAX - d) / 10u) return 0;
        v = (v * 10u) + d;
        c++;
    }
    *out = v;
    return 1;
}

/* Classify a Transfer-Encoding value. chunked must appear at most once and
 * only as the final coding; *other is set when non-chunked codings appear. */
static asx_http_parse_error http_classify_te(const char *value, int *chunked_final, int *other) {
    const char *cursor = value;
    const char *tok;
    size_t tok_len;
    size_t i;
    uint32_t count = 0u;
    int chunked_seen = 0;

    *chunked_final = 0;
    *other = 0;
    while (http_list_next(&cursor, &tok, &tok_len)) {
        count++;
        if (chunked_seen) return ASX_HTTP_PERR_BAD_TRANSFER_ENCODING;
        if (http_span_ieq(tok, tok_len, "chunked")) {
            chunked_seen = 1;
            continue;
        }
        for (i = 0u; i < tok_len; i++) {
            if (!http_is_tchar((uint8_t)tok[i])) return ASX_HTTP_PERR_BAD_TRANSFER_ENCODING;
        }
        *other = 1;
    }
    if (count == 0u) return ASX_HTTP_PERR_BAD_TRANSFER_ENCODING;
    *chunked_final = chunked_seen;
    return ASX_HTTP_PERR_NONE;
}

static void http_parser_enter_chunk_size(asx_http_parser *p) {
    p->state = HTTP_PS_CHUNK_SIZE;
    p->chunk_size = 0u;
    p->chunk_digits = 0u;
    p->chunk_line_len = 0u;
    p->ext_state = HTTP_EXT_NEED_SEMI;
}

/* Decide message framing and persistence once the header section is complete. */
static asx_status http_finish_head(asx_http_parser *p) {
    uint32_t i;
    uint32_t host_count = 0u;
    const char *te_value = NULL;
    const char *cl_value = NULL;
    int conn_close = 0;
    int conn_keep = 0;
    int expect_continue = 0;
    int chunked_final = 0;
    int other_codings = 0;
    asx_http_parse_error err;

    for (i = 0u; i < p->header_count; i++) {
        const char *name = http_field_name(p, i);
        const char *value = http_field_value(p, i);

        if (http_cstr_ieq(name, "content-length")) {
            if (cl_value != NULL)
                return http_parser_fail(p, ASX_HTTP_PERR_DUPLICATE_CONTENT_LENGTH);
            cl_value = value;
        } else if (http_cstr_ieq(name, "transfer-encoding")) {
            if (te_value != NULL) return http_parser_fail(p, ASX_HTTP_PERR_BAD_TRANSFER_ENCODING);
            te_value = value;
        } else if (http_cstr_ieq(name, "host")) {
            host_count++;
        } else if (http_cstr_ieq(name, "connection")) {
            if (asx_http_header_has_token(value, "close")) conn_close = 1;
            if (asx_http_header_has_token(value, "keep-alive")) conn_keep = 1;
        } else if (http_cstr_ieq(name, "expect")) {
            if (http_cstr_ieq(value, "100-continue")) expect_continue = 1;
        }
    }

    if (cl_value != NULL && !http_parse_content_length(cl_value, &p->content_length)) {
        return http_parser_fail(p, ASX_HTTP_PERR_BAD_CONTENT_LENGTH);
    }
    if (te_value != NULL && cl_value != NULL) {
        return http_parser_fail(p, ASX_HTTP_PERR_CONTENT_LENGTH_AND_CHUNKED);
    }
    if (te_value != NULL) {
        if (p->version == ASX_HTTP_VERSION_1_0) {
            return http_parser_fail(p, ASX_HTTP_PERR_BAD_TRANSFER_ENCODING);
        }
        err = http_classify_te(te_value, &chunked_final, &other_codings);
        if (err != ASX_HTTP_PERR_NONE) return http_parser_fail(p, err);
    }

    if (p->kind == ASX_HTTP_PARSE_REQUEST) {
        if (host_count > 1u) return http_parser_fail(p, ASX_HTTP_PERR_DUPLICATE_HOST);
        if (p->limits.require_host && p->version == ASX_HTTP_VERSION_1_1 && host_count == 0u) {
            return http_parser_fail(p, ASX_HTTP_PERR_MISSING_HOST);
        }
        if (te_value != NULL) {
            /* RFC 9112 section 6.3: chunked must be the final request coding. */
            if (!chunked_final) return http_parser_fail(p, ASX_HTTP_PERR_BAD_TRANSFER_ENCODING);
            if (other_codings) {
                return http_parser_fail(p, ASX_HTTP_PERR_UNSUPPORTED_TRANSFER_CODING);
            }
            p->framing = ASX_HTTP_BODY_FRAMING_CHUNKED;
        } else if (cl_value != NULL && p->content_length > 0u) {
            if (p->content_length > p->limits.max_body) {
                return http_parser_fail(p, ASX_HTTP_PERR_BODY_TOO_LARGE);
            }
            p->framing = ASX_HTTP_BODY_FRAMING_LENGTH;
        } else {
            p->framing = ASX_HTTP_BODY_FRAMING_NONE;
        }
        p->expect_continue = (uint8_t)(expect_continue && p->version == ASX_HTTP_VERSION_1_1);
    } else {
        int informational = p->status < 200u;
        if (informational) {
            p->framing = ASX_HTTP_BODY_FRAMING_NONE;
            if (p->status == ASX_HTTP_101_SWITCHING_PROTOCOLS) p->upgrade = 1u;
        } else if (p->response_to == ASX_HTTP_HEAD || p->status == ASX_HTTP_204_NO_CONTENT ||
                   p->status == ASX_HTTP_304_NOT_MODIFIED) {
            p->framing = ASX_HTTP_BODY_FRAMING_NONE;
        } else if (p->response_to == ASX_HTTP_CONNECT && p->status < 300u) {
            p->framing = ASX_HTTP_BODY_FRAMING_NONE;
            p->upgrade = 1u;
        } else if (te_value != NULL) {
            p->framing =
                chunked_final ? ASX_HTTP_BODY_FRAMING_CHUNKED : ASX_HTTP_BODY_FRAMING_CLOSE;
        } else if (cl_value != NULL) {
            if (p->content_length > p->limits.max_body) {
                return http_parser_fail(p, ASX_HTTP_PERR_BODY_TOO_LARGE);
            }
            p->framing =
                p->content_length > 0u ? ASX_HTTP_BODY_FRAMING_LENGTH : ASX_HTTP_BODY_FRAMING_NONE;
        } else {
            p->framing = ASX_HTTP_BODY_FRAMING_CLOSE;
        }
    }

    if (conn_close) {
        p->keep_alive = 0u;
    } else if (p->version == ASX_HTTP_VERSION_1_1) {
        p->keep_alive = 1u;
    } else {
        p->keep_alive = (uint8_t)conn_keep;
    }
    if (p->framing == ASX_HTTP_BODY_FRAMING_CLOSE || p->upgrade) p->keep_alive = 0u;
    p->head_done = 1u;

    switch (p->framing) {
    case ASX_HTTP_BODY_FRAMING_NONE: p->state = HTTP_PS_COMPLETE_PENDING; break;
    case ASX_HTTP_BODY_FRAMING_LENGTH:
        p->body_remaining = p->content_length;
        p->state = HTTP_PS_BODY_LENGTH;
        break;
    case ASX_HTTP_BODY_FRAMING_CHUNKED: http_parser_enter_chunk_size(p); break;
    case ASX_HTTP_BODY_FRAMING_CLOSE: p->state = HTTP_PS_BODY_CLOSE; break;
    }
    return ASX_OK;
}

/* Process one byte of a chunk extension. Returns ASX_OK or a failure status. */
static asx_status http_chunk_ext_byte(asx_http_parser *p, uint8_t b) {
    if (b == '\r') {
        if (p->ext_state == HTTP_EXT_NAME || p->ext_state == HTTP_EXT_VALUE_TOKEN ||
            p->ext_state == HTTP_EXT_AFTER_QUOTED) {
            p->state = HTTP_PS_CHUNK_SIZE_LF;
            return ASX_OK;
        }
        return http_parser_fail(p, ASX_HTTP_PERR_BAD_CHUNK_EXTENSION);
    }
    if (b == '\n') return http_parser_fail(p, ASX_HTTP_PERR_BAD_LINE_ENDING);

    switch (p->ext_state) {
    case HTTP_EXT_NEED_SEMI:
        if (http_is_ows(b)) return ASX_OK;
        if (b == ';') {
            p->ext_state = HTTP_EXT_NAME_START;
            return ASX_OK;
        }
        break;
    case HTTP_EXT_NAME_START:
        if (http_is_ows(b)) return ASX_OK;
        if (http_is_tchar(b)) {
            p->ext_state = HTTP_EXT_NAME;
            return ASX_OK;
        }
        break;
    case HTTP_EXT_NAME:
        if (http_is_tchar(b)) return ASX_OK;
        if (http_is_ows(b)) {
            p->ext_state = HTTP_EXT_AFTER_NAME;
            return ASX_OK;
        }
        if (b == '=') {
            p->ext_state = HTTP_EXT_VALUE_START;
            return ASX_OK;
        }
        if (b == ';') {
            p->ext_state = HTTP_EXT_NAME_START;
            return ASX_OK;
        }
        break;
    case HTTP_EXT_AFTER_NAME:
        if (http_is_ows(b)) return ASX_OK;
        if (b == '=') {
            p->ext_state = HTTP_EXT_VALUE_START;
            return ASX_OK;
        }
        if (b == ';') {
            p->ext_state = HTTP_EXT_NAME_START;
            return ASX_OK;
        }
        break;
    case HTTP_EXT_VALUE_START:
        if (http_is_ows(b)) return ASX_OK;
        if (b == '"') {
            p->ext_state = HTTP_EXT_QUOTED;
            return ASX_OK;
        }
        if (http_is_tchar(b)) {
            p->ext_state = HTTP_EXT_VALUE_TOKEN;
            return ASX_OK;
        }
        break;
    case HTTP_EXT_VALUE_TOKEN:
        if (http_is_tchar(b)) return ASX_OK;
        if (http_is_ows(b)) {
            p->ext_state = HTTP_EXT_NEED_SEMI;
            return ASX_OK;
        }
        if (b == ';') {
            p->ext_state = HTTP_EXT_NAME_START;
            return ASX_OK;
        }
        break;
    case HTTP_EXT_QUOTED:
        if (b == '"') {
            p->ext_state = HTTP_EXT_AFTER_QUOTED;
            return ASX_OK;
        }
        if (b == '\\') {
            p->ext_state = HTTP_EXT_QUOTED_ESCAPE;
            return ASX_OK;
        }
        if (http_is_field_char(b)) return ASX_OK;
        break;
    case HTTP_EXT_QUOTED_ESCAPE:
        if (http_is_field_char(b)) {
            p->ext_state = HTTP_EXT_QUOTED;
            return ASX_OK;
        }
        break;
    case HTTP_EXT_AFTER_QUOTED:
        if (http_is_ows(b)) {
            p->ext_state = HTTP_EXT_NEED_SEMI;
            return ASX_OK;
        }
        if (b == ';') {
            p->ext_state = HTTP_EXT_NAME_START;
            return ASX_OK;
        }
        break;
    default: break;
    }
    return http_parser_fail(p, ASX_HTTP_PERR_BAD_CHUNK_EXTENSION);
}

/* Shared body-delivery path: emit `n` bytes starting at in+pos. */
static asx_status http_emit_body(asx_http_parser *p, const uint8_t *in, uint32_t pos, uint32_t n,
                                 uint32_t *consumed, asx_http_event *ev) {
    p->body_received += n;
    ev->kind = ASX_HTTP_EVENT_BODY;
    ev->data = in + pos;
    ev->len = n;
    *consumed = pos + n;
    return ASX_OK;
}

asx_status asx_http_parser_feed(asx_http_parser *p, const void *data, uint32_t len,
                                uint32_t *consumed, asx_http_event *ev) {
    const uint8_t *in = (const uint8_t *)data;
    uint32_t pos = 0u;

    if (consumed != NULL) *consumed = 0u;
    if (ev != NULL) {
        ev->kind = ASX_HTTP_EVENT_NONE;
        ev->data = NULL;
        ev->len = 0u;
    }
    if (p == NULL || consumed == NULL || ev == NULL || (len > 0u && data == NULL)) {
        return ASX_E_INVALID_ARGUMENT;
    }
    if (p->state == HTTP_PS_ERROR) return http_perr_status(p->error);
    if (p->eof) return ASX_E_INVALID_STATE;

    for (;;) {
        uint8_t b;
        int r;
        asx_status st;

        switch (p->state) {
        case HTTP_PS_COMPLETE_PENDING:
            p->state = p->upgrade ? HTTP_PS_UPGRADED : HTTP_PS_DONE;
            p->messages_completed++;
            ev->kind = ASX_HTTP_EVENT_MESSAGE_COMPLETE;
            *consumed = pos;
            return ASX_OK;

        case HTTP_PS_DONE:
            if (pos >= len) goto need_more;
            http_parser_begin_message(p);
            break;

        case HTTP_PS_UPGRADED:
            if (pos >= len) goto need_more;
            return ASX_E_INVALID_STATE;

        case HTTP_PS_ERROR: return http_perr_status(p->error);

        case HTTP_PS_START:
            if (pos >= len) goto need_more;
            b = in[pos];
            if (p->cr_seen) {
                pos++;
                p->cr_seen = 0u;
                if (b != '\n') return http_parser_fail(p, ASX_HTTP_PERR_BAD_LINE_ENDING);
                p->empty_lines++;
                if (p->empty_lines > ASX_HTTP_MAX_LEADING_EMPTY_LINES) {
                    return http_parser_fail(p, ASX_HTTP_PERR_BAD_START_LINE);
                }
                break;
            }
            if (b == '\r') {
                pos++;
                p->cr_seen = 1u;
                break;
            }
            if (b == '\n') return http_parser_fail(p, ASX_HTTP_PERR_BAD_LINE_ENDING);
            p->state = HTTP_PS_START_LINE;
            p->line_start = 0u;
            p->line_len = 0u;
            break;

        case HTTP_PS_START_LINE:
            if (pos >= len) goto need_more;
            b = in[pos++];
            if (!p->cr_seen && b != '\r' && b != '\n' && p->line_len >= p->limits.max_start_line) {
                return http_parser_fail(p, ASX_HTTP_PERR_START_LINE_TOO_LONG);
            }
            r = http_line_byte(p, b, ASX_HTTP_PERR_START_LINE_TOO_LONG);
            if (r < 0) return http_perr_status(p->error);
            if (r == 0) break;
            if (p->line_len == 0u) return http_parser_fail(p, ASX_HTTP_PERR_BAD_START_LINE);
            st = (p->kind == ASX_HTTP_PARSE_REQUEST) ? http_parse_request_line(p)
                                                     : http_parse_status_line(p);
            if (st != ASX_OK) return st;
            p->state = HTTP_PS_HEADER_LINE;
            p->line_start = p->head_len;
            p->line_len = 0u;
            p->section_bytes = 0u;
            break;

        case HTTP_PS_HEADER_LINE:
        case HTTP_PS_TRAILER_LINE:
            if (pos >= len) goto need_more;
            b = in[pos++];
            p->section_bytes++;
            if (p->section_bytes > p->limits.max_header_bytes) {
                return http_parser_fail(p, ASX_HTTP_PERR_HEADERS_TOO_LARGE);
            }
            if (p->line_len == 0u && !p->cr_seen && http_is_ows(b)) {
                return http_parser_fail(p, ASX_HTTP_PERR_OBS_FOLD);
            }
            r = http_line_byte(p, b, ASX_HTTP_PERR_HEADERS_TOO_LARGE);
            if (r < 0) return http_perr_status(p->error);
            if (r == 0) break;
            if (p->line_len == 0u) {
                if (p->state == HTTP_PS_TRAILER_LINE) {
                    p->state = HTTP_PS_COMPLETE_PENDING;
                    break;
                }
                st = http_finish_head(p);
                if (st != ASX_OK) return st;
                ev->kind = ASX_HTTP_EVENT_HEAD;
                *consumed = pos;
                return ASX_OK;
            }
            st = http_store_field(p, p->state == HTTP_PS_TRAILER_LINE);
            if (st != ASX_OK) return st;
            p->line_start = p->head_len;
            p->line_len = 0u;
            break;

        case HTTP_PS_BODY_LENGTH: {
            uint32_t n;
            if (pos >= len) goto need_more;
            n = len - pos;
            if ((uint64_t)n > p->body_remaining) n = (uint32_t)p->body_remaining;
            p->body_remaining -= n;
            if (p->body_remaining == 0u) p->state = HTTP_PS_COMPLETE_PENDING;
            return http_emit_body(p, in, pos, n, consumed, ev);
        }

        case HTTP_PS_CHUNK_SIZE: {
            uint8_t d;
            if (pos >= len) goto need_more;
            b = in[pos++];
            if (b != '\r' && b != '\n') {
                p->chunk_line_len++;
                if (p->chunk_line_len > p->limits.max_chunk_line) {
                    return http_parser_fail(p, ASX_HTTP_PERR_CHUNK_LINE_TOO_LONG);
                }
            }
            if (http_hex_digit(b, &d)) {
                if (p->chunk_size > (UINT64_MAX >> 4)) {
                    return http_parser_fail(p, ASX_HTTP_PERR_CHUNK_SIZE_OVERFLOW);
                }
                p->chunk_size = (p->chunk_size << 4) | (uint64_t)d;
                p->chunk_digits = 1u;
                break;
            }
            if (!p->chunk_digits) return http_parser_fail(p, ASX_HTTP_PERR_BAD_CHUNK_SIZE);
            if (b == '\r') {
                p->state = HTTP_PS_CHUNK_SIZE_LF;
            } else if (b == ';') {
                p->ext_state = HTTP_EXT_NAME_START;
                p->state = HTTP_PS_CHUNK_EXT;
            } else if (http_is_ows(b)) {
                p->ext_state = HTTP_EXT_NEED_SEMI;
                p->state = HTTP_PS_CHUNK_EXT;
            } else if (b == '\n') {
                return http_parser_fail(p, ASX_HTTP_PERR_BAD_LINE_ENDING);
            } else {
                return http_parser_fail(p, ASX_HTTP_PERR_BAD_CHUNK_SIZE);
            }
            break;
        }

        case HTTP_PS_CHUNK_EXT:
            if (pos >= len) goto need_more;
            b = in[pos++];
            if (b != '\r' && b != '\n') {
                p->chunk_line_len++;
                if (p->chunk_line_len > p->limits.max_chunk_line) {
                    return http_parser_fail(p, ASX_HTTP_PERR_CHUNK_LINE_TOO_LONG);
                }
            }
            st = http_chunk_ext_byte(p, b);
            if (st != ASX_OK) return st;
            break;

        case HTTP_PS_CHUNK_SIZE_LF:
            if (pos >= len) goto need_more;
            b = in[pos++];
            if (b != '\n') return http_parser_fail(p, ASX_HTTP_PERR_BAD_LINE_ENDING);
            if (p->chunk_size == 0u) {
                p->state = HTTP_PS_TRAILER_LINE;
                p->line_start = p->head_len;
                p->line_len = 0u;
                p->section_bytes = 0u;
                p->cr_seen = 0u;
                break;
            }
            if (p->body_received > p->limits.max_body ||
                p->chunk_size > p->limits.max_body - p->body_received) {
                return http_parser_fail(p, ASX_HTTP_PERR_BODY_TOO_LARGE);
            }
            p->body_remaining = p->chunk_size;
            p->state = HTTP_PS_CHUNK_DATA;
            break;

        case HTTP_PS_CHUNK_DATA: {
            uint32_t n;
            if (pos >= len) goto need_more;
            n = len - pos;
            if ((uint64_t)n > p->body_remaining) n = (uint32_t)p->body_remaining;
            p->body_remaining -= n;
            if (p->body_remaining == 0u) p->state = HTTP_PS_CHUNK_DATA_CR;
            return http_emit_body(p, in, pos, n, consumed, ev);
        }

        case HTTP_PS_CHUNK_DATA_CR:
            if (pos >= len) goto need_more;
            b = in[pos++];
            if (b != '\r') return http_parser_fail(p, ASX_HTTP_PERR_BAD_CHUNK_TERMINATOR);
            p->state = HTTP_PS_CHUNK_DATA_LF;
            break;

        case HTTP_PS_CHUNK_DATA_LF:
            if (pos >= len) goto need_more;
            b = in[pos++];
            if (b != '\n') return http_parser_fail(p, ASX_HTTP_PERR_BAD_CHUNK_TERMINATOR);
            http_parser_enter_chunk_size(p);
            break;

        case HTTP_PS_BODY_CLOSE: {
            uint32_t n;
            if (pos >= len) goto need_more;
            n = len - pos;
            if (p->body_received > p->limits.max_body ||
                (uint64_t)n > p->limits.max_body - p->body_received) {
                return http_parser_fail(p, ASX_HTTP_PERR_BODY_TOO_LARGE);
            }
            return http_emit_body(p, in, pos, n, consumed, ev);
        }

        default: return http_parser_fail(p, ASX_HTTP_PERR_BAD_START_LINE);
        }
    }

need_more:
    *consumed = pos;
    return ASX_OK;
}

asx_status asx_http_parser_finish(asx_http_parser *p, asx_http_event *ev) {
    if (ev != NULL) {
        ev->kind = ASX_HTTP_EVENT_NONE;
        ev->data = NULL;
        ev->len = 0u;
    }
    if (p == NULL || ev == NULL) return ASX_E_INVALID_ARGUMENT;
    if (p->state == HTTP_PS_ERROR) return http_perr_status(p->error);

    switch (p->state) {
    case HTTP_PS_BODY_CLOSE:
    case HTTP_PS_COMPLETE_PENDING:
        p->state = p->upgrade ? HTTP_PS_UPGRADED : HTTP_PS_DONE;
        p->messages_completed++;
        p->eof = 1u;
        ev->kind = ASX_HTTP_EVENT_MESSAGE_COMPLETE;
        return ASX_OK;
    case HTTP_PS_DONE:
    case HTTP_PS_UPGRADED: p->eof = 1u; return ASX_OK;
    case HTTP_PS_START:
        p->eof = 1u;
        if (p->cr_seen) return http_parser_fail(p, ASX_HTTP_PERR_UNEXPECTED_EOF);
        return ASX_OK;
    default: p->eof = 1u; return http_parser_fail(p, ASX_HTTP_PERR_UNEXPECTED_EOF);
    }
}

int asx_http_parser_is_idle(const asx_http_parser *p) {
    if (p == NULL) return 0;
    if (p->state == HTTP_PS_DONE) return 1;
    return p->state == HTTP_PS_START && !p->cr_seen && p->empty_lines == 0u;
}

int asx_http_parser_is_upgraded(const asx_http_parser *p) {
    return p != NULL && p->state == HTTP_PS_UPGRADED;
}

asx_http_parse_error asx_http_parser_error(const asx_http_parser *p) {
    if (p == NULL) return ASX_HTTP_PERR_NONE;
    return p->error;
}

int asx_http_parser_head_complete(const asx_http_parser *p) { return p != NULL && p->head_done; }

asx_http_method asx_http_parser_method(const asx_http_parser *p) {
    if (p == NULL) return ASX_HTTP_GET;
    return p->method;
}

const char *asx_http_parser_target(const asx_http_parser *p) {
    if (p == NULL || !p->head_done || p->kind != ASX_HTTP_PARSE_REQUEST) return "";
    return (const char *)p->head_buf;
}

asx_http_version asx_http_parser_version(const asx_http_parser *p) {
    if (p == NULL) return ASX_HTTP_VERSION_1_1;
    return p->version;
}

asx_http_status asx_http_parser_status(const asx_http_parser *p) {
    if (p == NULL || !p->head_done) return 0u;
    return p->status;
}

const char *asx_http_parser_reason(const asx_http_parser *p) {
    if (p == NULL || !p->head_done || p->kind != ASX_HTTP_PARSE_RESPONSE) return "";
    return (const char *)p->head_buf;
}

uint32_t asx_http_parser_header_count(const asx_http_parser *p) {
    if (p == NULL || !p->head_done) return 0u;
    return p->header_count;
}

asx_status asx_http_parser_header_at(const asx_http_parser *p, uint32_t index,
                                     const char **out_name, const char **out_value) {
    if (p == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!p->head_done || index >= p->header_count) return ASX_E_NOT_FOUND;
    if (out_name != NULL) *out_name = http_field_name(p, index);
    if (out_value != NULL) *out_value = http_field_value(p, index);
    return ASX_OK;
}

const char *asx_http_parser_header(const asx_http_parser *p, const char *name) {
    uint32_t i;

    if (p == NULL || name == NULL || !p->head_done) return NULL;
    for (i = 0u; i < p->header_count; i++) {
        if (http_cstr_ieq(http_field_name(p, i), name)) return http_field_value(p, i);
    }
    return NULL;
}

uint32_t asx_http_parser_trailer_count(const asx_http_parser *p) {
    if (p == NULL || !p->head_done) return 0u;
    return p->trailer_count;
}

asx_status asx_http_parser_trailer_at(const asx_http_parser *p, uint32_t index,
                                      const char **out_name, const char **out_value) {
    if (p == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!p->head_done || index >= p->trailer_count) return ASX_E_NOT_FOUND;
    if (out_name != NULL) *out_name = http_field_name(p, p->header_count + index);
    if (out_value != NULL) *out_value = http_field_value(p, p->header_count + index);
    return ASX_OK;
}

asx_http_body_framing asx_http_parser_body_framing(const asx_http_parser *p) {
    if (p == NULL) return ASX_HTTP_BODY_FRAMING_NONE;
    return p->framing;
}

uint64_t asx_http_parser_content_length(const asx_http_parser *p) {
    if (p == NULL) return 0u;
    return p->content_length;
}

uint64_t asx_http_parser_body_received(const asx_http_parser *p) {
    if (p == NULL) return 0u;
    return p->body_received;
}

int asx_http_parser_keep_alive(const asx_http_parser *p) { return p != NULL && p->keep_alive; }

int asx_http_parser_expect_continue(const asx_http_parser *p) {
    return p != NULL && p->expect_continue;
}

uint32_t asx_http_parser_messages_completed(const asx_http_parser *p) {
    if (p == NULL) return 0u;
    return p->messages_completed;
}

/* Pre-flight the parsed fields against asx_http_headers capacities. */
static asx_status http_check_fields_fit(const asx_http_parser *p) {
    uint32_t i;

    if (p->header_count > ASX_HTTP_MAX_HEADERS) return ASX_E_RESOURCE_EXHAUSTED;
    for (i = 0u; i < p->header_count; i++) {
        if (http_bounded_strlen(http_field_name(p, i), ASX_HTTP_HEADER_NAME_MAX) >=
                ASX_HTTP_HEADER_NAME_MAX ||
            http_bounded_strlen(http_field_value(p, i), ASX_HTTP_HEADER_VALUE_MAX) >=
                ASX_HTTP_HEADER_VALUE_MAX) {
            return ASX_E_BUFFER_TOO_SMALL;
        }
    }
    return ASX_OK;
}

static void http_copy_fields(const asx_http_parser *p, asx_http_headers *hdrs) {
    uint32_t i;

    for (i = 0u; i < p->header_count; i++) {
        /* Capacities were pre-flighted by http_check_fields_fit. */
        if (asx_http_headers_add(hdrs, http_field_name(p, i), http_field_value(p, i)) != ASX_OK) {
            return;
        }
    }
}

asx_status asx_http_parser_copy_request(const asx_http_parser *p, asx_http_request *req) {
    const char *target;
    asx_status st;

    if (p == NULL || req == NULL) return ASX_E_INVALID_ARGUMENT;
    if (p->kind != ASX_HTTP_PARSE_REQUEST || !p->head_done) return ASX_E_INVALID_STATE;
    target = (const char *)p->head_buf;
    if (http_bounded_strlen(target, ASX_HTTP_URI_MAX) >= ASX_HTTP_URI_MAX) {
        return ASX_E_BUFFER_TOO_SMALL;
    }
    st = http_check_fields_fit(p);
    if (st != ASX_OK) return st;
    asx_http_request_init(req, p->method, target);
    req->version = p->version;
    http_copy_fields(p, &req->headers);
    return ASX_OK;
}

asx_status asx_http_parser_copy_response(const asx_http_parser *p, asx_http_response *resp) {
    asx_status st;

    if (p == NULL || resp == NULL) return ASX_E_INVALID_ARGUMENT;
    if (p->kind != ASX_HTTP_PARSE_RESPONSE || !p->head_done) return ASX_E_INVALID_STATE;
    st = http_check_fields_fit(p);
    if (st != ASX_OK) return st;
    asx_http_response_init(resp, p->status);
    resp->version = p->version;
    http_copy_fields(p, &resp->headers);
    return ASX_OK;
}

/* Map a struct-capacity copy failure onto the parse error a server reports. */
static asx_http_parse_error http_copy_failure_error(const asx_http_parser *p) {
    if (p->kind == ASX_HTTP_PARSE_REQUEST &&
        http_bounded_strlen((const char *)p->head_buf, ASX_HTTP_URI_MAX) >= ASX_HTTP_URI_MAX) {
        return ASX_HTTP_PERR_START_LINE_TOO_LONG;
    }
    return ASX_HTTP_PERR_HEADERS_TOO_LARGE;
}

asx_status asx_http_parser_collect_request(asx_http_parser *p, const void *data, uint32_t len,
                                           uint32_t *consumed, asx_http_request *req,
                                           int *out_complete) {
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t pos = 0u;
    uint64_t guard;

    if (consumed != NULL) *consumed = 0u;
    if (out_complete != NULL) *out_complete = 0;
    if (p == NULL || consumed == NULL || req == NULL || out_complete == NULL ||
        (len > 0u && data == NULL)) {
        return ASX_E_INVALID_ARGUMENT;
    }
    if (p->kind != ASX_HTTP_PARSE_REQUEST) return ASX_E_INVALID_ARGUMENT;

    /* Each iteration either consumes input or advances the state machine,
     * so 2 * len + a constant bounds the loop. */
    for (guard = 0u; guard <= ((uint64_t)len * 2u) + 8u; guard++) {
        asx_http_event ev;
        uint32_t used = 0u;
        asx_status st =
            asx_http_parser_feed(p, bytes == NULL ? NULL : bytes + pos, len - pos, &used, &ev);
        pos += used;
        *consumed = pos;
        if (st != ASX_OK) return st;
        switch (ev.kind) {
        case ASX_HTTP_EVENT_NONE: return ASX_OK;
        case ASX_HTTP_EVENT_HEAD:
            if (asx_http_parser_copy_request(p, req) != ASX_OK) {
                return http_parser_fail(p, http_copy_failure_error(p));
            }
            break;
        case ASX_HTTP_EVENT_BODY:
            if (asx_http_body_append(&req->body, ev.data, ev.len) != ASX_OK) {
                return http_parser_fail(p, ASX_HTTP_PERR_BODY_TOO_LARGE);
            }
            break;
        case ASX_HTTP_EVENT_MESSAGE_COMPLETE: *out_complete = 1; return ASX_OK;
        }
    }
    return ASX_OK;
}

asx_status asx_http_parser_collect_response(asx_http_parser *p, const void *data, uint32_t len,
                                            uint32_t *consumed, asx_http_response *resp,
                                            int *out_complete) {
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t pos = 0u;
    uint64_t guard;

    if (consumed != NULL) *consumed = 0u;
    if (out_complete != NULL) *out_complete = 0;
    if (p == NULL || consumed == NULL || resp == NULL || out_complete == NULL ||
        (len > 0u && data == NULL)) {
        return ASX_E_INVALID_ARGUMENT;
    }
    if (p->kind != ASX_HTTP_PARSE_RESPONSE) return ASX_E_INVALID_ARGUMENT;

    for (guard = 0u; guard <= ((uint64_t)len * 2u) + 8u; guard++) {
        asx_http_event ev;
        uint32_t used = 0u;
        int interim;
        asx_status st =
            asx_http_parser_feed(p, bytes == NULL ? NULL : bytes + pos, len - pos, &used, &ev);
        pos += used;
        *consumed = pos;
        if (st != ASX_OK) return st;
        interim = p->status < 200u && p->status != ASX_HTTP_101_SWITCHING_PROTOCOLS;
        switch (ev.kind) {
        case ASX_HTTP_EVENT_NONE: return ASX_OK;
        case ASX_HTTP_EVENT_HEAD:
            if (!interim && asx_http_parser_copy_response(p, resp) != ASX_OK) {
                return http_parser_fail(p, ASX_HTTP_PERR_HEADERS_TOO_LARGE);
            }
            break;
        case ASX_HTTP_EVENT_BODY:
            if (asx_http_body_append(&resp->body, ev.data, ev.len) != ASX_OK) {
                return http_parser_fail(p, ASX_HTTP_PERR_BODY_TOO_LARGE);
            }
            break;
        case ASX_HTTP_EVENT_MESSAGE_COMPLETE:
            if (interim) break;
            *out_complete = 1;
            return ASX_OK;
        }
    }
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Serializer                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t *buf;
    uint32_t cap;
    uint32_t len;
    uint8_t overflow;
} http_writer;

static void http_wr_bytes(http_writer *w, const void *src, size_t n) {
    if (w->overflow) return;
    if (n > (size_t)(w->cap - w->len)) {
        w->overflow = 1u;
        return;
    }
    if (n > 0u) memcpy(w->buf + w->len, src, n);
    w->len += (uint32_t)n;
}

static void http_wr_cstr(http_writer *w, const char *s) { http_wr_bytes(w, s, strlen(s)); }

static void http_wr_dec(http_writer *w, uint64_t v) {
    char tmp[20];
    uint32_t i = 20u;

    do {
        tmp[--i] = (char)('0' + (int)(v % 10u));
        v /= 10u;
    } while (v != 0u && i > 0u);
    http_wr_bytes(w, tmp + i, 20u - i);
}

static void http_wr_hex(http_writer *w, uint64_t v) {
    static const char digits[] = "0123456789abcdef";
    char tmp[16];
    uint32_t i = 16u;

    do {
        tmp[--i] = digits[v & 0x0fu];
        v >>= 4;
    } while (v != 0u && i > 0u);
    http_wr_bytes(w, tmp + i, 16u - i);
}

static void http_wr_field(http_writer *w, const char *name, const char *value) {
    http_wr_cstr(w, name);
    http_wr_bytes(w, ": ", 2u);
    http_wr_cstr(w, value);
    http_wr_bytes(w, "\r\n", 2u);
}

static int http_valid_field_name(const char *name) {
    size_t i;
    size_t len = http_bounded_strlen(name, ASX_HTTP_HEADER_NAME_MAX);

    if (len == 0u || len >= ASX_HTTP_HEADER_NAME_MAX) return 0;
    for (i = 0u; i < len; i++) {
        if (!http_is_tchar((uint8_t)name[i])) return 0;
    }
    return 1;
}

static int http_valid_field_value(const char *value, size_t max) {
    size_t i;
    size_t len = http_bounded_strlen(value, max);

    if (value == NULL || len >= max) return 0;
    for (i = 0u; i < len; i++) {
        if (!http_is_field_char((uint8_t)value[i])) return 0;
    }
    return 1;
}

static int http_valid_target(const char *uri) {
    size_t i;
    size_t len = http_bounded_strlen(uri, ASX_HTTP_URI_MAX);

    if (len == 0u || len >= ASX_HTTP_URI_MAX) return 0;
    for (i = 0u; i < len; i++) {
        if (!http_is_target_char((uint8_t)uri[i])) return 0;
    }
    return 1;
}

static int http_validate_headers(const asx_http_headers *hdrs) {
    uint32_t i;

    if (hdrs->count > ASX_HTTP_MAX_HEADERS) return 0;
    for (i = 0u; i < hdrs->count; i++) {
        if (!http_valid_field_name(hdrs->entries[i].name) ||
            !http_valid_field_value(hdrs->entries[i].value, ASX_HTTP_HEADER_VALUE_MAX)) {
            return 0;
        }
    }
    return 1;
}

static int http_serializer_skips(const char *name, const asx_http_serialize_opts *o,
                                 int date_emitted) {
    if (http_cstr_ieq(name, "content-length") || http_cstr_ieq(name, "transfer-encoding")) {
        return 1;
    }
    if (o->connection != ASX_HTTP_CONNECTION_DEFAULT && http_cstr_ieq(name, "connection")) {
        return 1;
    }
    return date_emitted && http_cstr_ieq(name, "date");
}

static void http_wr_user_headers(http_writer *w, const asx_http_headers *hdrs,
                                 const asx_http_serialize_opts *o, int date_emitted) {
    uint32_t i;

    for (i = 0u; i < hdrs->count; i++) {
        if (http_serializer_skips(hdrs->entries[i].name, o, date_emitted)) continue;
        http_wr_field(w, hdrs->entries[i].name, hdrs->entries[i].value);
    }
}

static void http_wr_trailer_section(http_writer *w, const asx_http_headers *trailers) {
    uint32_t i;

    http_wr_bytes(w, "0\r\n", 3u);
    if (trailers != NULL) {
        for (i = 0u; i < trailers->count; i++) {
            http_wr_field(w, trailers->entries[i].name, trailers->entries[i].value);
        }
    }
    http_wr_bytes(w, "\r\n", 2u);
}

static int http_validate_trailers(const asx_http_headers *trailers) {
    uint32_t i;

    if (trailers == NULL) return 1;
    if (!http_validate_headers(trailers)) return 0;
    for (i = 0u; i < trailers->count; i++) {
        if (http_is_forbidden_trailer(trailers->entries[i].name)) return 0;
    }
    return 1;
}

static void http_wr_connection(http_writer *w, asx_http_connection_directive directive) {
    if (directive == ASX_HTTP_CONNECTION_CLOSE) {
        http_wr_field(w, "Connection", "close");
    } else if (directive == ASX_HTTP_CONNECTION_KEEP_ALIVE) {
        http_wr_field(w, "Connection", "keep-alive");
    }
}

/* Fetch the Date value from the runtime clock hook (0 when no clock is installed). */
static int http_current_date(char *out, uint32_t out_size) {
    asx_time now = 0u;

    if (asx_runtime_now_ns(&now) != ASX_OK) return 0;
    return asx_http_format_date((uint64_t)(now / 1000000000u), out, out_size) == ASX_OK;
}

static asx_status http_finish_writer(const http_writer *w, uint32_t *out_len) {
    if (w->overflow) {
        *out_len = 0u;
        return ASX_E_BUFFER_TOO_SMALL;
    }
    *out_len = w->len;
    return ASX_OK;
}

static int http_method_allows_empty_length(asx_http_method m) {
    return m == ASX_HTTP_POST || m == ASX_HTTP_PUT || m == ASX_HTTP_PATCH;
}

static asx_status http_write_request(const asx_http_request *req,
                                     const asx_http_serialize_opts *opts, uint8_t *out,
                                     uint32_t cap, uint32_t *out_len, int full) {
    asx_http_serialize_opts defaults;
    const asx_http_serialize_opts *o = opts;
    http_writer w;
    char date[ASX_HTTP_DATE_LEN + 1u];
    int date_ok = 0;
    int has_host;
    uint32_t body_len;
    const char *method;

    if (out_len != NULL) *out_len = 0u;
    if (req == NULL || out == NULL || out_len == NULL) return ASX_E_INVALID_ARGUMENT;
    if (o == NULL) {
        asx_http_serialize_opts_init(&defaults);
        o = &defaults;
    }
    if ((unsigned)req->method > (unsigned)ASX_HTTP_TRACE) return ASX_E_INVALID_ARGUMENT;
    if (req->version != ASX_HTTP_VERSION_1_0 && req->version != ASX_HTTP_VERSION_1_1) {
        return ASX_E_INVALID_ARGUMENT;
    }
    if (!http_valid_target(req->uri) || !http_validate_headers(&req->headers)) {
        return ASX_E_INVALID_ARGUMENT;
    }
    body_len = asx_http_body_len(&req->body);
    if (body_len > ASX_HTTP_BODY_MAX) return ASX_E_INVALID_ARGUMENT;
    if (o->framing == ASX_HTTP_FRAMING_CLOSE) return ASX_E_INVALID_ARGUMENT;
    if (o->framing == ASX_HTTP_FRAMING_CHUNKED && req->version != ASX_HTTP_VERSION_1_1) {
        return ASX_E_INVALID_ARGUMENT;
    }
    if (full && o->framing == ASX_HTTP_FRAMING_NONE && body_len > 0u) {
        return ASX_E_INVALID_ARGUMENT;
    }
    if (full && o->framing == ASX_HTTP_FRAMING_CHUNKED && !http_validate_trailers(o->trailers)) {
        return ASX_E_INVALID_ARGUMENT;
    }
    has_host = asx_http_headers_get(&req->headers, "Host") != NULL;
    if (!has_host && o->host != NULL &&
        !http_valid_field_value(o->host, ASX_HTTP_HEADER_VALUE_MAX)) {
        return ASX_E_INVALID_ARGUMENT;
    }
    if (!has_host && o->host == NULL && req->version == ASX_HTTP_VERSION_1_1) {
        return ASX_E_INVALID_ARGUMENT;
    }
    if (o->emit_date) date_ok = http_current_date(date, sizeof(date));

    w.buf = out;
    w.cap = cap;
    w.len = 0u;
    w.overflow = 0u;
    method = asx_http_method_str(req->method);
    http_wr_cstr(&w, method);
    http_wr_bytes(&w, " ", 1u);
    http_wr_cstr(&w, req->uri);
    http_wr_bytes(&w, " ", 1u);
    http_wr_cstr(&w, asx_http_version_str(req->version));
    http_wr_bytes(&w, "\r\n", 2u);
    if (!has_host && o->host != NULL) http_wr_field(&w, "Host", o->host);
    http_wr_user_headers(&w, &req->headers, o, date_ok);

    switch (o->framing) {
    case ASX_HTTP_FRAMING_AUTO:
        if (body_len > 0u || http_method_allows_empty_length(req->method)) {
            http_wr_bytes(&w, "Content-Length: ", 16u);
            http_wr_dec(&w, body_len);
            http_wr_bytes(&w, "\r\n", 2u);
        }
        break;
    case ASX_HTTP_FRAMING_LENGTH:
        http_wr_bytes(&w, "Content-Length: ", 16u);
        http_wr_dec(&w, full ? (uint64_t)body_len : o->content_length);
        http_wr_bytes(&w, "\r\n", 2u);
        break;
    case ASX_HTTP_FRAMING_CHUNKED: http_wr_field(&w, "Transfer-Encoding", "chunked"); break;
    case ASX_HTTP_FRAMING_CLOSE:
    case ASX_HTTP_FRAMING_NONE: break;
    }
    http_wr_connection(&w, o->connection);
    if (date_ok) http_wr_field(&w, "Date", date);
    http_wr_bytes(&w, "\r\n", 2u);

    if (full) {
        if (o->framing == ASX_HTTP_FRAMING_CHUNKED) {
            if (body_len > 0u) {
                http_wr_hex(&w, body_len);
                http_wr_bytes(&w, "\r\n", 2u);
                http_wr_bytes(&w, req->body.data, body_len);
                http_wr_bytes(&w, "\r\n", 2u);
            }
            http_wr_trailer_section(&w, o->trailers);
        } else if (o->framing != ASX_HTTP_FRAMING_NONE) {
            http_wr_bytes(&w, req->body.data, body_len);
        }
    }
    return http_finish_writer(&w, out_len);
}

static int http_status_forbids_body(asx_http_status status) {
    return status < 200u || status == ASX_HTTP_204_NO_CONTENT ||
           status == ASX_HTTP_304_NOT_MODIFIED;
}

static asx_status http_write_response(const asx_http_response *resp,
                                      const asx_http_serialize_opts *opts, uint8_t *out,
                                      uint32_t cap, uint32_t *out_len, int full) {
    asx_http_serialize_opts defaults;
    asx_http_serialize_opts eff;
    http_writer w;
    char date[ASX_HTTP_DATE_LEN + 1u];
    int date_ok = 0;
    int no_body;
    uint32_t body_len;
    const char *reason;
    const char *user_cl;
    uint64_t user_cl_value = 0u;

    if (out_len != NULL) *out_len = 0u;
    if (resp == NULL || out == NULL || out_len == NULL) return ASX_E_INVALID_ARGUMENT;
    if (opts == NULL) {
        asx_http_serialize_opts_init(&defaults);
        opts = &defaults;
    }
    eff = *opts;
    if (resp->status < 100u || resp->status > 599u) return ASX_E_INVALID_ARGUMENT;
    if (resp->version != ASX_HTTP_VERSION_1_0 && resp->version != ASX_HTTP_VERSION_1_1) {
        return ASX_E_INVALID_ARGUMENT;
    }
    if (!http_validate_headers(&resp->headers)) return ASX_E_INVALID_ARGUMENT;
    no_body = http_status_forbids_body(resp->status);
    body_len = asx_http_body_len(&resp->body);
    if (body_len > ASX_HTTP_BODY_MAX) return ASX_E_INVALID_ARGUMENT;
    if (full && no_body && body_len > 0u) return ASX_E_INVALID_ARGUMENT;
    if (full && eff.framing == ASX_HTTP_FRAMING_NONE && body_len > 0u && !eff.head_response) {
        return ASX_E_INVALID_ARGUMENT;
    }
    if (eff.framing == ASX_HTTP_FRAMING_CHUNKED && resp->version != ASX_HTTP_VERSION_1_1) {
        return ASX_E_INVALID_ARGUMENT;
    }
    if (eff.framing == ASX_HTTP_FRAMING_CLOSE) {
        if (eff.connection == ASX_HTTP_CONNECTION_KEEP_ALIVE) return ASX_E_INVALID_ARGUMENT;
        eff.connection = ASX_HTTP_CONNECTION_CLOSE;
    }
    if (full && eff.framing == ASX_HTTP_FRAMING_CHUNKED && !http_validate_trailers(eff.trailers)) {
        return ASX_E_INVALID_ARGUMENT;
    }
    if (eff.emit_date) date_ok = http_current_date(date, sizeof(date));
    user_cl = asx_http_headers_get(&resp->headers, "Content-Length");
    if (user_cl != NULL && !http_parse_content_length(user_cl, &user_cl_value)) user_cl = NULL;

    w.buf = out;
    w.cap = cap;
    w.len = 0u;
    w.overflow = 0u;
    reason = http_reason_or_null(resp->status);
    http_wr_cstr(&w, asx_http_version_str(resp->version));
    http_wr_bytes(&w, " ", 1u);
    http_wr_dec(&w, resp->status);
    http_wr_bytes(&w, " ", 1u);
    if (reason != NULL) http_wr_cstr(&w, reason);
    http_wr_bytes(&w, "\r\n", 2u);
    http_wr_user_headers(&w, &resp->headers, &eff, date_ok);

    if (!no_body) {
        switch (eff.framing) {
        case ASX_HTTP_FRAMING_AUTO:
            http_wr_bytes(&w, "Content-Length: ", 16u);
            /* A HEAD handler may declare the GET length without producing the body. */
            if (eff.head_response && body_len == 0u && user_cl != NULL) {
                http_wr_dec(&w, user_cl_value);
            } else {
                http_wr_dec(&w, body_len);
            }
            http_wr_bytes(&w, "\r\n", 2u);
            break;
        case ASX_HTTP_FRAMING_LENGTH:
            http_wr_bytes(&w, "Content-Length: ", 16u);
            http_wr_dec(&w, full ? (uint64_t)body_len : eff.content_length);
            http_wr_bytes(&w, "\r\n", 2u);
            break;
        case ASX_HTTP_FRAMING_CHUNKED: http_wr_field(&w, "Transfer-Encoding", "chunked"); break;
        case ASX_HTTP_FRAMING_CLOSE:
        case ASX_HTTP_FRAMING_NONE: break;
        }
    }
    http_wr_connection(&w, eff.connection);
    if (date_ok) http_wr_field(&w, "Date", date);
    http_wr_bytes(&w, "\r\n", 2u);

    if (full && !no_body && !eff.head_response) {
        if (eff.framing == ASX_HTTP_FRAMING_CHUNKED) {
            if (body_len > 0u) {
                http_wr_hex(&w, body_len);
                http_wr_bytes(&w, "\r\n", 2u);
                http_wr_bytes(&w, resp->body.data, body_len);
                http_wr_bytes(&w, "\r\n", 2u);
            }
            http_wr_trailer_section(&w, eff.trailers);
        } else if (eff.framing != ASX_HTTP_FRAMING_NONE) {
            http_wr_bytes(&w, resp->body.data, body_len);
        }
    }
    return http_finish_writer(&w, out_len);
}

void asx_http_serialize_opts_init(asx_http_serialize_opts *opts) {
    if (opts == NULL) return;
    memset(opts, 0, sizeof(*opts));
    opts->framing = ASX_HTTP_FRAMING_AUTO;
    opts->connection = ASX_HTTP_CONNECTION_DEFAULT;
    opts->host = NULL;
    opts->trailers = NULL;
}

asx_status asx_http_serialize_request_head(const asx_http_request *req,
                                           const asx_http_serialize_opts *opts, uint8_t *out,
                                           uint32_t cap, uint32_t *out_len) {
    return http_write_request(req, opts, out, cap, out_len, 0);
}

asx_status asx_http_serialize_request(const asx_http_request *req,
                                      const asx_http_serialize_opts *opts, uint8_t *out,
                                      uint32_t cap, uint32_t *out_len) {
    return http_write_request(req, opts, out, cap, out_len, 1);
}

asx_status asx_http_serialize_response_head(const asx_http_response *resp,
                                            const asx_http_serialize_opts *opts, uint8_t *out,
                                            uint32_t cap, uint32_t *out_len) {
    return http_write_response(resp, opts, out, cap, out_len, 0);
}

asx_status asx_http_serialize_response(const asx_http_response *resp,
                                       const asx_http_serialize_opts *opts, uint8_t *out,
                                       uint32_t cap, uint32_t *out_len) {
    return http_write_response(resp, opts, out, cap, out_len, 1);
}

asx_status asx_http_encode_chunk(const void *data, uint32_t len, uint8_t *out, uint32_t cap,
                                 uint32_t *out_len) {
    http_writer w;

    if (out_len != NULL) *out_len = 0u;
    if (out == NULL || out_len == NULL || len == 0u || data == NULL) {
        return ASX_E_INVALID_ARGUMENT;
    }
    w.buf = out;
    w.cap = cap;
    w.len = 0u;
    w.overflow = 0u;
    http_wr_hex(&w, len);
    http_wr_bytes(&w, "\r\n", 2u);
    http_wr_bytes(&w, data, len);
    http_wr_bytes(&w, "\r\n", 2u);
    return http_finish_writer(&w, out_len);
}

asx_status asx_http_encode_last_chunk(const asx_http_headers *trailers, uint8_t *out, uint32_t cap,
                                      uint32_t *out_len) {
    http_writer w;

    if (out_len != NULL) *out_len = 0u;
    if (out == NULL || out_len == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!http_validate_trailers(trailers)) return ASX_E_INVALID_ARGUMENT;
    w.buf = out;
    w.cap = cap;
    w.len = 0u;
    w.overflow = 0u;
    http_wr_trailer_section(&w, trailers);
    return http_finish_writer(&w, out_len);
}

static void http_put2(char *dst, uint32_t v) {
    dst[0] = (char)('0' + (int)((v / 10u) % 10u));
    dst[1] = (char)('0' + (int)(v % 10u));
}

asx_status asx_http_format_date(uint64_t unix_seconds, char *out, uint32_t out_size) {
    static const char *const wdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    uint64_t days;
    uint64_t z;
    uint64_t era;
    uint64_t doe;
    uint64_t yoe;
    uint64_t year;
    uint64_t doy;
    uint64_t mp;
    uint32_t day;
    uint32_t month;
    uint32_t secs_of_day;
    uint32_t y;

    if (out == NULL || out_size < ASX_HTTP_DATE_LEN + 1u) return ASX_E_INVALID_ARGUMENT;
    days = unix_seconds / 86400u;
    secs_of_day = (uint32_t)(unix_seconds % 86400u);
    /* Civil-from-days (proleptic Gregorian), unsigned since days >= 0. */
    z = days + 719468u;
    era = z / 146097u;
    doe = z - (era * 146097u);
    yoe = (doe - (doe / 1460u) + (doe / 36524u) - (doe / 146096u)) / 365u;
    year = yoe + (era * 400u);
    doy = doe - ((365u * yoe) + (yoe / 4u) - (yoe / 100u));
    mp = ((5u * doy) + 2u) / 153u;
    day = (uint32_t)(doy - (((153u * mp) + 2u) / 5u) + 1u);
    month = (uint32_t)(mp < 10u ? mp + 3u : mp - 9u);
    if (month <= 2u) year++;
    if (year > 9999u) return ASX_E_INVALID_ARGUMENT;
    y = (uint32_t)year;

    memcpy(out, wdays[(days + 4u) % 7u], 3u);
    out[3] = ',';
    out[4] = ' ';
    http_put2(out + 5, day);
    out[7] = ' ';
    memcpy(out + 8, months[month - 1u], 3u);
    out[11] = ' ';
    http_put2(out + 12, y / 100u);
    http_put2(out + 14, y % 100u);
    out[16] = ' ';
    http_put2(out + 17, secs_of_day / 3600u);
    out[19] = ':';
    http_put2(out + 20, (secs_of_day / 60u) % 60u);
    out[22] = ':';
    http_put2(out + 23, secs_of_day % 60u);
    memcpy(out + 25, " GMT", 4u);
    out[ASX_HTTP_DATE_LEN] = '\0';
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Stream I/O helpers                                                  */
/* ------------------------------------------------------------------ */

/* Write as much of `data` as the transport accepts. The in-memory transport
 * accepts writes all-or-nothing, so a refused write is retried with halved
 * sizes before reporting ASX_E_PENDING. Real transports may write partially. */
static asx_status http_stream_write_some(asx_tcp_stream stream, const uint8_t *data, uint32_t len,
                                         uint32_t *written) {
    uint32_t attempt = len > ASX_BUF_CAPACITY ? ASX_BUF_CAPACITY : len;

    *written = 0u;
    while (attempt > 0u) {
        asx_buf src = asx_buf_from(data, attempt);
        uint32_t n = 0u;
        asx_status st = asx_tcp_stream_poll_write(stream, &src, &n);

        if (st == ASX_OK) {
            if (n > attempt) n = attempt;
            *written = n;
            return n > 0u ? ASX_OK : ASX_E_PENDING;
        }
        if (st == ASX_E_WOULD_BLOCK && attempt > 1u) {
            attempt /= 2u;
            continue;
        }
        if (st == ASX_E_WOULD_BLOCK) return ASX_E_PENDING;
        return st;
    }
    return ASX_OK;
}

/* Flush data[*pos..len). Returns ASX_OK once everything is written. */
static asx_status http_stream_flush(asx_tcp_stream stream, const uint8_t *data, uint32_t len,
                                    uint32_t *pos) {
    while (*pos < len) {
        uint32_t n = 0u;
        asx_status st = http_stream_write_some(stream, data + *pos, len - *pos, &n);
        if (st != ASX_OK) return st;
        *pos += n;
    }
    return ASX_OK;
}

/* Read into `rx`. ASX_OK when bytes arrived, ASX_E_DISCONNECTED on EOF
 * (ASX_OK with zero bytes, or the transport's own disconnect report). */
static asx_status http_stream_read(asx_tcp_stream stream, asx_buf_mut *rx) {
    uint32_t n = 0u;
    asx_status st;

    if (asx_buf_mut_remaining(rx) == 0u) {
        asx_buf_mut_clear(rx);
    } else {
        asx_buf_mut_compact(rx);
    }
    if (asx_buf_mut_writable(rx) == 0u) return ASX_E_BUFFER_TOO_SMALL;
    st = asx_tcp_stream_poll_read(stream, rx, &n);
    if (st == ASX_OK && n == 0u) return ASX_E_DISCONNECTED;
    return st;
}

static int http_is_wait_status(asx_status st) {
    return st == ASX_E_PENDING || st == ASX_E_WOULD_BLOCK;
}

/* Bound on state-machine steps per poll call (keeps polls cooperative). */
#define HTTP_CONN_POLL_STEPS 64u

/* ------------------------------------------------------------------ */
/* Client connection                                                   */
/* ------------------------------------------------------------------ */

static void http_client_fail(asx_http_client_conn *c) {
    if (c->state != ASX_HTTP_CONN_CLOSED) (void)asx_tcp_stream_close(c->stream);
    c->state = ASX_HTTP_CONN_CLOSED;
    c->request_open = 0u;
    c->keep_alive = 0u;
}

static void http_client_tx_compact(asx_http_client_conn *c) {
    if (c->tx_pos == 0u) return;
    if (c->tx_pos >= c->tx_len) {
        c->tx_pos = 0u;
        c->tx_len = 0u;
        return;
    }
    memmove(c->tx, c->tx + c->tx_pos, c->tx_len - c->tx_pos);
    c->tx_len -= c->tx_pos;
    c->tx_pos = 0u;
}

asx_status asx_http_client_conn_init(asx_http_client_conn *c, asx_tcp_stream stream,
                                     const asx_http_limits *limits) {
    asx_status st;

    if (c == NULL) return ASX_E_INVALID_ARGUMENT;
    memset(c, 0, sizeof(*c));
    st = asx_http_parser_init(&c->parser, ASX_HTTP_PARSE_RESPONSE, limits, c->head_buf,
                              (uint32_t)sizeof(c->head_buf));
    if (st != ASX_OK) return st;
    asx_buf_mut_init(&c->rx);
    c->stream = stream;
    c->state = ASX_HTTP_CONN_READING;
    c->request_method = ASX_HTTP_GET;
    c->keep_alive = 1u;
    return ASX_OK;
}

void asx_http_client_conn_set_body_sink(asx_http_client_conn *c, asx_http_body_sink_fn sink,
                                        void *ctx) {
    if (c == NULL) return;
    c->body_sink = sink;
    c->body_sink_ctx = sink != NULL ? ctx : NULL;
}

static asx_status http_client_ready(const asx_http_client_conn *c) {
    if (c->state == ASX_HTTP_CONN_CLOSED || c->request_open || !c->keep_alive) {
        return ASX_E_INVALID_STATE;
    }
    return ASX_OK;
}

static void http_client_open_request(asx_http_client_conn *c, asx_http_method method) {
    c->request_open = 1u;
    c->request_method = method;
    c->informational_seen = 0u;
    c->head_received = 0u;
    asx_http_parser_set_request_method(&c->parser, method);
    c->state = ASX_HTTP_CONN_WRITING;
}

asx_status asx_http_client_conn_send(asx_http_client_conn *c, const asx_http_request *req,
                                     const asx_http_serialize_opts *opts) {
    asx_status st;
    uint32_t n = 0u;

    if (c == NULL || req == NULL) return ASX_E_INVALID_ARGUMENT;
    st = http_client_ready(c);
    if (st != ASX_OK) return st;
    http_client_tx_compact(c);
    st = asx_http_serialize_request(req, opts, c->tx + c->tx_len,
                                    (uint32_t)sizeof(c->tx) - c->tx_len, &n);
    if (st != ASX_OK) return st;
    c->tx_len += n;
    c->body_mode = 0u;
    c->body_remaining = 0u;
    c->request_sent = 1u;
    http_client_open_request(c, req->method);
    return ASX_OK;
}

asx_status asx_http_client_conn_begin(asx_http_client_conn *c, const asx_http_request *req,
                                      const asx_http_serialize_opts *opts) {
    asx_status st;
    uint32_t n = 0u;

    if (c == NULL || req == NULL || opts == NULL) return ASX_E_INVALID_ARGUMENT;
    if (opts->framing != ASX_HTTP_FRAMING_CHUNKED && opts->framing != ASX_HTTP_FRAMING_LENGTH) {
        return ASX_E_INVALID_ARGUMENT;
    }
    st = http_client_ready(c);
    if (st != ASX_OK) return st;
    http_client_tx_compact(c);
    st = asx_http_serialize_request_head(req, opts, c->tx + c->tx_len,
                                         (uint32_t)sizeof(c->tx) - c->tx_len, &n);
    if (st != ASX_OK) return st;
    c->tx_len += n;
    if (opts->framing == ASX_HTTP_FRAMING_CHUNKED) {
        c->body_mode = 1u;
        c->body_remaining = 0u;
        c->request_sent = 0u;
    } else {
        c->body_mode = 2u;
        c->body_remaining = opts->content_length;
        c->request_sent = opts->content_length == 0u;
        if (c->request_sent) c->body_mode = 0u;
    }
    http_client_open_request(c, req->method);
    return ASX_OK;
}

asx_status asx_http_client_conn_write_body(asx_http_client_conn *c, const void *data,
                                           uint32_t len) {
    uint32_t free_space;
    uint32_t n = 0u;
    asx_status st;

    if (c == NULL || (len > 0u && data == NULL)) return ASX_E_INVALID_ARGUMENT;
    if (c->state == ASX_HTTP_CONN_CLOSED || !c->request_open || c->request_sent ||
        c->body_mode == 0u) {
        return ASX_E_INVALID_STATE;
    }
    if (len == 0u) return ASX_OK;
    http_client_tx_compact(c);
    free_space = (uint32_t)sizeof(c->tx) - c->tx_len;
    if (c->body_mode == 2u) {
        if ((uint64_t)len > c->body_remaining) return ASX_E_INVALID_ARGUMENT;
        if (len > (uint32_t)sizeof(c->tx)) return ASX_E_BUFFER_TOO_SMALL;
        if (len > free_space) return ASX_E_WOULD_BLOCK;
        memcpy(c->tx + c->tx_len, data, len);
        c->tx_len += len;
        c->body_remaining -= len;
    } else {
        st = asx_http_encode_chunk(data, len, c->tx + c->tx_len, free_space, &n);
        if (st == ASX_E_BUFFER_TOO_SMALL) {
            /* Chunk framing is at most 16 hex digits + 2 CRLFs. */
            return (len > (uint32_t)sizeof(c->tx) - 20u) ? ASX_E_BUFFER_TOO_SMALL
                                                         : ASX_E_WOULD_BLOCK;
        }
        if (st != ASX_OK) return st;
        c->tx_len += n;
    }
    c->state = ASX_HTTP_CONN_WRITING;
    return ASX_OK;
}

asx_status asx_http_client_conn_finish(asx_http_client_conn *c, const asx_http_headers *trailers) {
    uint32_t n = 0u;
    asx_status st;

    if (c == NULL) return ASX_E_INVALID_ARGUMENT;
    if (c->state == ASX_HTTP_CONN_CLOSED || !c->request_open || c->request_sent ||
        c->body_mode == 0u) {
        return ASX_E_INVALID_STATE;
    }
    if (c->body_mode == 2u) {
        if (trailers != NULL && trailers->count > 0u) return ASX_E_INVALID_ARGUMENT;
        if (c->body_remaining != 0u) return ASX_E_INVALID_STATE;
    } else {
        http_client_tx_compact(c);
        st = asx_http_encode_last_chunk(trailers, c->tx + c->tx_len,
                                        (uint32_t)sizeof(c->tx) - c->tx_len, &n);
        if (st == ASX_E_BUFFER_TOO_SMALL) {
            return c->tx_len > 0u ? ASX_E_WOULD_BLOCK : ASX_E_BUFFER_TOO_SMALL;
        }
        if (st != ASX_OK) return st;
        c->tx_len += n;
        c->state = ASX_HTTP_CONN_WRITING;
    }
    c->request_sent = 1u;
    c->body_mode = 0u;
    return ASX_OK;
}

/* Complete the in-flight exchange after the final response. */
static asx_status http_client_complete(asx_http_client_conn *c, int force_close) {
    c->request_open = 0u;
    c->responses_received++;
    c->keep_alive =
        (uint8_t)(!force_close && c->request_sent && asx_http_parser_keep_alive(&c->parser));
    if (!c->keep_alive) {
        (void)asx_tcp_stream_close(c->stream);
        c->state = ASX_HTTP_CONN_CLOSED;
    } else {
        c->state = ASX_HTTP_CONN_READING;
    }
    return ASX_OK;
}

/* Parse buffered response bytes. Returns ASX_OK with *done=1 when the final
 * response completed, ASX_OK with *done=0 when more input is needed. */
static asx_status http_client_process(asx_http_client_conn *c, asx_http_response *resp, int *done) {
    uint32_t guard;

    *done = 0;
    for (guard = 0u; guard < HTTP_CONN_POLL_STEPS; guard++) {
        asx_buf readable = asx_buf_mut_readable(&c->rx);
        asx_http_event ev;
        uint32_t used = 0u;
        int interim;
        asx_status st;

        st = asx_http_parser_feed(&c->parser, readable.ptr, readable.len, &used, &ev);
        if (used > 0u && asx_buf_mut_advance(&c->rx, used) != ASX_OK) return ASX_E_INVALID_STATE;
        if (st != ASX_OK) return st;
        interim = c->parser.status < 200u && c->parser.status != ASX_HTTP_101_SWITCHING_PROTOCOLS;
        switch (ev.kind) {
        case ASX_HTTP_EVENT_NONE: return ASX_OK;
        case ASX_HTTP_EVENT_HEAD:
            if (interim) {
                c->informational_seen++;
                if (c->informational_seen > ASX_HTTP_CLIENT_MAX_INFORMATIONAL) {
                    return ASX_E_RESOURCE_EXHAUSTED;
                }
                break;
            }
            st = asx_http_parser_copy_response(&c->parser, resp);
            if (st != ASX_OK) {
                return http_parser_fail(&c->parser, ASX_HTTP_PERR_HEADERS_TOO_LARGE);
            }
            if (c->body_sink != NULL) resp->body.kind = ASX_HTTP_BODY_STREAM;
            c->head_received = 1u;
            break;
        case ASX_HTTP_EVENT_BODY:
            if (!c->head_received) break;
            if (c->body_sink != NULL) {
                st = c->body_sink(c->body_sink_ctx, ev.data, ev.len);
                if (st != ASX_OK) return st;
                resp->body.content_length += ev.len;
            } else if (asx_http_body_append(&resp->body, ev.data, ev.len) != ASX_OK) {
                return http_parser_fail(&c->parser, ASX_HTTP_PERR_BODY_TOO_LARGE);
            }
            break;
        case ASX_HTTP_EVENT_MESSAGE_COMPLETE:
            if (interim || !c->head_received) break;
            *done = 1;
            return http_client_complete(c, asx_http_parser_is_upgraded(&c->parser));
        }
    }
    return ASX_OK;
}

asx_status asx_http_client_conn_poll(asx_http_client_conn *c, asx_http_response *resp) {
    uint32_t guard;

    if (c == NULL || resp == NULL) return ASX_E_INVALID_ARGUMENT;
    if (c->state == ASX_HTTP_CONN_CLOSED || !c->request_open) return ASX_E_INVALID_STATE;

    for (guard = 0u; guard < HTTP_CONN_POLL_STEPS; guard++) {
        asx_status st;
        int done = 0;

        if (c->tx_pos < c->tx_len) {
            st = http_stream_flush(c->stream, c->tx, c->tx_len, &c->tx_pos);
            if (http_is_wait_status(st)) return ASX_E_PENDING;
            if (st != ASX_OK) {
                http_client_fail(c);
                return st;
            }
            c->tx_pos = 0u;
            c->tx_len = 0u;
            c->state = ASX_HTTP_CONN_READING;
        }

        if (asx_buf_mut_remaining(&c->rx) > 0u) {
            st = http_client_process(c, resp, &done);
            if (st != ASX_OK) {
                http_client_fail(c);
                return st;
            }
            if (done) return ASX_OK;
            if (asx_buf_mut_remaining(&c->rx) > 0u) continue;
        }

        if (c->peer_eof) {
            asx_http_event ev;
            st = asx_http_parser_finish(&c->parser, &ev);
            if (st == ASX_OK && ev.kind == ASX_HTTP_EVENT_MESSAGE_COMPLETE && c->head_received) {
                return http_client_complete(c, 1);
            }
            http_client_fail(c);
            return st != ASX_OK ? st : ASX_E_DISCONNECTED;
        }

        st = http_stream_read(c->stream, &c->rx);
        if (st == ASX_OK) continue;
        if (http_is_wait_status(st)) return ASX_E_PENDING;
        if (st == ASX_E_DISCONNECTED) {
            c->peer_eof = 1u;
            continue;
        }
        http_client_fail(c);
        return st;
    }
    return ASX_E_PENDING;
}

void asx_http_client_conn_notify_eof(asx_http_client_conn *c) {
    if (c == NULL) return;
    c->peer_eof = 1u;
}

int asx_http_client_conn_reusable(const asx_http_client_conn *c) {
    return c != NULL && c->state != ASX_HTTP_CONN_CLOSED && !c->request_open && c->keep_alive &&
           !c->peer_eof;
}

asx_status asx_http_client_conn_close(asx_http_client_conn *c) {
    if (c == NULL) return ASX_E_INVALID_ARGUMENT;
    if (c->state != ASX_HTTP_CONN_CLOSED) (void)asx_tcp_stream_close(c->stream);
    c->state = ASX_HTTP_CONN_CLOSED;
    c->request_open = 0u;
    c->keep_alive = 0u;
    return ASX_OK;
}

#if ASX_HAS_SERVER_SURFACE
/* ------------------------------------------------------------------ */
/* Server connection                                                   */
/* ------------------------------------------------------------------ */

void asx_http_server_config_init(asx_http_server_config *cfg, asx_http_router *router) {
    if (cfg == NULL) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->router = router;
    asx_http_limits_init(&cfg->limits);
    cfg->max_requests = 1000u;
    cfg->idle_timeout_ms = 60000u;
    cfg->keep_alive_enabled = 1u;
    cfg->auto_continue = 1u;
    cfg->emit_date = 0u;
    cfg->body_sink = NULL;
    cfg->body_sink_ctx = NULL;
}

/* Start a new idle-timeout window now (no window without a runtime clock). */
static void http_server_window_restart(asx_http_server_conn *c) {
    c->window_open = (uint8_t)(asx_runtime_now_ns(&c->window_start) == ASX_OK);
}

asx_status asx_http_server_conn_init(asx_http_server_conn *c, asx_tcp_stream stream,
                                     const asx_http_server_config *cfg) {
    asx_status st;

    if (c == NULL || cfg == NULL || cfg->router == NULL) return ASX_E_INVALID_ARGUMENT;
    st = asx_surface_gate(ASX_SURFACE_SERVER);
    if (st != ASX_OK) return st;
    memset(c, 0, sizeof(*c));
    c->stream = stream;
    c->config = *cfg;
    st = asx_http_parser_init(&c->parser, ASX_HTTP_PARSE_REQUEST, &cfg->limits, c->head_buf,
                              (uint32_t)sizeof(c->head_buf));
    if (st != ASX_OK) return st;
    asx_buf_mut_init(&c->rx);
    asx_http_request_init(&c->request, ASX_HTTP_GET, "/");
    asx_http_response_init(&c->response, ASX_HTTP_200_OK);
    c->state = ASX_HTTP_CONN_READING;
    c->last_error = ASX_HTTP_PERR_NONE;
    http_server_window_restart(c);
    return ASX_OK;
}

static int http_server_tx_pending(const asx_http_server_conn *c) {
    return c->tx_pos < c->tx_len || (c->tx_body != NULL && c->tx_body_pos < c->tx_body_len);
}

/* The idle-timeout deadline of the current window; 0 when there is none
 * (no timeout configured, or no runtime clock when the window opened). */
static int http_server_deadline(const asx_http_server_conn *c, asx_time *out) {
    if (c->config.idle_timeout_ms == 0u || !c->window_open) return 0;
    *out = c->window_start + (asx_time)c->config.idle_timeout_ms * 1000000u;
    return 1;
}

static void http_server_close(asx_http_server_conn *c) {
    if (c->state != ASX_HTTP_CONN_CLOSED) (void)asx_tcp_stream_close(c->stream);
    c->state = ASX_HTTP_CONN_CLOSED;
    c->in_message = 0u;
    c->tx_len = 0u;
    c->tx_pos = 0u;
    c->tx_body = NULL;
    c->tx_body_len = 0u;
    c->tx_body_pos = 0u;
}

/* Queue the current c->response with framing/persistence decided here. */
static void http_server_queue_response(asx_http_server_conn *c, int keep, int head_request) {
    asx_http_serialize_opts opts;
    asx_status st;

    asx_http_serialize_opts_init(&opts);
    opts.framing = ASX_HTTP_FRAMING_AUTO;
    opts.head_response = (uint8_t)head_request;
    opts.emit_date = c->config.emit_date;
    if (!keep) {
        opts.connection = ASX_HTTP_CONNECTION_CLOSE;
    } else if (c->response.version == ASX_HTTP_VERSION_1_0) {
        opts.connection = ASX_HTTP_CONNECTION_KEEP_ALIVE;
    }
    st = asx_http_serialize_response_head(&c->response, &opts, c->tx, (uint32_t)sizeof(c->tx),
                                          &c->tx_len);
    if (st != ASX_OK) {
        /* The handler produced an unserializable response: answer 500 and close. */
        asx_http_version ver = c->response.version;
        asx_http_response_init(&c->response, ASX_HTTP_500_INTERNAL_ERROR);
        c->response.version = ver;
        opts.connection = ASX_HTTP_CONNECTION_CLOSE;
        opts.head_response = 0u;
        keep = 0;
        head_request = 0;
        if (asx_http_serialize_response_head(&c->response, &opts, c->tx, (uint32_t)sizeof(c->tx),
                                             &c->tx_len) != ASX_OK) {
            c->tx_len = 0u;
        }
    }
    c->tx_pos = 0u;
    if (!head_request && c->response.body.len > 0u) {
        c->tx_body = c->response.body.data;
        c->tx_body_len = c->response.body.len;
    } else {
        c->tx_body = NULL;
        c->tx_body_len = 0u;
    }
    c->tx_body_pos = 0u;
    c->close_after_write = (uint8_t)!keep;
    c->state = ASX_HTTP_CONN_WRITING;
    http_server_window_restart(c);
}

/* Answer a protocol failure with the mapped status and close afterwards. */
static void http_server_queue_error(asx_http_server_conn *c, asx_http_status status) {
    const char *reason = asx_http_status_reason(status);

    asx_http_response_init(&c->response, status);
    if (asx_http_headers_add(&c->response.headers, "Content-Type", "text/plain") != ASX_OK ||
        asx_http_body_set_bytes(&c->response.body, reason, (uint32_t)strlen(reason)) != ASX_OK) {
        asx_http_body_init(&c->response.body);
    }
    c->in_message = 0u;
    http_server_queue_response(c, 0, 0);
}

static void http_server_queue_continue(asx_http_server_conn *c) {
    static const char line[] = "HTTP/1.1 100 Continue\r\n\r\n";

    memcpy(c->tx, line, sizeof(line) - 1u);
    c->tx_len = (uint32_t)(sizeof(line) - 1u);
    c->tx_pos = 0u;
    c->tx_body = NULL;
    c->tx_body_len = 0u;
    c->tx_body_pos = 0u;
    c->state = ASX_HTTP_CONN_WRITING;
    http_server_window_restart(c);
}

static void http_server_dispatch(asx_http_server_conn *c) {
    asx_http_method original = c->request.method;
    int head_request = original == ASX_HTTP_HEAD;
    int keep = asx_http_parser_keep_alive(&c->parser);
    asx_status st;

    c->in_message = 0u;
    if (!c->config.keep_alive_enabled || c->drain_requested || c->peer_eof) keep = 0;
    if (c->config.max_requests != 0u && c->requests_served + 1u >= c->config.max_requests) {
        keep = 0;
    }

    asx_http_response_init(&c->response, ASX_HTTP_200_OK);
    st = asx_http_router_dispatch(c->config.router, &c->request, &c->response, c->config.session,
                                  c->config.security, NULL);
    if (st == ASX_E_NOT_FOUND && head_request) {
        /* RFC 9110 section 9.3.2: HEAD mirrors GET without the content. */
        c->request.method = ASX_HTTP_GET;
        asx_http_response_init(&c->response, ASX_HTTP_200_OK);
        st = asx_http_router_dispatch(c->config.router, &c->request, &c->response,
                                      c->config.session, c->config.security, NULL);
        c->request.method = original;
    }
    if (st == ASX_E_NOT_FOUND) {
        asx_http_response_init(&c->response, ASX_HTTP_404_NOT_FOUND);
    } else if (st != ASX_OK) {
        asx_http_response_init(&c->response, ASX_HTTP_500_INTERNAL_ERROR);
    }
    c->response.version =
        c->request.version == ASX_HTTP_VERSION_1_0 ? ASX_HTTP_VERSION_1_0 : ASX_HTTP_VERSION_1_1;
    if (asx_http_headers_has_token(&c->response.headers, "Connection", "close")) keep = 0;
    if (http_status_forbids_body(c->response.status)) asx_http_body_init(&c->response.body);
    c->requests_served++;
    http_server_queue_response(c, keep, head_request);
}

static void http_server_on_head(asx_http_server_conn *c) {
    asx_http_body_framing framing;

    if (asx_http_parser_copy_request(&c->parser, &c->request) != ASX_OK) {
        c->last_error = http_copy_failure_error(&c->parser);
        http_server_queue_error(c, asx_http_parse_error_status(c->last_error));
        return;
    }
    c->in_message = 1u;
    if (c->config.body_sink != NULL) c->request.body.kind = ASX_HTTP_BODY_STREAM;
    framing = asx_http_parser_body_framing(&c->parser);
    if (c->config.auto_continue && asx_http_parser_expect_continue(&c->parser) &&
        framing != ASX_HTTP_BODY_FRAMING_NONE && asx_buf_mut_remaining(&c->rx) == 0u) {
        http_server_queue_continue(c);
    }
}

static void http_server_on_body(asx_http_server_conn *c, const asx_http_event *ev) {
    if (c->config.body_sink != NULL) {
        if (c->config.body_sink(c->config.body_sink_ctx, ev->data, ev->len) != ASX_OK) {
            http_server_queue_error(c, ASX_HTTP_500_INTERNAL_ERROR);
            return;
        }
        c->request.body.content_length += ev->len;
        return;
    }
    if (asx_http_body_append(&c->request.body, ev->data, ev->len) != ASX_OK) {
        c->last_error = ASX_HTTP_PERR_BODY_TOO_LARGE;
        http_server_queue_error(c, ASX_HTTP_413_CONTENT_TOO_LARGE);
    }
}

/* Parse buffered input until a response is queued or more input is needed. */
static asx_status http_server_process_input(asx_http_server_conn *c) {
    uint32_t guard;

    for (guard = 0u; guard < HTTP_CONN_POLL_STEPS; guard++) {
        asx_buf readable;
        asx_http_event ev;
        uint32_t used = 0u;
        asx_status st;

        if (http_server_tx_pending(c) || c->close_after_write) return ASX_OK;
        if (asx_buf_mut_remaining(&c->rx) == 0u && c->parser.state != HTTP_PS_COMPLETE_PENDING) {
            return ASX_OK;
        }
        readable = asx_buf_mut_readable(&c->rx);
        st = asx_http_parser_feed(&c->parser, readable.ptr, readable.len, &used, &ev);
        if (used > 0u && asx_buf_mut_advance(&c->rx, used) != ASX_OK) return ASX_E_INVALID_STATE;
        if (st != ASX_OK) {
            c->last_error = asx_http_parser_error(&c->parser);
            if (c->last_error == ASX_HTTP_PERR_NONE) c->last_error = ASX_HTTP_PERR_BAD_START_LINE;
            http_server_queue_error(c, asx_http_parse_error_status(c->last_error));
            return ASX_OK;
        }
        switch (ev.kind) {
        case ASX_HTTP_EVENT_NONE: return ASX_OK;
        case ASX_HTTP_EVENT_HEAD: http_server_on_head(c); break;
        case ASX_HTTP_EVENT_BODY: http_server_on_body(c, &ev); break;
        case ASX_HTTP_EVENT_MESSAGE_COMPLETE: http_server_dispatch(c); return ASX_OK;
        }
    }
    return ASX_OK;
}

static asx_status http_server_conn_step(asx_http_server_conn *c) {
    uint32_t guard;

    for (guard = 0u; guard < HTTP_CONN_POLL_STEPS; guard++) {
        asx_status st;

        if (http_server_tx_pending(c)) {
            uint32_t head_pos = c->tx_pos;
            uint32_t body_pos = c->tx_body_pos;
            st = http_stream_flush(c->stream, c->tx, c->tx_len, &c->tx_pos);
            if (st == ASX_OK && c->tx_body != NULL) {
                st = http_stream_flush(c->stream, c->tx_body, c->tx_body_len, &c->tx_body_pos);
            }
            /* Bytes went out: the stalled-write window starts over. */
            if (c->tx_pos != head_pos || c->tx_body_pos != body_pos) {
                http_server_window_restart(c);
            }
            if (http_is_wait_status(st)) return ASX_E_PENDING;
            if (st != ASX_OK) {
                http_server_close(c);
                return st;
            }
            c->tx_len = 0u;
            c->tx_pos = 0u;
            c->tx_body = NULL;
            c->tx_body_len = 0u;
            c->tx_body_pos = 0u;
            c->state = ASX_HTTP_CONN_READING;
            continue;
        }
        if (c->close_after_write) {
            http_server_close(c);
            return ASX_OK;
        }
        if (c->drain_requested && !c->in_message && asx_http_parser_is_idle(&c->parser) &&
            c->parser.state != HTTP_PS_COMPLETE_PENDING) {
            http_server_close(c);
            return ASX_OK;
        }

        if (asx_buf_mut_remaining(&c->rx) > 0u || c->parser.state == HTTP_PS_COMPLETE_PENDING) {
            st = http_server_process_input(c);
            if (st != ASX_OK) {
                http_server_close(c);
                return st;
            }
            if (http_server_tx_pending(c) || c->close_after_write) continue;
            if (asx_buf_mut_remaining(&c->rx) > 0u) continue;
        }

        if (c->peer_eof) {
            asx_http_event ev;
            if (asx_http_parser_finish(&c->parser, &ev) == ASX_OK) {
                http_server_close(c);
                return ASX_OK;
            }
            /* Truncated request: answer 400 (the peer may have half-closed). */
            c->last_error = asx_http_parser_error(&c->parser);
            http_server_queue_error(c, ASX_HTTP_400_BAD_REQUEST);
            continue;
        }

        st = http_stream_read(c->stream, &c->rx);
        if (st == ASX_OK) continue;
        if (http_is_wait_status(st)) return ASX_E_PENDING;
        if (st == ASX_E_DISCONNECTED) {
            c->peer_eof = 1u;
            continue;
        }
        http_server_close(c);
        return st;
    }
    return ASX_E_PENDING;
}

asx_status asx_http_server_conn_poll(asx_http_server_conn *c) {
    asx_time deadline;
    asx_time now;
    asx_status st;

    if (c == NULL) return ASX_E_INVALID_ARGUMENT;
    if (c->state == ASX_HTTP_CONN_CLOSED) return ASX_OK;

    if (http_server_deadline(c, &deadline) && asx_runtime_now_ns(&now) == ASX_OK &&
        now >= deadline) {
        c->timed_out = 1u;
        http_server_close(c);
        return ASX_E_TIMED_OUT;
    }
    st = http_server_conn_step(c);
    if (st == ASX_E_PENDING && http_server_deadline(c, &deadline)) {
        /* Wake the polling task (if any) to enforce the deadline. */
        asx_status armed = asx_task_arm_timer(asx_task_current(), deadline);
        (void)armed;
    }
    return st;
}

void asx_http_server_conn_request_close(asx_http_server_conn *c) {
    if (c == NULL || c->state == ASX_HTTP_CONN_CLOSED) return;
    c->drain_requested = 1u;
    /* A final response already queued for a persistent exchange still
     * finishes, then the connection closes instead of reading another
     * request. (While a request is in flight - e.g. after 100 Continue -
     * dispatch sees drain_requested and answers with Connection: close.) */
    if (http_server_tx_pending(c) && !c->in_message) c->close_after_write = 1u;
}

int asx_http_server_conn_is_idle(const asx_http_server_conn *c) {
    if (c == NULL || c->state == ASX_HTTP_CONN_CLOSED) return 0;
    return !c->in_message && !http_server_tx_pending(c) && asx_http_parser_is_idle(&c->parser) &&
           asx_buf_mut_remaining(&c->rx) == 0u;
}

/* ------------------------------------------------------------------ */
/* HTTP server over asx_server                                         */
/* ------------------------------------------------------------------ */

asx_status asx_http_server_init(asx_http_server *hs, asx_server *srv,
                                const asx_http_server_config *cfg) {
    asx_status st;

    if (hs == NULL || srv == NULL || cfg == NULL || cfg->router == NULL) {
        return ASX_E_INVALID_ARGUMENT;
    }
    st = asx_surface_gate(ASX_SURFACE_SERVER);
    if (st != ASX_OK) return st;
    memset(hs, 0, sizeof(*hs));
    hs->server = srv;
    hs->config = *cfg;
    return ASX_OK;
}

/* Accepts per poll, so a burst of clients cannot monopolize one poll. */
#define HTTP_SERVER_ACCEPT_STEPS (ASX_HTTP_SERVER_MAX_CONNS + 8u)

/* Accept pending clients into free slots; with no slot (or the asx_server
 * at max_connections), accept and close them, so they fail fast. */
static void http_server_accept_pending(asx_http_server *hs) {
    uint32_t slot = 0u;
    uint32_t guard;

    for (guard = 0u; guard < HTTP_SERVER_ACCEPT_STEPS; guard++) {
        asx_server_conn conn;
        asx_status st;

        while (slot < ASX_HTTP_SERVER_MAX_CONNS && hs->conn_ids[slot] != 0u) slot++;
        st = slot < ASX_HTTP_SERVER_MAX_CONNS ? asx_server_poll_accept(hs->server, &conn)
                                              : ASX_E_RESOURCE_EXHAUSTED;
        if (st == ASX_E_RESOURCE_EXHAUSTED) {
            if (asx_server_reject_pending(hs->server) != ASX_OK) return;
            continue;
        }
        if (st != ASX_OK) return;
        if (asx_http_server_conn_init(&hs->conns[slot], conn.stream, &hs->config) != ASX_OK) {
            (void)asx_server_close_conn(hs->server, conn.id);
            return;
        }
        hs->conn_ids[slot] = conn.id;
    }
    /* More may be pending: poll again rather than wait for a new wake. */
    {
        asx_status woke = asx_task_wake(asx_task_current());
        (void)woke;
    }
}

/* At the drain deadline, close whatever is left; before it, wake the
 * polling task then. */
static void http_server_enforce_drain(asx_http_server *hs) {
    asx_time deadline;
    asx_time now;
    uint32_t slot;

    if (asx_server_drain_deadline(hs->server, &deadline) != ASX_OK) return;
    if (asx_http_server_active_conns(hs) == 0u) return;
    if (asx_runtime_now_ns(&now) != ASX_OK) return;
    if (now < deadline) {
        asx_status armed = asx_task_arm_timer(asx_task_current(), deadline);
        (void)armed;
        return;
    }
    for (slot = 0u; slot < ASX_HTTP_SERVER_MAX_CONNS; slot++) {
        if (hs->conn_ids[slot] == 0u || hs->conns[slot].state == ASX_HTTP_CONN_CLOSED) continue;
        http_server_close(&hs->conns[slot]);
        hs->connections_drain_closed++;
    }
}

asx_status asx_http_server_poll(asx_http_server *hs) {
    asx_server_state state;
    uint32_t slot;
    asx_status st;

    if (hs == NULL || hs->server == NULL) return ASX_E_INVALID_ARGUMENT;
    st = asx_surface_gate(ASX_SURFACE_SERVER);
    if (st != ASX_OK) return st;

    state = asx_server_get_state(hs->server);
    if (state == ASX_SERVER_STATE_LISTENING) http_server_accept_pending(hs);
    state = asx_server_get_state(hs->server);
    if (state == ASX_SERVER_STATE_DRAINING) http_server_enforce_drain(hs);

    for (slot = 0u; slot < ASX_HTTP_SERVER_MAX_CONNS; slot++) {
        asx_http_server_conn *c = &hs->conns[slot];

        if (hs->conn_ids[slot] == 0u) continue;
        if (state != ASX_SERVER_STATE_LISTENING) asx_http_server_conn_request_close(c);
        /* Per-connection transport failures close that connection only. */
        st = asx_http_server_conn_poll(c);
        if (st == ASX_E_TIMED_OUT) hs->connections_timed_out++;
        if (st != ASX_OK && st != ASX_E_PENDING) c->state = ASX_HTTP_CONN_CLOSED;
        if (c->state == ASX_HTTP_CONN_CLOSED) {
            hs->requests_completed_conns += c->requests_served;
            hs->connections_completed++;
            /* Accounting: releases the asx_server slot (and completes a drain). */
            (void)asx_server_close_conn(hs->server, hs->conn_ids[slot]);
            hs->conn_ids[slot] = 0u;
        }
    }

    if (asx_server_get_state(hs->server) == ASX_SERVER_STATE_STOPPED &&
        asx_http_server_active_conns(hs) == 0u) {
        return ASX_OK;
    }
    return ASX_E_PENDING;
}

uint32_t asx_http_server_active_conns(const asx_http_server *hs) {
    uint32_t slot;
    uint32_t count = 0u;

    if (hs == NULL) return 0u;
    for (slot = 0u; slot < ASX_HTTP_SERVER_MAX_CONNS; slot++) {
        if (hs->conn_ids[slot] != 0u) count++;
    }
    return count;
}

uint32_t asx_http_server_requests_served(const asx_http_server *hs) {
    uint32_t slot;
    uint32_t total;

    if (hs == NULL) return 0u;
    total = hs->requests_completed_conns;
    for (slot = 0u; slot < ASX_HTTP_SERVER_MAX_CONNS; slot++) {
        if (hs->conn_ids[slot] != 0u) total += hs->conns[slot].requests_served;
    }
    return total;
}
#endif /* ASX_HAS_SERVER_SURFACE */

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
