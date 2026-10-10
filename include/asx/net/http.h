/*
 * asx/net/http.h — HTTP request/response types, HTTP/1.1 wire protocol, and router
 *
 * Provides the public HTTP surface: method/status enums, header management,
 * body types, request/response builders, a `:param` router with middleware,
 * multipart/SSE helpers, a connection-pool bookkeeping model, and a real
 * RFC 9112 HTTP/1.1 wire protocol:
 *
 *   - asx_http_parser: sans-IO incremental parser for requests and responses
 *     (byte-at-a-time safe, streaming body events, chunked decoding,
 *     request-smuggling defenses, fail-closed limits),
 *   - asx_http_serialize_*: request/response head and body serializers with
 *     Content-Length or chunked framing,
 *   - asx_http_server_conn / asx_http_client_conn: keep-alive connection state
 *     machines driven over the asx_tcp_stream poll API (net.h),
 *   - asx_http_server: accept/drain wiring on top of asx_server (server.h).
 *
 * All storage is caller-owned and fixed-size; nothing allocates.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_NET_HTTP_H
#define ASX_NET_HTTP_H

#include <asx/asx_config.h>
#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <asx/bytes/buf.h>
#include <asx/net/net.h>
#include <asx/net/server.h>
#include <asx/security/security.h>
#include <asx/session/session.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * HTTP method
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_HTTP_GET = 0,
    ASX_HTTP_POST = 1,
    ASX_HTTP_PUT = 2,
    ASX_HTTP_DELETE = 3,
    ASX_HTTP_PATCH = 4,
    ASX_HTTP_HEAD = 5,
    ASX_HTTP_OPTIONS = 6,
    ASX_HTTP_CONNECT = 7,
    ASX_HTTP_TRACE = 8
} asx_http_method;

/* Return the method name as a string (e.g. "GET"). */
ASX_API const char *asx_http_method_str(asx_http_method method);

/* -------------------------------------------------------------------
 * HTTP status code
 * ------------------------------------------------------------------- */

typedef uint16_t asx_http_status;

/* Common status code constants. */
#define ASX_HTTP_100_CONTINUE 100
#define ASX_HTTP_101_SWITCHING_PROTOCOLS 101
#define ASX_HTTP_200_OK 200
#define ASX_HTTP_201_CREATED 201
#define ASX_HTTP_204_NO_CONTENT 204
#define ASX_HTTP_301_MOVED 301
#define ASX_HTTP_302_FOUND 302
#define ASX_HTTP_304_NOT_MODIFIED 304
#define ASX_HTTP_400_BAD_REQUEST 400
#define ASX_HTTP_401_UNAUTHORIZED 401
#define ASX_HTTP_403_FORBIDDEN 403
#define ASX_HTTP_404_NOT_FOUND 404
#define ASX_HTTP_405_METHOD_NOT_ALLOWED 405
#define ASX_HTTP_408_REQUEST_TIMEOUT 408
#define ASX_HTTP_413_CONTENT_TOO_LARGE 413
#define ASX_HTTP_414_URI_TOO_LONG 414
#define ASX_HTTP_417_EXPECTATION_FAILED 417
#define ASX_HTTP_429_TOO_MANY_REQUESTS 429
#define ASX_HTTP_431_HEADER_FIELDS_TOO_LARGE 431
#define ASX_HTTP_500_INTERNAL_ERROR 500
#define ASX_HTTP_501_NOT_IMPLEMENTED 501
#define ASX_HTTP_502_BAD_GATEWAY 502
#define ASX_HTTP_503_SERVICE_UNAVAILABLE 503
#define ASX_HTTP_504_GATEWAY_TIMEOUT 504
#define ASX_HTTP_505_VERSION_NOT_SUPPORTED 505

/* Return the RFC 9110 reason phrase for a status code ("Unknown" if unregistered). */
ASX_API const char *asx_http_status_reason(asx_http_status code);

/* Check if status is informational (1xx). */
ASX_API int asx_http_status_is_info(asx_http_status code);

/* Check if status is success (2xx). */
ASX_API int asx_http_status_is_success(asx_http_status code);

/* Check if status is redirect (3xx). */
ASX_API int asx_http_status_is_redirect(asx_http_status code);

/* Check if status is client error (4xx). */
ASX_API int asx_http_status_is_client_error(asx_http_status code);

/* Check if status is server error (5xx). */
ASX_API int asx_http_status_is_server_error(asx_http_status code);

/* -------------------------------------------------------------------
 * HTTP version
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_HTTP_VERSION_1_0 = 0,
    ASX_HTTP_VERSION_1_1 = 1,
    ASX_HTTP_VERSION_2 = 2,
    ASX_HTTP_VERSION_3 = 3
} asx_http_version;

/* Return the version string (e.g. "HTTP/1.1"). */
ASX_API const char *asx_http_version_str(asx_http_version ver);

/* -------------------------------------------------------------------
 * HTTP headers
 * ------------------------------------------------------------------- */

#ifndef ASX_HTTP_MAX_HEADERS
#define ASX_HTTP_MAX_HEADERS 16u
#endif

#ifndef ASX_HTTP_HEADER_NAME_MAX
#define ASX_HTTP_HEADER_NAME_MAX 64u
#endif

#ifndef ASX_HTTP_HEADER_VALUE_MAX
#define ASX_HTTP_HEADER_VALUE_MAX 256u
#endif

typedef struct {
    char name[ASX_HTTP_HEADER_NAME_MAX];
    char value[ASX_HTTP_HEADER_VALUE_MAX];
} asx_http_header;

typedef struct {
    asx_http_header entries[ASX_HTTP_MAX_HEADERS];
    uint32_t count;
} asx_http_headers;

/* Initialize empty header set. */
ASX_API void asx_http_headers_init(asx_http_headers *hdrs);

/* Add a header (appends; does not deduplicate). */
ASX_API asx_status asx_http_headers_add(asx_http_headers *hdrs, const char *name,
                                        const char *value);

/* Find first header by name (case-insensitive). Returns NULL if not found. */
ASX_API const char *asx_http_headers_get(const asx_http_headers *hdrs, const char *name);

/* Remove all headers with the given name. Returns count removed. */
ASX_API uint32_t asx_http_headers_remove(asx_http_headers *hdrs, const char *name);

/* Return 1 if a comma-separated field value contains `token` (case-insensitive,
 * OWS-trimmed list members, RFC 9110 section 5.6.1), else 0. */
ASX_API int asx_http_header_has_token(const char *value, const char *token);

/* Return 1 if any header line named `name` carries `token` in its list value. */
ASX_API int asx_http_headers_has_token(const asx_http_headers *hdrs, const char *name,
                                       const char *token);

