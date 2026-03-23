/*
 * asx/net/http.h — HTTP request/response types, connection pool, and version support
 *
 * Provides the public HTTP surface: method/status enums, header management,
 * body types, request/response builders, connection pool, and HTTP/1.1, HTTP/2,
 * and HTTP/3 version negotiation. In the deterministic core, all HTTP operations
 * are modeled as in-memory value types without real network I/O.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_NET_HTTP_H
#define ASX_NET_HTTP_H

#include <asx/asx_export.h>
#include <asx/asx_config.h>
#include <asx/asx_status.h>
#include <asx/bytes/buf.h>
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
    ASX_HTTP_GET     = 0,
    ASX_HTTP_POST    = 1,
    ASX_HTTP_PUT     = 2,
    ASX_HTTP_DELETE  = 3,
    ASX_HTTP_PATCH   = 4,
    ASX_HTTP_HEAD    = 5,
    ASX_HTTP_OPTIONS = 6,
    ASX_HTTP_CONNECT = 7,
    ASX_HTTP_TRACE   = 8
} asx_http_method;

/* Return the method name as a string (e.g. "GET"). */
ASX_API const char *asx_http_method_str(asx_http_method method);

/* -------------------------------------------------------------------
 * HTTP status code
 * ------------------------------------------------------------------- */

typedef uint16_t asx_http_status;

/* Common status code constants. */
#define ASX_HTTP_200_OK                    200
#define ASX_HTTP_201_CREATED               201
#define ASX_HTTP_204_NO_CONTENT            204
#define ASX_HTTP_301_MOVED                 301
#define ASX_HTTP_302_FOUND                 302
#define ASX_HTTP_304_NOT_MODIFIED          304
#define ASX_HTTP_400_BAD_REQUEST           400
#define ASX_HTTP_401_UNAUTHORIZED          401
#define ASX_HTTP_403_FORBIDDEN             403
#define ASX_HTTP_404_NOT_FOUND             404
#define ASX_HTTP_405_METHOD_NOT_ALLOWED    405
#define ASX_HTTP_408_REQUEST_TIMEOUT       408
#define ASX_HTTP_429_TOO_MANY_REQUESTS     429
#define ASX_HTTP_500_INTERNAL_ERROR        500
#define ASX_HTTP_502_BAD_GATEWAY           502
#define ASX_HTTP_503_SERVICE_UNAVAILABLE   503
#define ASX_HTTP_504_GATEWAY_TIMEOUT       504

/* Return the reason phrase for a status code. */
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
    ASX_HTTP_VERSION_2   = 2,
    ASX_HTTP_VERSION_3   = 3
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

/* -------------------------------------------------------------------
 * HTTP body
 * ------------------------------------------------------------------- */

#ifndef ASX_HTTP_BODY_MAX
#define ASX_HTTP_BODY_MAX 4096u
#endif

typedef enum {
    ASX_HTTP_BODY_EMPTY  = 0,
    ASX_HTTP_BODY_BYTES  = 1,
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
    ASX_HTTP_MIDDLEWARE_RESPOND  = 1
} asx_http_middleware_result;

typedef asx_status (*asx_http_handler_fn)(asx_http_request_context *ctx, asx_http_response *resp,
                                          void *user_data);
typedef asx_status (*asx_http_middleware_fn)(asx_http_request_context *ctx,
                                             asx_http_response *resp, void *user_data,
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

ASX_API void asx_http_route_policy_init(asx_http_route_policy *policy);
ASX_API void asx_http_router_init(asx_http_router *router);
ASX_API asx_status asx_http_router_add_route(asx_http_router *router, asx_http_method method,
                                             const char *pattern, asx_http_handler_fn handler,
                                             void *handler_user_data,
                                             const asx_http_route_policy *policy,
                                             asx_http_route **out_route);
ASX_API asx_status asx_http_route_add_middleware(asx_http_route *route,
                                                 asx_http_middleware_fn fn, void *user_data);
ASX_API asx_status asx_http_router_dispatch(asx_http_router *router, const asx_http_request *req,
                                            asx_http_response *resp, asx_session_pair *session,
                                            asx_security_context *security,
                                            asx_http_request_context *out_ctx);

ASX_API asx_status asx_http_request_path_param(const asx_http_request_context *ctx,
                                               const char *name, char *out, uint32_t out_size);
ASX_API asx_status asx_http_request_query_param(const asx_http_request *req, const char *name,
                                                char *out, uint32_t out_size);
ASX_API asx_status asx_http_request_cookie(const asx_http_request *req, const char *name,
                                           char *out, uint32_t out_size);
ASX_API asx_status asx_http_request_verify_body_auth(const asx_http_request *req,
                                                     asx_security_context *security,
                                                     const char *purpose, int *out_verified);

ASX_API asx_status asx_http_response_set_session_cookie(asx_http_response *resp,
                                                        const char *name, const char *value,
                                                        int secure, int http_only);
ASX_API asx_status asx_http_response_set_sse(asx_http_response *resp, const char *event,
                                             const char *id, const char *data);
#if ASX_HAS_NATIVE_RUNTIME_SURFACES
ASX_API asx_status asx_http_serve_static(asx_http_response *resp, const char *root,
                                         const char *uri);
#endif
ASX_API asx_status asx_http_parse_multipart(const asx_http_request *req,
                                            asx_http_multipart_form *out_form);

/* -------------------------------------------------------------------
 * HTTP connection pool
 * ------------------------------------------------------------------- */

#ifndef ASX_HTTP_POOL_MAX_CONNECTIONS
#define ASX_HTTP_POOL_MAX_CONNECTIONS 8u
#endif

typedef enum {
    ASX_HTTP_POOL_CONN_IDLE   = 0,
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
