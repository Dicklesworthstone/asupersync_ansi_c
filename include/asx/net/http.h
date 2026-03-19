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
#include <asx/asx_status.h>
#include <asx/bytes/buf.h>
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