/* Decide connection persistence (RFC 9112 section 9.3) from the Connection
 * header lines and protocol version: "close" always wins; HTTP/1.1 defaults to
 * persistent; HTTP/1.0 is persistent only with "keep-alive". Returns 1 to keep
 * the connection open, 0 to close it. */
ASX_API int asx_http_headers_keep_alive(const asx_http_headers *hdrs, asx_http_version version);

/* -------------------------------------------------------------------
 * HTTP body
 * ------------------------------------------------------------------- */

#ifndef ASX_HTTP_BODY_MAX
#define ASX_HTTP_BODY_MAX 4096u
#endif

typedef enum {
    ASX_HTTP_BODY_EMPTY = 0,
    ASX_HTTP_BODY_BYTES = 1,
    ASX_HTTP_BODY_STREAM = 2
} asx_http_body_kind;

typedef struct {
    asx_http_body_kind kind;
    uint8_t data[ASX_HTTP_BODY_MAX];
    uint32_t len;
    uint32_t content_length;
} asx_http_body;

/* Initialize an empty body. */
ASX_API void asx_http_body_init(asx_http_body *body);

/* Set body from bytes. */
ASX_API asx_status asx_http_body_set_bytes(asx_http_body *body, const void *data, uint32_t len);

/* Get body content length. */
ASX_API uint32_t asx_http_body_len(const asx_http_body *body);

/* Check if body is empty. */
ASX_API int asx_http_body_is_empty(const asx_http_body *body);

/* Append bytes to an inline body (the small-body collection path).
 * Fails closed with ASX_E_BUFFER_TOO_SMALL, leaving the body unchanged, when the
 * result would exceed ASX_HTTP_BODY_MAX. */
ASX_API ASX_MUST_USE asx_status asx_http_body_append(asx_http_body *body, const void *data,
                                                     uint32_t len);

/* -------------------------------------------------------------------
 * HTTP request
 * ------------------------------------------------------------------- */

#ifndef ASX_HTTP_URI_MAX
#define ASX_HTTP_URI_MAX 256u
#endif

typedef struct {
    asx_http_method method;
    char uri[ASX_HTTP_URI_MAX];
    asx_http_version version;
    asx_http_headers headers;
    asx_http_body body;
} asx_http_request;

/* Initialize a request. */
ASX_API void asx_http_request_init(asx_http_request *req, asx_http_method method, const char *uri);

/* -------------------------------------------------------------------
 * HTTP response
 * ------------------------------------------------------------------- */

typedef struct {
    asx_http_status status;
    asx_http_version version;
    asx_http_headers headers;
    asx_http_body body;
} asx_http_response;

/* Initialize a response. */
ASX_API void asx_http_response_init(asx_http_response *resp, asx_http_status status);

/* -------------------------------------------------------------------
 * Web router, middleware, extractors, sessions, static, SSE, multipart
 * ------------------------------------------------------------------- */

#ifndef ASX_HTTP_MAX_ROUTES
#define ASX_HTTP_MAX_ROUTES 16u
#endif

#ifndef ASX_HTTP_MAX_ROUTE_MIDDLEWARE
#define ASX_HTTP_MAX_ROUTE_MIDDLEWARE 8u
#endif

#ifndef ASX_HTTP_MAX_PATH_PARAMS
#define ASX_HTTP_MAX_PATH_PARAMS 8u
#endif

#ifndef ASX_HTTP_PARAM_NAME_MAX
#define ASX_HTTP_PARAM_NAME_MAX 32u
#endif

#ifndef ASX_HTTP_PARAM_VALUE_MAX
#define ASX_HTTP_PARAM_VALUE_MAX 128u
#endif

#ifndef ASX_HTTP_COOKIE_VALUE_MAX
#define ASX_HTTP_COOKIE_VALUE_MAX 128u
#endif

#ifndef ASX_HTTP_SECURITY_PURPOSE_MAX
#define ASX_HTTP_SECURITY_PURPOSE_MAX 32u
#endif

#ifndef ASX_HTTP_MULTIPART_PARTS_MAX
#define ASX_HTTP_MULTIPART_PARTS_MAX 8u
#endif

#ifndef ASX_HTTP_MULTIPART_DATA_MAX
#define ASX_HTTP_MULTIPART_DATA_MAX 512u
#endif

#ifndef ASX_HTTP_MULTIPART_FILENAME_MAX
#define ASX_HTTP_MULTIPART_FILENAME_MAX 64u
#endif

typedef struct {
    char name[ASX_HTTP_PARAM_NAME_MAX];
    char value[ASX_HTTP_PARAM_VALUE_MAX];
} asx_http_path_param;

typedef struct {
    asx_http_path_param entries[ASX_HTTP_MAX_PATH_PARAMS];
    uint32_t count;
} asx_http_path_params;

typedef struct {
    uint8_t require_session;
    uint8_t require_security;
    char security_purpose[ASX_HTTP_SECURITY_PURPOSE_MAX];
} asx_http_route_policy;

typedef struct asx_http_request_context asx_http_request_context;

typedef enum {
    ASX_HTTP_MIDDLEWARE_CONTINUE = 0,
    ASX_HTTP_MIDDLEWARE_RESPOND = 1
} asx_http_middleware_result;

typedef asx_status (*asx_http_handler_fn)(asx_http_request_context *ctx, asx_http_response *resp,
                                          void *user_data);
typedef asx_status (*asx_http_middleware_fn)(asx_http_request_context *ctx, asx_http_response *resp,
                                             void *user_data,
                                             asx_http_middleware_result *out_result);

struct asx_http_request_context {
    const asx_http_request *request;
    asx_http_path_params path_params;
    asx_session_pair *session;
    asx_security_context *security;
    uint8_t security_verified;
    char route_pattern[ASX_HTTP_URI_MAX];
};

typedef struct {
    asx_http_middleware_fn fn;
    void *user_data;
} asx_http_middleware_entry;

typedef struct {
    asx_http_method method;
    char pattern[ASX_HTTP_URI_MAX];
    asx_http_handler_fn handler;
    void *handler_user_data;
    asx_http_middleware_entry middleware[ASX_HTTP_MAX_ROUTE_MIDDLEWARE];
    uint32_t middleware_count;
    asx_http_route_policy policy;
} asx_http_route;

typedef struct {
    asx_http_route routes[ASX_HTTP_MAX_ROUTES];
    uint32_t count;
} asx_http_router;

typedef struct {
    char name[ASX_HTTP_PARAM_NAME_MAX];
    char filename[ASX_HTTP_MULTIPART_FILENAME_MAX];
    char content_type[ASX_HTTP_HEADER_VALUE_MAX];
    uint8_t data[ASX_HTTP_MULTIPART_DATA_MAX];
    uint32_t data_len;
} asx_http_multipart_part;

