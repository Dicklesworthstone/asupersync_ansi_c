/*
 * asx/net/web.h — web router, middleware, extraction, session, SSE, multipart, security
 *
 * Provides the public web application surface: path-based routing with method
 * dispatch, layered middleware, typed request extraction, cookie-based sessions,
 * server-sent events, multipart form parsing, CORS, CSRF protection, and static
 * file serving. All deterministic in-memory for the core profile.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_NET_WEB_H
#define ASX_NET_WEB_H

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <asx/net/http.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Router
 * ------------------------------------------------------------------- */

#ifndef ASX_WEB_MAX_ROUTES
#define ASX_WEB_MAX_ROUTES 32u
#endif

#ifndef ASX_WEB_PATH_MAX
#define ASX_WEB_PATH_MAX 128u
#endif

/* Handler callback: receives request, writes response. Returns status. */
typedef asx_status (*asx_web_handler_fn)(const asx_http_request *req, asx_http_response *resp,
                                         void *user_data);

typedef struct {
    asx_http_method method;
    char path[ASX_WEB_PATH_MAX];
    asx_web_handler_fn handler;
    void *user_data;
    uint8_t active;
} asx_web_route;

typedef struct {
    asx_web_route routes[ASX_WEB_MAX_ROUTES];
    uint32_t count;
    asx_web_handler_fn not_found_handler;
    void *not_found_user_data;
} asx_web_router;

/* Initialize a router. */
ASX_API void asx_web_router_init(asx_web_router *router);

/* Register a route. Path must be an exact match (no wildcards in core). */
ASX_API asx_status asx_web_router_add(asx_web_router *router, asx_http_method method,
                                      const char *path, asx_web_handler_fn handler,
                                      void *user_data);

/* Set a custom 404 handler. */
ASX_API void asx_web_router_set_not_found(asx_web_router *router, asx_web_handler_fn handler,
                                          void *user_data);

/* Dispatch a request through the router. */
ASX_API asx_status asx_web_router_dispatch(const asx_web_router *router,
                                           const asx_http_request *req, asx_http_response *resp);

/* -------------------------------------------------------------------
 * Middleware
 * ------------------------------------------------------------------- */

#ifndef ASX_WEB_MAX_MIDDLEWARE
#define ASX_WEB_MAX_MIDDLEWARE 8u
#endif

/* Middleware callback: may modify request/response, calls next via chain. */
typedef asx_status (*asx_web_middleware_fn)(const asx_http_request *req, asx_http_response *resp,
                                            void *user_data, asx_web_handler_fn next,
                                            void *next_data);

typedef struct {
    asx_web_middleware_fn fn;
    void *user_data;
} asx_web_middleware_entry;

typedef struct {
    asx_web_middleware_entry layers[ASX_WEB_MAX_MIDDLEWARE];
    uint32_t count;
    asx_web_handler_fn inner;
    void *inner_data;
} asx_web_middleware_stack;

/* Initialize a middleware stack with an inner handler. */
ASX_API void asx_web_middleware_init(asx_web_middleware_stack *stack, asx_web_handler_fn inner,
                                     void *inner_data);

/* Add a middleware layer (outermost added last). */
ASX_API asx_status asx_web_middleware_add(asx_web_middleware_stack *stack, asx_web_middleware_fn fn,
                                          void *user_data);

/* Execute the middleware stack. */
ASX_API asx_status asx_web_middleware_run(const asx_web_middleware_stack *stack,
                                          const asx_http_request *req, asx_http_response *resp);

/* -------------------------------------------------------------------
 * Request extraction
 * ------------------------------------------------------------------- */

#ifndef ASX_WEB_MAX_QUERY_PARAMS
#define ASX_WEB_MAX_QUERY_PARAMS 16u
#endif

#ifndef ASX_WEB_PARAM_KEY_MAX
#define ASX_WEB_PARAM_KEY_MAX 64u
#endif

#ifndef ASX_WEB_PARAM_VALUE_MAX
#define ASX_WEB_PARAM_VALUE_MAX 256u
#endif

typedef struct {
    char key[ASX_WEB_PARAM_KEY_MAX];
    char value[ASX_WEB_PARAM_VALUE_MAX];
} asx_web_query_param;

typedef struct {
    asx_web_query_param params[ASX_WEB_MAX_QUERY_PARAMS];
    uint32_t count;
} asx_web_query;

/* Parse query string from URI (everything after '?'). */
ASX_API asx_status asx_web_query_parse(asx_web_query *out, const char *uri);

/* Get a query parameter by key. Returns NULL if not found. */
ASX_API const char *asx_web_query_get(const asx_web_query *query, const char *key);