typedef struct {
    asx_http_multipart_part parts[ASX_HTTP_MULTIPART_PARTS_MAX];
    uint32_t count;
} asx_http_multipart_form;

/* Initialize a route policy to defaults (no session or security required). */
ASX_API void asx_http_route_policy_init(asx_http_route_policy *policy);
/* Initialize an empty HTTP router. */
ASX_API void asx_http_router_init(asx_http_router *router);
/* Add a route with handler, policy, and optional middleware slot. */
ASX_API asx_status asx_http_router_add_route(asx_http_router *router, asx_http_method method,
                                             const char *pattern, asx_http_handler_fn handler,
                                             void *handler_user_data,
                                             const asx_http_route_policy *policy,
                                             asx_http_route **out_route);
/* Attach a middleware function to a route. */
ASX_API asx_status asx_http_route_add_middleware(asx_http_route *route, asx_http_middleware_fn fn,
                                                 void *user_data);
/* Dispatch an incoming request through the router, matching routes and running middleware. */
ASX_API asx_status asx_http_router_dispatch(asx_http_router *router, const asx_http_request *req,
                                            asx_http_response *resp, asx_session_pair *session,
                                            asx_security_context *security,
                                            asx_http_request_context *out_ctx);

/* Extract a named path parameter from the request context. */
ASX_API asx_status asx_http_request_path_param(const asx_http_request_context *ctx,
                                               const char *name, char *out, uint32_t out_size);
/* Extract a named query parameter from the request URI. */
ASX_API asx_status asx_http_request_query_param(const asx_http_request *req, const char *name,
                                                char *out, uint32_t out_size);
/* Extract a named cookie value from the request headers. */
ASX_API asx_status asx_http_request_cookie(const asx_http_request *req, const char *name, char *out,
                                           uint32_t out_size);
/* Verify the request body against the security context for the given purpose. */
ASX_API asx_status asx_http_request_verify_body_auth(const asx_http_request *req,
                                                     asx_security_context *security,
                                                     const char *purpose, int *out_verified);

/* Set a session cookie on the response with secure/httponly flags. */
ASX_API asx_status asx_http_response_set_session_cookie(asx_http_response *resp, const char *name,
                                                        const char *value, int secure,
                                                        int http_only);
/* Set the response as a Server-Sent Event with event type, id, and data. */
ASX_API asx_status asx_http_response_set_sse(asx_http_response *resp, const char *event,
                                             const char *id, const char *data);
#if ASX_HAS_NATIVE_RUNTIME_SURFACES
/* Serve a static file from the given root directory matching the URI path. */
ASX_API asx_status asx_http_serve_static(asx_http_response *resp, const char *root,
                                         const char *uri);
#endif
/* Parse multipart form data from a request body. */
ASX_API asx_status asx_http_parse_multipart(const asx_http_request *req,
                                            asx_http_multipart_form *out_form);

/* ===================================================================
 * HTTP/1.1 wire protocol (RFC 9112)
 *
 * Line-ending policy: HTTP/1.x lines MUST end in CRLF. A bare LF or a bare
 * CR anywhere in the start line, header section, chunk-size line, chunk data
 * terminator, or trailer section is rejected (ASX_HTTP_PERR_BAD_LINE_ENDING,
 * 400). The parser never "tolerates" LF-only framing because a peer that
 * splits lines differently can be used for request smuggling.
 *
 * obs-fold (a header line starting with SP/HTAB, including whitespace
 * between the start line and the first header) is rejected with 400 for
 * requests and responses alike.
 * =================================================================== */

/* Maximum number of header + trailer fields one parser instance can index. */
#ifndef ASX_HTTP_PARSER_MAX_FIELDS
#define ASX_HTTP_PARSER_MAX_FIELDS 64u
#endif

/* Default parser limits (see asx_http_limits_init). */
#ifndef ASX_HTTP_DEFAULT_MAX_START_LINE
#define ASX_HTTP_DEFAULT_MAX_START_LINE 8192u
#endif

#ifndef ASX_HTTP_DEFAULT_MAX_HEADER_BYTES
#define ASX_HTTP_DEFAULT_MAX_HEADER_BYTES 16384u
#endif

#ifndef ASX_HTTP_DEFAULT_MAX_CHUNK_LINE
#define ASX_HTTP_DEFAULT_MAX_CHUNK_LINE 1024u
#endif

#ifndef ASX_HTTP_DEFAULT_MAX_BODY
#define ASX_HTTP_DEFAULT_MAX_BODY 16777216u /* 16 MiB */
#endif

/* Leading empty lines tolerated before a start line (RFC 9112 section 2.2). */
#ifndef ASX_HTTP_MAX_LEADING_EMPTY_LINES
#define ASX_HTTP_MAX_LEADING_EMPTY_LINES 4u
#endif

/* Configurable fail-closed parser limits. */
typedef struct {
    uint32_t max_start_line;   /* request-line / status-line bytes, excluding CRLF */
    uint32_t max_header_count; /* header fields (and, separately, trailer fields) */
    uint32_t max_header_bytes; /* header-section bytes incl. CRLFs (trailers separately) */
    uint32_t max_chunk_line;   /* chunk-size line incl. extensions, excluding CRLF */
    uint64_t max_body;         /* decoded body bytes */
    uint8_t require_host;      /* 1: HTTP/1.1 requests MUST carry exactly one Host */
} asx_http_limits;

/* Initialize limits to the documented defaults (8 KiB start line, 64 fields,
 * 16 KiB header section, 1 KiB chunk line, 16 MiB body, Host required). */
ASX_API void asx_http_limits_init(asx_http_limits *limits);

/* Which message kind a parser decodes. */
typedef enum { ASX_HTTP_PARSE_REQUEST = 0, ASX_HTTP_PARSE_RESPONSE = 1 } asx_http_parse_kind;

/* Detailed parse failure reasons. Each maps to a distinct HTTP status via
 * asx_http_parse_error_status(). */
typedef enum {
    ASX_HTTP_PERR_NONE = 0,
    ASX_HTTP_PERR_BAD_START_LINE = 1,               /* 400 */
    ASX_HTTP_PERR_UNKNOWN_METHOD = 2,               /* 501: valid token, unsupported method */
    ASX_HTTP_PERR_BAD_TARGET = 3,                   /* 400 */
    ASX_HTTP_PERR_BAD_VERSION = 4,                  /* 400: malformed HTTP-version */
    ASX_HTTP_PERR_UNSUPPORTED_VERSION = 5,          /* 505: well-formed but not 1.0/1.1 */
    ASX_HTTP_PERR_BAD_STATUS = 6,                   /* 400: malformed status-line */
    ASX_HTTP_PERR_START_LINE_TOO_LONG = 7,          /* 414 */
    ASX_HTTP_PERR_BAD_LINE_ENDING = 8,              /* 400: bare CR or bare LF */
    ASX_HTTP_PERR_OBS_FOLD = 9,                     /* 400 */
    ASX_HTTP_PERR_BAD_HEADER_NAME = 10,             /* 400 */
    ASX_HTTP_PERR_BAD_HEADER_VALUE = 11,            /* 400 */
    ASX_HTTP_PERR_TOO_MANY_HEADERS = 12,            /* 431 */
    ASX_HTTP_PERR_HEADERS_TOO_LARGE = 13,           /* 431 */
    ASX_HTTP_PERR_MISSING_HOST = 14,                /* 400 */
    ASX_HTTP_PERR_DUPLICATE_HOST = 15,              /* 400 */
    ASX_HTTP_PERR_BAD_CONTENT_LENGTH = 16,          /* 400 */
    ASX_HTTP_PERR_DUPLICATE_CONTENT_LENGTH = 17,    /* 400 */
    ASX_HTTP_PERR_CONTENT_LENGTH_AND_CHUNKED = 18,  /* 400: CL + TE together */
    ASX_HTTP_PERR_BAD_TRANSFER_ENCODING = 19,       /* 400 */
    ASX_HTTP_PERR_UNSUPPORTED_TRANSFER_CODING = 20, /* 501 */
    ASX_HTTP_PERR_BAD_CHUNK_SIZE = 21,              /* 400 */
    ASX_HTTP_PERR_CHUNK_SIZE_OVERFLOW = 22,         /* 400 */
    ASX_HTTP_PERR_BAD_CHUNK_EXTENSION = 23,         /* 400 */
    ASX_HTTP_PERR_CHUNK_LINE_TOO_LONG = 24,         /* 400 */
    ASX_HTTP_PERR_BAD_CHUNK_TERMINATOR = 25,        /* 400 */
    ASX_HTTP_PERR_BAD_TRAILER = 26,                 /* 400: malformed or forbidden trailer */
    ASX_HTTP_PERR_BODY_TOO_LARGE = 27,              /* 413 */
    ASX_HTTP_PERR_UNEXPECTED_EOF = 28               /* 400: peer closed mid-message */
} asx_http_parse_error;

/* Return the HTTP status a server should answer with for a parse error
 * (400/413/414/431/501/505), or 0 for ASX_HTTP_PERR_NONE. */
ASX_API asx_http_status asx_http_parse_error_status(asx_http_parse_error err);

/* Return a stable human-readable name for a parse error. Never NULL. */
ASX_API const char *asx_http_parse_error_str(asx_http_parse_error err);

/* How the message body is delimited (RFC 9112 section 6.3). */
typedef enum {
    ASX_HTTP_BODY_FRAMING_NONE = 0,    /* no body */
    ASX_HTTP_BODY_FRAMING_LENGTH = 1,  /* Content-Length */
    ASX_HTTP_BODY_FRAMING_CHUNKED = 2, /* Transfer-Encoding: chunked */
    ASX_HTTP_BODY_FRAMING_CLOSE = 3    /* response delimited by connection close */
} asx_http_body_framing;

/* Parser events returned by asx_http_parser_feed(). */
typedef enum {
    ASX_HTTP_EVENT_NONE = 0,            /* all input consumed; feed more bytes */
    ASX_HTTP_EVENT_HEAD = 1,            /* start line + header section complete */
    ASX_HTTP_EVENT_BODY = 2,            /* decoded body bytes in event.data/len */
    ASX_HTTP_EVENT_MESSAGE_COMPLETE = 3 /* body (and trailers) complete */
} asx_http_event_kind;

typedef struct {
    asx_http_event_kind kind;
    const uint8_t *data; /* BODY: points into the caller's fed input (zero-copy) */
    uint32_t len;
} asx_http_event;

/* Offsets of one stored field inside the parser's head buffer. */
typedef struct {
    uint32_t name_off;
    uint32_t value_off;
} asx_http_field_span;

/* Incremental HTTP/1.1 parser. Treat all fields as private; use accessors.
 * Header/trailer names and values are stored NUL-terminated inside the
 * caller-provided head buffer and stay valid until the next message starts
 * (the first byte fed after MESSAGE_COMPLETE) or asx_http_parser_reset(). */
typedef struct {
    asx_http_parse_kind kind;
    asx_http_limits limits;
    uint8_t *head_buf;
    uint32_t head_cap;
    uint32_t head_len;
    uint32_t line_start;
    uint32_t line_len;
    uint32_t section_bytes;
    uint32_t empty_lines;
    asx_http_field_span fields[ASX_HTTP_PARSER_MAX_FIELDS];
    uint32_t header_count;
    uint32_t trailer_count;
    uint64_t content_length;
    uint64_t body_remaining;
    uint64_t body_received;
    uint64_t chunk_size;
    uint32_t chunk_line_len;
    uint32_t messages_completed;
    asx_http_parse_error error;
    asx_http_method method;
    asx_http_method response_to; /* request method the next response answers */
    asx_http_version version;
    asx_http_status status;
    asx_http_body_framing framing;
    uint8_t state;
    uint8_t cr_seen;
    uint8_t ext_state;
    uint8_t chunk_digits;
    uint8_t head_done;
    uint8_t keep_alive;
    uint8_t expect_continue;
    uint8_t upgrade;
    uint8_t eof;
} asx_http_parser;

/* Initialize a parser. `head_buf`/`head_cap` is caller-owned scratch that
 * stores the start line, header fields, and trailers (bounded: overflow fails
 * closed with 414/431). `limits` may be NULL for defaults. */
ASX_API ASX_MUST_USE asx_status asx_http_parser_init(asx_http_parser *p, asx_http_parse_kind kind,
                                                     const asx_http_limits *limits,
                                                     uint8_t *head_buf, uint32_t head_cap);

/* Discard all message state (keeps kind, limits, buffer, response_to); the
 * parser becomes ready for a fresh message and clears any sticky error. */
ASX_API void asx_http_parser_reset(asx_http_parser *p);

/* Tell a response parser which request method the next response answers
 * (HEAD responses carry no body; 2xx to CONNECT switches to a tunnel). */
ASX_API void asx_http_parser_set_request_method(asx_http_parser *p, asx_http_method method);