/* Extract a header value from a request. Returns NULL if missing. */
ASX_API const char *asx_web_extract_header(const asx_http_request *req, const char *name);

/* Extract Content-Type from a request. Returns NULL if missing. */
ASX_API const char *asx_web_extract_content_type(const asx_http_request *req);

/* -------------------------------------------------------------------
 * Session
 * ------------------------------------------------------------------- */

#ifndef ASX_WEB_MAX_SESSIONS
#define ASX_WEB_MAX_SESSIONS 16u
#endif

#ifndef ASX_WEB_SESSION_ID_LEN
#define ASX_WEB_SESSION_ID_LEN 32u
#endif

#ifndef ASX_WEB_SESSION_MAX_DATA
#define ASX_WEB_SESSION_MAX_DATA 512u
#endif

typedef struct {
    char id[ASX_WEB_SESSION_ID_LEN + 1];
    uint8_t data[ASX_WEB_SESSION_MAX_DATA];
    uint32_t data_len;
    uint64_t created_at_ns;
    uint64_t expires_at_ns;
    uint8_t active;
} asx_web_session;

typedef struct {
    asx_web_session sessions[ASX_WEB_MAX_SESSIONS];
    uint32_t next_seq;
} asx_web_session_store;

/* Initialize session store. */
ASX_API void asx_web_session_store_init(asx_web_session_store *store);

/* Create a new session. Returns the session ID in out_id. */
ASX_API asx_status asx_web_session_create(asx_web_session_store *store, char *out_id,
                                          uint32_t id_capacity);

/* Look up a session by ID. Returns NULL if not found or expired. */
ASX_API asx_web_session *asx_web_session_get(asx_web_session_store *store, const char *id);

/* Set session data. */
ASX_API asx_status asx_web_session_set_data(asx_web_session *session, const void *data,
                                            uint32_t len);

/* Destroy a session by ID. */
ASX_API asx_status asx_web_session_destroy(asx_web_session_store *store, const char *id);

/* -------------------------------------------------------------------
 * Server-Sent Events (SSE)
 * ------------------------------------------------------------------- */

#ifndef ASX_WEB_SSE_MAX_EVENTS
#define ASX_WEB_SSE_MAX_EVENTS 16u
#endif

#ifndef ASX_WEB_SSE_DATA_MAX
#define ASX_WEB_SSE_DATA_MAX 256u
#endif

#ifndef ASX_WEB_SSE_EVENT_NAME_MAX
#define ASX_WEB_SSE_EVENT_NAME_MAX 32u
#endif

typedef struct {
    char event[ASX_WEB_SSE_EVENT_NAME_MAX];
    char data[ASX_WEB_SSE_DATA_MAX];
    uint32_t id;
} asx_web_sse_event;

typedef struct {
    asx_web_sse_event events[ASX_WEB_SSE_MAX_EVENTS];
    uint32_t head;
    uint32_t count;
    uint32_t next_id;
    uint8_t closed;
} asx_web_sse_stream;

/* Initialize an SSE stream. */
ASX_API void asx_web_sse_init(asx_web_sse_stream *stream);

/* Push an event to the SSE stream. */
ASX_API asx_status asx_web_sse_push(asx_web_sse_stream *stream, const char *event_name,
                                    const char *data);

/* Poll-receive an event. Returns ASX_E_PENDING if empty. */
ASX_API asx_status asx_web_sse_poll(asx_web_sse_stream *stream, asx_web_sse_event *out);

/* Close the SSE stream. */
ASX_API void asx_web_sse_close(asx_web_sse_stream *stream);

/* Check if the SSE stream is open. */
ASX_API int asx_web_sse_is_open(const asx_web_sse_stream *stream);

/* -------------------------------------------------------------------
 * Multipart form parsing
 * ------------------------------------------------------------------- */

#ifndef ASX_WEB_MAX_MULTIPART_PARTS
#define ASX_WEB_MAX_MULTIPART_PARTS 8u
#endif

#ifndef ASX_WEB_MULTIPART_NAME_MAX
#define ASX_WEB_MULTIPART_NAME_MAX 64u
#endif

#ifndef ASX_WEB_MULTIPART_FILENAME_MAX
#define ASX_WEB_MULTIPART_FILENAME_MAX 128u
#endif

#ifndef ASX_WEB_MULTIPART_DATA_MAX
#define ASX_WEB_MULTIPART_DATA_MAX 4096u
#endif

typedef struct {
    char name[ASX_WEB_MULTIPART_NAME_MAX];
    char filename[ASX_WEB_MULTIPART_FILENAME_MAX];
    char content_type[ASX_HTTP_HEADER_NAME_MAX];
    uint8_t data[ASX_WEB_MULTIPART_DATA_MAX];
    uint32_t data_len;
    uint8_t is_file;
} asx_web_multipart_part;

typedef struct {
    asx_web_multipart_part parts[ASX_WEB_MAX_MULTIPART_PARTS];
    uint32_t count;
} asx_web_multipart;

/* Initialize multipart result. */
ASX_API void asx_web_multipart_init(asx_web_multipart *mp);

/* Add a field part (non-file). */
ASX_API asx_status asx_web_multipart_add_field(asx_web_multipart *mp, const char *name,
                                               const void *data, uint32_t len);

/* Add a file part. */
ASX_API asx_status asx_web_multipart_add_file(asx_web_multipart *mp, const char *name,
                                              const char *filename, const char *content_type,
                                              const void *data, uint32_t len);

/* Get a part by name. Returns NULL if not found. */
ASX_API const asx_web_multipart_part *asx_web_multipart_get(const asx_web_multipart *mp,
                                                            const char *name);

/* -------------------------------------------------------------------
 * CORS
 * ------------------------------------------------------------------- */

#ifndef ASX_WEB_CORS_MAX_ORIGINS
#define ASX_WEB_CORS_MAX_ORIGINS 8u
#endif

#ifndef ASX_WEB_CORS_ORIGIN_MAX
#define ASX_WEB_CORS_ORIGIN_MAX 128u
#endif

typedef struct {
    char allowed_origins[ASX_WEB_CORS_MAX_ORIGINS][ASX_WEB_CORS_ORIGIN_MAX];
    uint32_t origin_count;
    uint8_t allow_credentials;
    uint32_t max_age_seconds;
} asx_web_cors_config;

/* Initialize CORS config (permissive defaults). */
ASX_API void asx_web_cors_config_init(asx_web_cors_config *cfg);

/* Add an allowed origin. */
ASX_API asx_status asx_web_cors_add_origin(asx_web_cors_config *cfg, const char *origin);

/* Check if an origin is allowed. */
ASX_API int asx_web_cors_check_origin(const asx_web_cors_config *cfg, const char *origin);

/* Apply CORS headers to a response for the given request origin. */
ASX_API asx_status asx_web_cors_apply(const asx_web_cors_config *cfg, const char *request_origin,
                                      asx_http_response *resp);

/* -------------------------------------------------------------------
 * CSRF protection
 * ------------------------------------------------------------------- */

#ifndef ASX_WEB_CSRF_TOKEN_LEN
#define ASX_WEB_CSRF_TOKEN_LEN 32u
#endif

typedef struct {
    char token[ASX_WEB_CSRF_TOKEN_LEN + 1];
    uint32_t seq;
} asx_web_csrf;

/* Initialize CSRF state with a deterministic token. */
ASX_API void asx_web_csrf_init(asx_web_csrf *csrf, uint32_t seed);

/* Get the current CSRF token. */
ASX_API const char *asx_web_csrf_token(const asx_web_csrf *csrf);

/* Validate a submitted token against the current state. */
ASX_API int asx_web_csrf_validate(const asx_web_csrf *csrf, const char *submitted);

/* -------------------------------------------------------------------
 * Static file serving (deterministic model)
 * ------------------------------------------------------------------- */

#ifndef ASX_WEB_MAX_STATIC_FILES
#define ASX_WEB_MAX_STATIC_FILES 16u
#endif

typedef struct {
    char path[ASX_WEB_PATH_MAX];
    char content_type[ASX_HTTP_HEADER_NAME_MAX];
    const uint8_t *data;
    uint32_t data_len;
} asx_web_static_entry;

typedef struct {
    asx_web_static_entry files[ASX_WEB_MAX_STATIC_FILES];
    uint32_t count;
} asx_web_static_files;

/* Initialize static file registry. */
ASX_API void asx_web_static_init(asx_web_static_files *sf);

/* Register a static file. Data pointer must remain valid for the lifetime of the registry. */
ASX_API asx_status asx_web_static_add(asx_web_static_files *sf, const char *path,
                                      const char *content_type, const uint8_t *data,
                                      uint32_t data_len);

/* Look up a static file by path. Returns NULL if not found. */
ASX_API const asx_web_static_entry *asx_web_static_lookup(const asx_web_static_files *sf,
                                                          const char *path);

/* Serve a static file into an HTTP response. */
ASX_API asx_status asx_web_static_serve(const asx_web_static_files *sf, const char *path,
                                        asx_http_response *resp);

#ifdef __cplusplus
}
#endif

#endif /* ASX_NET_WEB_H */