/* Feed bytes. Parses until one event occurs or input is exhausted; `*consumed`
 * reports how many bytes were used. Call repeatedly (also with zero remaining
 * bytes) until the event is ASX_HTTP_EVENT_NONE. Byte-at-a-time feeding is
 * fully supported. BODY event data points into `data`.
 * Errors are sticky: ASX_E_INVALID_ARGUMENT (protocol violation),
 * ASX_E_RESOURCE_EXHAUSTED (limit exceeded), ASX_E_INVALID_STATE (fed after
 * EOF or after a protocol upgrade); asx_http_parser_error() gives details. */
ASX_API ASX_MUST_USE asx_status asx_http_parser_feed(asx_http_parser *p, const void *data,
                                                     uint32_t len, uint32_t *consumed,
                                                     asx_http_event *ev);

/* Signal end of input (peer closed). Completes a close-delimited response
 * body (event MESSAGE_COMPLETE); between messages it is a clean close (event
 * NONE). EOF inside any other message state returns ASX_E_DISCONNECTED with
 * ASX_HTTP_PERR_UNEXPECTED_EOF. */
ASX_API ASX_MUST_USE asx_status asx_http_parser_finish(asx_http_parser *p, asx_http_event *ev);

/* Return 1 when the parser sits between messages with no partial bytes. */
ASX_API int asx_http_parser_is_idle(const asx_http_parser *p);

/* Return 1 after a 101 response or 2xx-to-CONNECT completed (bytes that
 * follow belong to another protocol and are not consumed). */
ASX_API int asx_http_parser_is_upgraded(const asx_http_parser *p);

/* Return the detailed error after a failed feed/finish (NONE otherwise). */
ASX_API asx_http_parse_error asx_http_parser_error(const asx_http_parser *p);

/* Return 1 once the current message head has been parsed (until the next message starts). */
ASX_API int asx_http_parser_head_complete(const asx_http_parser *p);

/* Return the parsed request method (requests only). */
ASX_API asx_http_method asx_http_parser_method(const asx_http_parser *p);

/* Return the parsed request-target as a NUL-terminated string ("" before HEAD). */
ASX_API const char *asx_http_parser_target(const asx_http_parser *p);

/* Return the parsed HTTP version of the current message. */
ASX_API asx_http_version asx_http_parser_version(const asx_http_parser *p);

/* Return the parsed status code (responses only; 0 before HEAD). */
ASX_API asx_http_status asx_http_parser_status(const asx_http_parser *p);

/* Return the parsed reason phrase (responses only; "" before HEAD). */
ASX_API const char *asx_http_parser_reason(const asx_http_parser *p);

/* Return the number of parsed header fields of the current message. */
ASX_API uint32_t asx_http_parser_header_count(const asx_http_parser *p);

/* Fetch header field `index` (wire order). Returns ASX_E_NOT_FOUND if out of range. */
ASX_API ASX_MUST_USE asx_status asx_http_parser_header_at(const asx_http_parser *p, uint32_t index,
                                                          const char **out_name,
                                                          const char **out_value);

/* Return the first header value named `name` (case-insensitive) or NULL. */
ASX_API const char *asx_http_parser_header(const asx_http_parser *p, const char *name);

/* Return the number of parsed trailer fields (chunked bodies only). */
ASX_API uint32_t asx_http_parser_trailer_count(const asx_http_parser *p);

/* Fetch trailer field `index`. Returns ASX_E_NOT_FOUND if out of range. */
ASX_API ASX_MUST_USE asx_status asx_http_parser_trailer_at(const asx_http_parser *p, uint32_t index,
                                                           const char **out_name,
                                                           const char **out_value);

/* Return the body framing chosen for the current message. */
ASX_API asx_http_body_framing asx_http_parser_body_framing(const asx_http_parser *p);

/* Return the declared Content-Length (0 if absent). */
ASX_API uint64_t asx_http_parser_content_length(const asx_http_parser *p);

/* Return the number of decoded body bytes delivered so far for this message. */
ASX_API uint64_t asx_http_parser_body_received(const asx_http_parser *p);

/* Return 1 if the connection may persist after this message (RFC 9112 section 9.3). */
ASX_API int asx_http_parser_keep_alive(const asx_http_parser *p);

/* Return 1 if an HTTP/1.1 request carried Expect: 100-continue. */
ASX_API int asx_http_parser_expect_continue(const asx_http_parser *p);

/* Return the number of messages completed by this parser. */
ASX_API uint32_t asx_http_parser_messages_completed(const asx_http_parser *p);

/* Copy the parsed request head (method, target, version, headers) into `req`
 * and reset its body. Fails closed: ASX_E_BUFFER_TOO_SMALL when the target or
 * a field exceeds the struct capacities, ASX_E_RESOURCE_EXHAUSTED when there
 * are more than ASX_HTTP_MAX_HEADERS fields, ASX_E_INVALID_STATE before HEAD. */
ASX_API ASX_MUST_USE asx_status asx_http_parser_copy_request(const asx_http_parser *p,
                                                             asx_http_request *req);

/* Copy the parsed response head (status, version, headers) into `resp` and
 * reset its body. Same failure contract as asx_http_parser_copy_request. */
ASX_API ASX_MUST_USE asx_status asx_http_parser_copy_response(const asx_http_parser *p,
                                                              asx_http_response *resp);

/* Convenience path: feed bytes and collect one request (head + inline body)
 * into `req`. Sets *out_complete=1 once the full message is collected (the
 * remaining input, e.g. a pipelined request, is left unconsumed). Head or
 * body overflow of the struct capacities poisons the parser with
 * START_LINE_TOO_LONG / HEADERS_TOO_LARGE / BODY_TOO_LARGE. */
ASX_API ASX_MUST_USE asx_status asx_http_parser_collect_request(asx_http_parser *p,
                                                                const void *data, uint32_t len,
                                                                uint32_t *consumed,
                                                                asx_http_request *req,
                                                                int *out_complete);

/* Convenience path for responses: like asx_http_parser_collect_request but
 * fills `resp`. Interim 1xx responses (other than 101) are skipped. */
ASX_API ASX_MUST_USE asx_status asx_http_parser_collect_response(asx_http_parser *p,
                                                                 const void *data, uint32_t len,
                                                                 uint32_t *consumed,
                                                                 asx_http_response *resp,
                                                                 int *out_complete);

/* -------------------------------------------------------------------
 * HTTP/1.1 serializer
 * ------------------------------------------------------------------- */

/* Body framing requested from the serializer. */
typedef enum {
    ASX_HTTP_FRAMING_AUTO = 0,    /* Content-Length from the inline body */
    ASX_HTTP_FRAMING_LENGTH = 1,  /* Content-Length (head-only calls use opts.content_length) */
    ASX_HTTP_FRAMING_CHUNKED = 2, /* Transfer-Encoding: chunked (HTTP/1.1 only) */
    ASX_HTTP_FRAMING_CLOSE = 3,   /* responses only: delimited by closing the connection */
    ASX_HTTP_FRAMING_NONE = 4     /* emit no framing headers and no body */
} asx_http_framing;

/* Connection header directive emitted by the serializer. */
typedef enum {
    ASX_HTTP_CONNECTION_DEFAULT = 0,   /* keep caller-supplied Connection fields */
    ASX_HTTP_CONNECTION_CLOSE = 1,     /* replace with "Connection: close" */
    ASX_HTTP_CONNECTION_KEEP_ALIVE = 2 /* replace with "Connection: keep-alive" */
} asx_http_connection_directive;

typedef struct {
    asx_http_framing framing;
    uint64_t content_length; /* LENGTH framing for head-only serialization */
    asx_http_connection_directive connection;
    uint8_t emit_date;     /* add Date from the runtime clock hook (skipped if no hook) */
    uint8_t head_response; /* response to HEAD: framing headers but no body bytes */
    const char *host;      /* requests: Host value to emit when headers lack one */
    const asx_http_headers *trailers; /* chunked full-message: trailer fields */
} asx_http_serialize_opts;

/* Initialize serializer options (AUTO framing, default Connection, no Date). */
ASX_API void asx_http_serialize_opts_init(asx_http_serialize_opts *opts);

/* Serialize a request head ("METHOD target HTTP/1.x", fields, framing, CRLF).
 * The serializer owns framing: caller Content-Length / Transfer-Encoding
 * fields are dropped and regenerated. Field names must be tokens and values
 * must not contain CR/LF/NUL/CTL (response-splitting defense), otherwise
 * ASX_E_INVALID_ARGUMENT. HTTP/1.1 requests without Host (and no opts.host)
 * fail with ASX_E_INVALID_ARGUMENT. ASX_E_BUFFER_TOO_SMALL if `cap` is short;
 * `*out_len` is 0 on any failure. `opts` may be NULL. */
ASX_API ASX_MUST_USE asx_status asx_http_serialize_request_head(const asx_http_request *req,
                                                                const asx_http_serialize_opts *opts,
                                                                uint8_t *out, uint32_t cap,
                                                                uint32_t *out_len);

/* Serialize a complete request: head plus the inline body framed per opts
 * (chunked bodies are emitted as one chunk plus the last-chunk/trailers). */
ASX_API ASX_MUST_USE asx_status asx_http_serialize_request(const asx_http_request *req,
                                                           const asx_http_serialize_opts *opts,
                                                           uint8_t *out, uint32_t cap,
                                                           uint32_t *out_len);

/* Serialize a response head ("HTTP/1.x code reason", fields, framing, CRLF).
 * 1xx/204/304 never carry framing headers. Same validation contract as the
 * request serializer; status must be 100..599. */
ASX_API ASX_MUST_USE asx_status
asx_http_serialize_response_head(const asx_http_response *resp, const asx_http_serialize_opts *opts,
                                 uint8_t *out, uint32_t cap, uint32_t *out_len);

/* Serialize a complete response (head + framed inline body; no body bytes
 * for HEAD responses). A non-empty body on 1xx/204/304 is rejected. */
ASX_API ASX_MUST_USE asx_status asx_http_serialize_response(const asx_http_response *resp,
                                                            const asx_http_serialize_opts *opts,
                                                            uint8_t *out, uint32_t cap,
                                                            uint32_t *out_len);

/* Encode one chunk ("<hex-size>CRLF<data>CRLF"). A zero-length chunk is
 * rejected (it would terminate the body; use asx_http_encode_last_chunk). */
ASX_API ASX_MUST_USE asx_status asx_http_encode_chunk(const void *data, uint32_t len, uint8_t *out,
                                                      uint32_t cap, uint32_t *out_len);

/* Encode the last chunk "0CRLF", optional trailer fields, and the final CRLF.
 * Trailers that RFC 9110 section 6.5.1 forbids (framing, routing, auth, ...)
 * are rejected with ASX_E_INVALID_ARGUMENT. `trailers` may be NULL. */
ASX_API ASX_MUST_USE asx_status asx_http_encode_last_chunk(const asx_http_headers *trailers,
                                                           uint8_t *out, uint32_t cap,
                                                           uint32_t *out_len);

/* Length of an IMF-fixdate string ("Sun, 06 Nov 1994 08:49:37 GMT"). */
#define ASX_HTTP_DATE_LEN 29u

/* Format Unix seconds as an IMF-fixdate (RFC 9110 section 5.6.7) into `out`
 * (needs ASX_HTTP_DATE_LEN + 1 bytes). Years after 9999 are rejected. */
ASX_API ASX_MUST_USE asx_status asx_http_format_date(uint64_t unix_seconds, char *out,
                                                     uint32_t out_size);

/* -------------------------------------------------------------------
 * HTTP/1.1 connections over asx_tcp_stream
 *
 * EOF detection: a poll_read returning ASX_OK with zero bytes or
 * ASX_E_DISCONNECTED is treated as the peer closing. The deterministic
 * in-memory transport never reports EOF (reads stay ASX_E_PENDING), so
 * close-delimited responses only complete there via
 * asx_http_client_conn_notify_eof().
 * ------------------------------------------------------------------- */

/* Per-connection head scratch (start line + fields + trailers). */
#ifndef ASX_HTTP_CONN_HEAD_MAX
#define ASX_HTTP_CONN_HEAD_MAX 8192u
#endif

/* Serialized head staging capacity (bodies are written zero-copy). */
#ifndef ASX_HTTP_CONN_TX_MAX
#define ASX_HTTP_CONN_TX_MAX                                                                       \
    ((ASX_HTTP_MAX_HEADERS * (ASX_HTTP_HEADER_NAME_MAX + ASX_HTTP_HEADER_VALUE_MAX + 4u)) +        \
     ASX_HTTP_URI_MAX + 512u)
#endif

/* Client transmit staging: request head + one inline body + chunk framing. */
#ifndef ASX_HTTP_CLIENT_TX_MAX
#define ASX_HTTP_CLIENT_TX_MAX (ASX_HTTP_CONN_TX_MAX + ASX_HTTP_BODY_MAX + 64u)
#endif

/* Maximum interim (1xx) responses a client skips before failing closed. */
#ifndef ASX_HTTP_CLIENT_MAX_INFORMATIONAL
#define ASX_HTTP_CLIENT_MAX_INFORMATIONAL 8u
#endif

/* Streaming body sink: receives decoded body bytes instead of the inline
 * body. Returning non-OK aborts the message (server answers 500 and closes). */
typedef asx_status (*asx_http_body_sink_fn)(void *ctx, const uint8_t *data, uint32_t len);

/* Lifecycle of a connection state machine. */
typedef enum {
    ASX_HTTP_CONN_READING = 0, /* waiting for / parsing a message */
    ASX_HTTP_CONN_WRITING = 1, /* flushing serialized bytes */
    ASX_HTTP_CONN_CLOSED = 2   /* stream closed; connection finished */
} asx_http_conn_state;

/* Client connection: write one request at a time, parse the response
 * (Content-Length, chunked, or close-delimited), keep-alive reuse. */
typedef struct {
    asx_tcp_stream stream;
    asx_http_parser parser;
    uint8_t head_buf[ASX_HTTP_CONN_HEAD_MAX];
    asx_buf_mut rx;
    uint8_t tx[ASX_HTTP_CLIENT_TX_MAX];
    uint32_t tx_len;
    uint32_t tx_pos;
    uint64_t body_remaining; /* streamed Content-Length request body */
    asx_http_body_sink_fn body_sink;
    void *body_sink_ctx;
    asx_http_conn_state state;
    asx_http_method request_method;
    uint32_t informational_seen;
    uint32_t responses_received;
    uint8_t request_open;  /* a request is in flight */
    uint8_t body_mode;     /* 0 none, 1 chunked stream, 2 length stream */
    uint8_t request_sent;  /* head + body fully queued */
    uint8_t head_received; /* final response head copied */
    uint8_t keep_alive;    /* persistence of the last response */
    uint8_t peer_eof;
} asx_http_client_conn;

/* Bind a client connection to a connected stream. `limits` may be NULL. */
ASX_API ASX_MUST_USE asx_status asx_http_client_conn_init(asx_http_client_conn *c,
                                                          asx_tcp_stream stream,
                                                          const asx_http_limits *limits);

/* Route response body bytes to `sink` instead of the inline response body
 * (needed for bodies larger than ASX_HTTP_BODY_MAX). Pass NULL to restore. */
ASX_API void asx_http_client_conn_set_body_sink(asx_http_client_conn *c, asx_http_body_sink_fn sink,
                                                void *ctx);

/* Queue a complete request (head + inline body per opts; opts may be NULL).
 * ASX_E_INVALID_STATE if a request is already in flight or the connection is
 * closed; serializer errors pass through. */
ASX_API ASX_MUST_USE asx_status asx_http_client_conn_send(asx_http_client_conn *c,
                                                          const asx_http_request *req,
                                                          const asx_http_serialize_opts *opts);

/* Queue only the request head and open a streamed body: CHUNKED framing
 * streams chunks; LENGTH framing streams exactly opts->content_length raw
 * bytes. Any other framing (or NULL opts) is ASX_E_INVALID_ARGUMENT; use
 * asx_http_client_conn_send for inline bodies. */
ASX_API ASX_MUST_USE asx_status asx_http_client_conn_begin(asx_http_client_conn *c,
                                                           const asx_http_request *req,
                                                           const asx_http_serialize_opts *opts);

/* Append streamed body bytes (one chunk when chunked). Fail-atomic:
 * ASX_E_WOULD_BLOCK when the transmit buffer is full (poll, then retry),
 * ASX_E_INVALID_ARGUMENT when exceeding a declared Content-Length. */
ASX_API ASX_MUST_USE asx_status asx_http_client_conn_write_body(asx_http_client_conn *c,
                                                                const void *data, uint32_t len);

/* Close a streamed body (chunked: last-chunk + optional trailers; length:
 * requires all declared bytes written). ASX_E_WOULD_BLOCK if the transmit
 * buffer is full. */
ASX_API ASX_MUST_USE asx_status asx_http_client_conn_finish(asx_http_client_conn *c,
                                                            const asx_http_headers *trailers);

/* Drive the connection: flush queued bytes, then read and parse the
 * response. Returns ASX_OK once the final response is collected into `resp`
 * (afterwards the connection is idle for reuse, or closed when the response
 * was not persistent), ASX_E_PENDING while waiting on I/O, or an error (the
 * connection is then closed). */
ASX_API ASX_MUST_USE asx_status asx_http_client_conn_poll(asx_http_client_conn *c,
                                                          asx_http_response *resp);

/* Inform the client that the transport reached EOF (for transports that
 * cannot report it through poll_read, e.g. the in-memory model). */
ASX_API void asx_http_client_conn_notify_eof(asx_http_client_conn *c);

/* Return 1 if the connection can carry another request. */
ASX_API int asx_http_client_conn_reusable(const asx_http_client_conn *c);

/* Close the underlying stream and mark the connection closed. */
ASX_API asx_status asx_http_client_conn_close(asx_http_client_conn *c);

#if ASX_HAS_SERVER_SURFACE
/* Server connection configuration. Request bodies are collected into the
 * inline asx_http_request body (larger bodies are answered with 413) unless
 * `body_sink` is set, in which case they stream to the sink and the handler
 * sees body.kind == ASX_HTTP_BODY_STREAM with body.content_length = bytes. */
typedef struct {
    asx_http_router *router;
    asx_session_pair *session;
    asx_security_context *security;
    asx_http_limits limits;
    uint32_t max_requests; /* per connection; 0 = unlimited */
    /* Rust's Http1Config::idle_timeout: how long a connection may wait for
     * and read one whole request (head and body), idle between keep-alive
     * requests included, and how long a response write may make no progress
     * (each write that sends bytes starts a new window). When it elapses the
     * connection is closed (poll returns ASX_E_TIMED_OUT), so slow or idle
     * clients cannot hold a slot. 0 = no timeout. Measured on the runtime
     * clock; without one there is no timeout. */
    uint32_t idle_timeout_ms;
    uint8_t keep_alive_enabled;      /* 0: close after every response */
    uint8_t auto_continue;           /* answer Expect: 100-continue with 100 Continue */
    uint8_t emit_date;               /* Date header via the runtime clock hook */
    asx_http_body_sink_fn body_sink; /* optional streaming request-body sink */
    void *body_sink_ctx;
} asx_http_server_config;

/* Initialize server-connection config with defaults: keep-alive on, auto
 * 100-continue on, at most 1000 requests per connection and a 60 s idle
 * timeout (Rust's Http1Config defaults), default limits. */
ASX_API void asx_http_server_config_init(asx_http_server_config *cfg, asx_http_router *router);

/* Server connection: read request -> dispatch through the router/middleware
 * -> write response -> keep-alive loop (pipelined requests are answered in
 * order). Parse failures answer 400/413/414/431/501/505 and close. */
typedef struct {
    asx_tcp_stream stream;
    asx_http_server_config config;
    asx_http_parser parser;
    uint8_t head_buf[ASX_HTTP_CONN_HEAD_MAX];
    asx_buf_mut rx;
    uint8_t tx[ASX_HTTP_CONN_TX_MAX];
    uint32_t tx_len;
    uint32_t tx_pos;
    const uint8_t *tx_body;
    uint32_t tx_body_len;
    uint32_t tx_body_pos;
    asx_http_request request;
    asx_http_response response;
    asx_http_conn_state state;
    uint32_t requests_served;
    asx_http_parse_error last_error;
    asx_time window_start; /* start of the current idle-timeout window */
    uint8_t window_open;   /* the runtime clock gave window_start */
    uint8_t in_message;
    uint8_t close_after_write;
    uint8_t drain_requested;
    uint8_t peer_eof;
    uint8_t timed_out; /* closed by the idle timeout */
} asx_http_server_conn;

/* Bind a server connection to an accepted stream. Fails with
 * ASX_E_PERMISSION_DENIED where the server surface is unavailable. */
ASX_API ASX_MUST_USE asx_status asx_http_server_conn_init(asx_http_server_conn *c,
                                                          asx_tcp_stream stream,
                                                          const asx_http_server_config *cfg);

/* Drive the connection. Returns ASX_OK once the connection has finished and
 * its stream is closed, ASX_E_PENDING while it waits on I/O (or idles in
 * keep-alive), ASX_E_TIMED_OUT once the idle timeout closed it, or a
 * transport error (the connection is then closed). While pending inside a
 * task's poll, it arms that task's timer for the idle deadline, so the task
 * is woken to enforce it. */
ASX_API ASX_MUST_USE asx_status asx_http_server_conn_poll(asx_http_server_conn *c);

/* Ask the connection to finish gracefully: the in-flight response (if any)
 * is sent with Connection: close and no further requests are read. */
ASX_API void asx_http_server_conn_request_close(asx_http_server_conn *c);

/* Return 1 when no request is in flight and nothing is queued for writing. */
ASX_API int asx_http_server_conn_is_idle(const asx_http_server_conn *c);

/* Connection slots of an asx_http_server (each holds an
 * asx_http_server_conn, about 38 KB). The live limit is also bounded by the
 * asx_server's max_connections. */
#ifndef ASX_HTTP_SERVER_MAX_CONNS
#if defined(ASX_PROFILE_EMBEDDED_ROUTER) || defined(ASX_PROFILE_FREESTANDING)
#define ASX_HTTP_SERVER_MAX_CONNS 4u
#else
#define ASX_HTTP_SERVER_MAX_CONNS 16u
#endif
#endif
#if (ASX_HTTP_SERVER_MAX_CONNS) < 1
#error "ASX_HTTP_SERVER_MAX_CONNS must be at least 1"
#endif

/* HTTP server wired to asx_server accept/drain accounting. */
typedef struct {
    asx_server *server;
    asx_http_server_config config;
    asx_http_server_conn conns[ASX_HTTP_SERVER_MAX_CONNS];
    uint32_t conn_ids[ASX_HTTP_SERVER_MAX_CONNS]; /* asx_server conn id, 0 = free */
    uint32_t connections_completed;
    uint32_t requests_completed_conns; /* requests answered by already-finished connections */
    uint32_t connections_timed_out;    /* closed by the idle timeout */
    uint32_t connections_drain_closed; /* closed at the drain deadline */
} asx_http_server;

/* Bind an HTTP server to an asx_server (which must already be listening). */
ASX_API ASX_MUST_USE asx_status asx_http_server_init(asx_http_server *hs, asx_server *srv,
                                                     const asx_http_server_config *cfg);

/* Accept pending connections and drive every connection once. With every
 * slot taken (ASX_HTTP_SERVER_MAX_CONNS, or the asx_server's
 * max_connections) a pending client is accepted and closed at once
 * (asx_server_reject_pending), as Rust's listener does at its limit, so it
 * fails fast instead of hanging in the backlog. A connection idle or stalled
 * past config.idle_timeout_ms is closed. While the server drains, idle
 * keep-alive connections are closed and in-flight responses finish with
 * Connection: close; at the drain deadline (asx_server_drain_deadline) the
 * remaining connections are closed. Inside a task's poll it arms the task's
 * timer for the next deadline. Returns ASX_OK once the server is stopped
 * with no connections left, ASX_E_PENDING otherwise. */
ASX_API ASX_MUST_USE asx_status asx_http_server_poll(asx_http_server *hs);

/* Return the number of live HTTP connections. */
ASX_API uint32_t asx_http_server_active_conns(const asx_http_server *hs);

/* Return the total number of requests answered across all connections. */
ASX_API uint32_t asx_http_server_requests_served(const asx_http_server *hs);
#endif /* ASX_HAS_SERVER_SURFACE */

/* -------------------------------------------------------------------
 * HTTP connection pool
 * ------------------------------------------------------------------- */

#ifndef ASX_HTTP_POOL_MAX_CONNECTIONS
#define ASX_HTTP_POOL_MAX_CONNECTIONS 8u
#endif

typedef enum {
    ASX_HTTP_POOL_CONN_IDLE = 0,
    ASX_HTTP_POOL_CONN_ACTIVE = 1,
    ASX_HTTP_POOL_CONN_CLOSED = 2
} asx_http_pool_conn_state;

typedef struct {
    asx_http_version version;
    asx_http_pool_conn_state state;
    uint32_t id;
    uint32_t requests_served;
} asx_http_pool_conn;

typedef struct {
    asx_http_pool_conn connections[ASX_HTTP_POOL_MAX_CONNECTIONS];
    uint32_t max_connections;
    uint32_t active_count;
    uint32_t idle_count;
    uint32_t next_id;
} asx_http_pool;

/* Initialize connection pool. */
ASX_API void asx_http_pool_init(asx_http_pool *pool, uint32_t max_connections);

/* Acquire an idle connection or create a new one. */
ASX_API asx_status asx_http_pool_acquire(asx_http_pool *pool, asx_http_pool_conn *out);

/* Release a connection back to idle. */
ASX_API asx_status asx_http_pool_release(asx_http_pool *pool, uint32_t conn_id);

/* Close a specific connection. */
ASX_API asx_status asx_http_pool_close(asx_http_pool *pool, uint32_t conn_id);

/* Get pool statistics. */
ASX_API uint32_t asx_http_pool_active_count(const asx_http_pool *pool);
ASX_API uint32_t asx_http_pool_idle_count(const asx_http_pool *pool);

/* Reset pool (test support). */
ASX_API void asx_http_pool_reset(asx_http_pool *pool);

#ifdef __cplusplus
}
#endif

#endif /* ASX_NET_HTTP_H */
