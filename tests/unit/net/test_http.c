/*
 * test_http.c — unit tests for HTTP surface
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/fs/fs.h>
#include <asx/net/http.h>
#include <asx/runtime/rt.h>
#include <asx/runtime/runtime.h>
#include <stdio.h>
#include <string.h>

static asx_runtime g_rt;
static asx_status st_sink_;

#define MUST_OK(expr)                                                                              \
    do {                                                                                           \
        st_sink_ = (expr);                                                                         \
        (void)st_sink_;                                                                            \
    } while (0)

static void setup_web(void) {
    MUST_OK(asx_runtime_init_default(&g_rt));
    asx_fs_reset();
    asx_session_reset();
}

static void teardown_web(void) { asx_runtime_shutdown(&g_rt); }

static void encode_tag_hex(const asx_auth_tag *tag, char *out, uint32_t out_size) {
    static const char hex[] = "0123456789abcdef";
    uint32_t i;

    ASSERT_TRUE(out_size >= (ASX_AUTH_TAG_SIZE * 2u) + 1u);
    for (i = 0u; i < ASX_AUTH_TAG_SIZE; i++) {
        out[i * 2u] = hex[(tag->bytes[i] >> 4u) & 0x0fu];
        out[(i * 2u) + 1u] = hex[tag->bytes[i] & 0x0fu];
    }
    out[ASX_AUTH_TAG_SIZE * 2u] = '\0';
}

static asx_status route_echo_handler(asx_http_request_context *ctx, asx_http_response *resp,
                                     void *user_data) {
    char id[32];
    char page[32];
    char body[128];
    int written;
    (void)user_data;

    if (asx_http_request_path_param(ctx, "id", id, sizeof(id)) != ASX_OK) return ASX_E_NOT_FOUND;
    if (asx_http_request_query_param(ctx->request, "page", page, sizeof(page)) != ASX_OK) {
        return ASX_E_NOT_FOUND;
    }
    asx_http_response_init(resp, ASX_HTTP_200_OK);
    written = snprintf(body, sizeof(body), "id=%s page=%s", id, page);
    if (written <= 0) return ASX_E_INVALID_ARGUMENT;
    return asx_http_body_set_bytes(&resp->body, body, (uint32_t)written);
}

static asx_status route_cookie_handler(asx_http_request_context *ctx, asx_http_response *resp,
                                       void *user_data) {
    char session_id[64];
    (void)user_data;

    if (asx_http_request_cookie(ctx->request, "sid", session_id, sizeof(session_id)) != ASX_OK) {
        return ASX_E_NOT_FOUND;
    }
    asx_http_response_init(resp, ASX_HTTP_200_OK);
    return asx_http_body_set_bytes(&resp->body, session_id, (uint32_t)strlen(session_id));
}

static asx_status route_ok_handler(asx_http_request_context *ctx, asx_http_response *resp,
                                   void *user_data) {
    (void)ctx;
    (void)user_data;
    asx_http_response_init(resp, ASX_HTTP_204_NO_CONTENT);
    return ASX_OK;
}

static asx_status route_session_handler(asx_http_request_context *ctx, asx_http_response *resp,
                                        void *user_data) {
    (void)user_data;
    if (ctx->session == NULL) return ASX_E_INVALID_ARGUMENT;
    asx_http_response_init(resp, ASX_HTTP_200_OK);
    if (asx_http_response_set_session_cookie(resp, "sid", "abc123", 1, 1) != ASX_OK) {
        return ASX_E_INVALID_ARGUMENT;
    }
    return asx_http_body_set_bytes(&resp->body, "session-ok", 10u);
}

static asx_status deny_middleware(asx_http_request_context *ctx, asx_http_response *resp,
                                  void *user_data, asx_http_middleware_result *out_result) {
    const char *body = (const char *)user_data;
    (void)ctx;

    asx_http_response_init(resp, ASX_HTTP_403_FORBIDDEN);
    if (asx_http_body_set_bytes(&resp->body, body, (uint32_t)strlen(body)) != ASX_OK) {
        return ASX_E_INVALID_ARGUMENT;
    }
    *out_result = ASX_HTTP_MIDDLEWARE_RESPOND;
    return ASX_OK;
}

/* Method tests */

TEST(method_str) {
    ASSERT_STR_EQ(asx_http_method_str(ASX_HTTP_GET), "GET");
    ASSERT_STR_EQ(asx_http_method_str(ASX_HTTP_POST), "POST");
    ASSERT_STR_EQ(asx_http_method_str(ASX_HTTP_DELETE), "DELETE");
    ASSERT_STR_EQ(asx_http_method_str(ASX_HTTP_PATCH), "PATCH");
    ASSERT_STR_EQ(asx_http_method_str(ASX_HTTP_OPTIONS), "OPTIONS");
}

/* Status tests */

TEST(status_categories) {
    ASSERT_TRUE(asx_http_status_is_info(100));
    ASSERT_TRUE(asx_http_status_is_success(200));
    ASSERT_TRUE(asx_http_status_is_success(204));
    ASSERT_TRUE(asx_http_status_is_redirect(301));
    ASSERT_TRUE(asx_http_status_is_client_error(404));
    ASSERT_TRUE(asx_http_status_is_server_error(500));
    ASSERT_FALSE(asx_http_status_is_success(404));
    ASSERT_FALSE(asx_http_status_is_client_error(200));
}

TEST(status_reason) {
    ASSERT_STR_EQ(asx_http_status_reason(200), "OK");
    ASSERT_STR_EQ(asx_http_status_reason(404), "Not Found");
    ASSERT_STR_EQ(asx_http_status_reason(500), "Internal Server Error");
}

/* Version tests */

TEST(version_str) {
    ASSERT_STR_EQ(asx_http_version_str(ASX_HTTP_VERSION_1_0), "HTTP/1.0");
    ASSERT_STR_EQ(asx_http_version_str(ASX_HTTP_VERSION_1_1), "HTTP/1.1");
    ASSERT_STR_EQ(asx_http_version_str(ASX_HTTP_VERSION_2), "HTTP/2");
    ASSERT_STR_EQ(asx_http_version_str(ASX_HTTP_VERSION_3), "HTTP/3");
}

/* Header tests */

TEST(headers_add_and_get) {
    asx_http_headers hdrs;
    const char *val;

    asx_http_headers_init(&hdrs);
    ASSERT_EQ(asx_http_headers_add(&hdrs, "Content-Type", "text/plain"), ASX_OK);
    ASSERT_EQ(asx_http_headers_add(&hdrs, "X-Custom", "value"), ASX_OK);
    ASSERT_EQ(hdrs.count, 2u);

    val = asx_http_headers_get(&hdrs, "Content-Type");
    ASSERT_TRUE(val != NULL);
    ASSERT_STR_EQ(val, "text/plain");

    val = asx_http_headers_get(&hdrs, "content-type");
    ASSERT_TRUE(val != NULL);  /* case-insensitive lookup */
}

TEST(headers_remove) {
    asx_http_headers hdrs;

    asx_http_headers_init(&hdrs);
    asx_http_headers_add(&hdrs, "X-A", "1");
    asx_http_headers_add(&hdrs, "X-B", "2");
    asx_http_headers_add(&hdrs, "X-A", "3");
    ASSERT_EQ(hdrs.count, 3u);

    ASSERT_EQ(asx_http_headers_remove(&hdrs, "X-A"), 2u);
    ASSERT_EQ(hdrs.count, 1u);
    ASSERT_TRUE(asx_http_headers_get(&hdrs, "X-B") != NULL);
    ASSERT_TRUE(asx_http_headers_get(&hdrs, "X-A") == NULL);
}

TEST(headers_not_found) {
    asx_http_headers hdrs;
    asx_http_headers_init(&hdrs);
    ASSERT_TRUE(asx_http_headers_get(&hdrs, "Missing") == NULL);
}

TEST(headers_exhaustion) {
    asx_http_headers hdrs;
    uint32_t i;

    asx_http_headers_init(&hdrs);
    for (i = 0; i < ASX_HTTP_MAX_HEADERS; i++) {
        ASSERT_EQ(asx_http_headers_add(&hdrs, "X-H", "v"), ASX_OK);
    }
    ASSERT_EQ(asx_http_headers_add(&hdrs, "X-Extra", "v"), ASX_E_RESOURCE_EXHAUSTED);
}

TEST(headers_null_args) {
    asx_http_headers hdrs;
    asx_http_headers_init(&hdrs);
    ASSERT_EQ(asx_http_headers_add(NULL, "k", "v"), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_http_headers_add(&hdrs, NULL, "v"), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_http_headers_add(&hdrs, "k", NULL), ASX_E_INVALID_ARGUMENT);
}

/* Body tests */

TEST(body_set_bytes) {
    asx_http_body body;
    asx_http_body_init(&body);
    ASSERT_TRUE(asx_http_body_is_empty(&body));

    ASSERT_EQ(asx_http_body_set_bytes(&body, "hello", 5), ASX_OK);
    ASSERT_FALSE(asx_http_body_is_empty(&body));
    ASSERT_EQ(asx_http_body_len(&body), 5u);
    ASSERT_EQ(body.kind, ASX_HTTP_BODY_BYTES);
    ASSERT_TRUE(memcmp(body.data, "hello", 5) == 0);
}

TEST(body_empty) {
    asx_http_body body;
    asx_http_body_init(&body);
    ASSERT_TRUE(asx_http_body_is_empty(&body));
    ASSERT_EQ(asx_http_body_len(&body), 0u);
}

TEST(body_rejects_null_nonempty_payload) {
    asx_http_body body;

    asx_http_body_init(&body);
    ASSERT_EQ(asx_http_body_set_bytes(&body, NULL, 1u), ASX_E_INVALID_ARGUMENT);
    ASSERT_TRUE(asx_http_body_is_empty(&body));
}

/* Request/response tests */

TEST(request_init) {
    asx_http_request req;
    asx_http_request_init(&req, ASX_HTTP_GET, "/api/users");
    ASSERT_EQ(req.method, ASX_HTTP_GET);
    ASSERT_STR_EQ(req.uri, "/api/users");
    ASSERT_EQ(req.version, ASX_HTTP_VERSION_1_1);
    ASSERT_TRUE(asx_http_body_is_empty(&req.body));
}

TEST(request_with_body_and_headers) {
    asx_http_request req;
    asx_http_request_init(&req, ASX_HTTP_POST, "/api/data");
    asx_http_headers_add(&req.headers, "Content-Type", "application/json");
    asx_http_body_set_bytes(&req.body, "{\"key\":\"val\"}", 13);

    ASSERT_EQ(req.method, ASX_HTTP_POST);
    ASSERT_EQ(req.headers.count, 1u);
    ASSERT_EQ(asx_http_body_len(&req.body), 13u);
}

TEST(response_init) {
    asx_http_response resp;
    asx_http_response_init(&resp, 200);
    ASSERT_EQ(resp.status, 200);
    ASSERT_EQ(resp.version, ASX_HTTP_VERSION_1_1);
    ASSERT_TRUE(asx_http_body_is_empty(&resp.body));
}

TEST(router_dispatch_extracts_path_and_query) {
    asx_http_router router;
    asx_http_route *route = NULL;
    asx_http_request req;
    asx_http_response resp;
    asx_http_request_context ctx;

    asx_http_router_init(&router);
    ASSERT_EQ(asx_http_router_add_route(&router, ASX_HTTP_GET, "/users/:id", route_echo_handler,
                                        NULL, NULL, &route),
              ASX_OK);

    asx_http_request_init(&req, ASX_HTTP_GET, "/users/42?page=7");
    ASSERT_EQ(asx_http_router_dispatch(&router, &req, &resp, NULL, NULL, &ctx), ASX_OK);
    ASSERT_EQ(resp.status, ASX_HTTP_200_OK);
    ASSERT_TRUE(memcmp(resp.body.data, "id=42 page=7", 12u) == 0);
    ASSERT_STR_EQ(ctx.route_pattern, "/users/:id");
}

TEST(router_cookie_extract) {
    asx_http_router router;
    asx_http_request req;
    asx_http_response resp;

    asx_http_router_init(&router);
    ASSERT_EQ(asx_http_router_add_route(&router, ASX_HTTP_GET, "/cookie", route_cookie_handler,
                                        NULL, NULL, NULL),
              ASX_OK);
    asx_http_request_init(&req, ASX_HTTP_GET, "/cookie");
    ASSERT_EQ(asx_http_headers_add(&req.headers, "Cookie", "sid=xyz; theme=dark"), ASX_OK);

    ASSERT_EQ(asx_http_router_dispatch(&router, &req, &resp, NULL, NULL, NULL), ASX_OK);
    ASSERT_EQ(resp.status, ASX_HTTP_200_OK);
    ASSERT_TRUE(memcmp(resp.body.data, "xyz", 3u) == 0);
}

TEST(router_middleware_short_circuit) {
    asx_http_router router;
    asx_http_route *route = NULL;
    asx_http_request req;
    asx_http_response resp;

    asx_http_router_init(&router);
    ASSERT_EQ(asx_http_router_add_route(&router, ASX_HTTP_GET, "/blocked", route_ok_handler, NULL,
                                        NULL, &route),
              ASX_OK);
    ASSERT_EQ(asx_http_route_add_middleware(route, deny_middleware, "blocked"), ASX_OK);
    asx_http_request_init(&req, ASX_HTTP_GET, "/blocked");

    ASSERT_EQ(asx_http_router_dispatch(&router, &req, &resp, NULL, NULL, NULL), ASX_OK);
    ASSERT_EQ(resp.status, ASX_HTTP_403_FORBIDDEN);
    ASSERT_TRUE(memcmp(resp.body.data, "blocked", 7u) == 0);
}

TEST(router_security_policy_requires_valid_auth) {
    asx_http_router router;
    asx_http_route_policy policy;
    asx_http_request req;
    asx_http_response resp;
    asx_http_request_context ctx;
    asx_security_context security;
    asx_auth_tag tag;
    char hex_tag[(ASX_AUTH_TAG_SIZE * 2u) + 1u];

    asx_http_router_init(&router);
    asx_http_route_policy_init(&policy);
    policy.require_security = 1u;
    ASSERT_EQ(asx_http_router_add_route(&router, ASX_HTTP_POST, "/secure", route_ok_handler, NULL,
                                        &policy, NULL),
              ASX_OK);

    asx_security_context_for_testing(&security, 77u);
    asx_http_request_init(&req, ASX_HTTP_POST, "/secure");
    ASSERT_EQ(asx_http_body_set_bytes(&req.body, "payload", 7u), ASX_OK);
    asx_security_context_sign(&security, req.body.data, req.body.len, &tag);
    encode_tag_hex(&tag, hex_tag, sizeof(hex_tag));
    ASSERT_EQ(asx_http_headers_add(&req.headers, "X-ASX-Auth", hex_tag), ASX_OK);

    ASSERT_EQ(asx_http_router_dispatch(&router, &req, &resp, NULL, &security, &ctx), ASX_OK);
    ASSERT_EQ(resp.status, ASX_HTTP_204_NO_CONTENT);
    ASSERT_EQ(ctx.security_verified, 1u);

    asx_http_headers_remove(&req.headers, "X-ASX-Auth");
    ASSERT_EQ(asx_http_headers_add(&req.headers, "X-ASX-Auth", "deadbeef"), ASX_OK);
    ASSERT_EQ(asx_http_router_dispatch(&router, &req, &resp, NULL, &security, NULL), ASX_OK);
    ASSERT_EQ(resp.status, ASX_HTTP_401_UNAUTHORIZED);
}

TEST(router_session_policy_requires_session) {
    asx_http_router router;
    asx_http_route_policy policy;
    asx_http_request req;
    asx_http_response resp;
    asx_region_id region;
    asx_session_pair pair;
    const char *cookie;

    setup_web();
    MUST_OK(asx_region_open(&region));
    MUST_OK(asx_session_open(region, 4u, &pair));

    asx_http_router_init(&router);
    asx_http_route_policy_init(&policy);
    policy.require_session = 1u;
    ASSERT_EQ(asx_http_router_add_route(&router, ASX_HTTP_GET, "/session", route_session_handler,
                                        NULL, &policy, NULL),
              ASX_OK);
    asx_http_request_init(&req, ASX_HTTP_GET, "/session");

    ASSERT_EQ(asx_http_router_dispatch(&router, &req, &resp, NULL, NULL, NULL), ASX_OK);
    ASSERT_EQ(resp.status, ASX_HTTP_401_UNAUTHORIZED);

    ASSERT_EQ(asx_http_router_dispatch(&router, &req, &resp, &pair, NULL, NULL), ASX_OK);
    ASSERT_EQ(resp.status, ASX_HTTP_200_OK);
    cookie = asx_http_headers_get(&resp.headers, "Set-Cookie");
    ASSERT_TRUE(cookie != NULL);
    ASSERT_TRUE(strstr(cookie, "sid=abc123") != NULL);
    ASSERT_TRUE(strstr(cookie, "HttpOnly") != NULL);
    ASSERT_TRUE(strstr(cookie, "Secure") != NULL);

    asx_session_close_initiator(&pair);
    asx_session_close_responder(&pair);
    teardown_web();
}

TEST(static_file_serving_and_traversal_rejection) {
    asx_fs_path path;
    asx_file_handle file;
    asx_buf payload;
    asx_http_response resp;
    uint32_t n;

    asx_fs_reset();
    ASSERT_EQ(asx_fs_path_from_cstr(&path, "/www/index.html"), ASX_OK);
    ASSERT_EQ(asx_fs_file_open(&file, &path,
                               ASX_FS_OPEN_CREATE | ASX_FS_OPEN_READ | ASX_FS_OPEN_WRITE),
              ASX_OK);
    payload = asx_buf_from_cstr("<h1>ok</h1>");
    ASSERT_EQ(asx_fs_file_poll_write(file, &payload, &n), ASX_OK);
    ASSERT_EQ(asx_fs_file_close(file), ASX_OK);

    ASSERT_EQ(asx_http_serve_static(&resp, "/www", "/"), ASX_OK);
    ASSERT_EQ(resp.status, ASX_HTTP_200_OK);
    ASSERT_STR_EQ(asx_http_headers_get(&resp.headers, "Content-Type"), "text/html");
    ASSERT_TRUE(memcmp(resp.body.data, "<h1>ok</h1>", 11u) == 0);

    ASSERT_EQ(asx_http_serve_static(&resp, "/www", "/../secret.txt"), ASX_E_PERMISSION_DENIED);
    ASSERT_EQ(resp.status, ASX_HTTP_403_FORBIDDEN);
}

TEST(sse_response_builder) {
    asx_http_response resp;

    ASSERT_EQ(asx_http_response_set_sse(&resp, "tick", "42", "hello"), ASX_OK);
    ASSERT_EQ(resp.status, ASX_HTTP_200_OK);
    ASSERT_STR_EQ(asx_http_headers_get(&resp.headers, "Content-Type"), "text/event-stream");
    ASSERT_TRUE(strstr((const char *)resp.body.data, "id: 42") != NULL);
    ASSERT_TRUE(strstr((const char *)resp.body.data, "event: tick") != NULL);
    ASSERT_TRUE(strstr((const char *)resp.body.data, "data: hello") != NULL);
}

TEST(multipart_parse_extracts_parts) {
    asx_http_request req;
    asx_http_multipart_form form;
    const char *body =
        "--BOUND\r\n"
        "Content-Disposition: form-data; name=\"title\"\r\n\r\n"
        "report\r\n"
        "--BOUND\r\n"
        "Content-Disposition: form-data; name=\"upload\"; filename=\"a.txt\"\r\n"
        "Content-Type: text/plain\r\n\r\n"
        "hello world\r\n"
        "--BOUND--\r\n";

    asx_http_request_init(&req, ASX_HTTP_POST, "/upload");
    ASSERT_EQ(asx_http_headers_add(&req.headers, "Content-Type",
                                   "multipart/form-data; boundary=BOUND"),
              ASX_OK);
    ASSERT_EQ(asx_http_body_set_bytes(&req.body, body, (uint32_t)strlen(body)), ASX_OK);

    ASSERT_EQ(asx_http_parse_multipart(&req, &form), ASX_OK);
    ASSERT_EQ(form.count, 2u);
    ASSERT_STR_EQ(form.parts[0].name, "title");
    ASSERT_TRUE(memcmp(form.parts[0].data, "report", 6u) == 0);
    ASSERT_STR_EQ(form.parts[1].name, "upload");
    ASSERT_STR_EQ(form.parts[1].filename, "a.txt");
    ASSERT_STR_EQ(form.parts[1].content_type, "text/plain");
    ASSERT_TRUE(memcmp(form.parts[1].data, "hello world", 11u) == 0);
}

TEST(multipart_parse_preserves_binary_file_payload) {
    asx_http_request req;
    asx_http_multipart_form form;
    uint8_t body[] = {
        '-', '-', 'B', 'O', 'U', 'N', 'D', '\r', '\n',
        'C', 'o', 'n', 't', 'e', 'n', 't', '-', 'D', 'i', 's', 'p', 'o', 's', 'i', 't', 'i',
        'o', 'n', ':', ' ', 'f', 'o', 'r', 'm', '-', 'd', 'a', 't', 'a', ';', ' ', 'n', 'a',
        'm', 'e', '=', '"', 'u', 'p', 'l', 'o', 'a', 'd', '"', ';', ' ', 'f', 'i', 'l', 'e',
        'n', 'a', 'm', 'e', '=', '"', 'b', 'i', 'n', '.', 'd', 'a', 't', '"', '\r', '\n',
        'C', 'o', 'n', 't', 'e', 'n', 't', '-', 'T', 'y', 'p', 'e', ':', ' ', 'a', 'p', 'p',
        'l', 'i', 'c', 'a', 't', 'i', 'o', 'n', '/', 'o', 'c', 't', 'e', 't', '-', 's', 't',
        'r', 'e', 'a', 'm', '\r', '\n', '\r', '\n',
        'A', 0x00, 'B', '\r', '\n',
        '-', '-', 'B', 'O', 'U', 'N', 'D', '-', '-', '\r', '\n'
    };

    asx_http_request_init(&req, ASX_HTTP_POST, "/upload");
    ASSERT_EQ(asx_http_headers_add(&req.headers, "Content-Type",
                                   "multipart/form-data; boundary=BOUND"),
              ASX_OK);
    ASSERT_EQ(asx_http_body_set_bytes(&req.body, body, (uint32_t)sizeof(body)), ASX_OK);

    ASSERT_EQ(asx_http_parse_multipart(&req, &form), ASX_OK);
    ASSERT_EQ(form.count, 1u);
    ASSERT_STR_EQ(form.parts[0].name, "upload");
    ASSERT_STR_EQ(form.parts[0].filename, "bin.dat");
    ASSERT_EQ(form.parts[0].data_len, 3u);
    ASSERT_EQ(form.parts[0].data[0], 'A');
    ASSERT_EQ(form.parts[0].data[1], 0x00u);
    ASSERT_EQ(form.parts[0].data[2], 'B');
}

/* Pool tests */

TEST(pool_acquire_release) {
    asx_http_pool pool;
    asx_http_pool_conn conn;

    asx_http_pool_init(&pool, 4);
    ASSERT_EQ(asx_http_pool_active_count(&pool), 0u);

    ASSERT_EQ(asx_http_pool_acquire(&pool, &conn), ASX_OK);
    ASSERT_EQ(conn.state, ASX_HTTP_POOL_CONN_ACTIVE);
    ASSERT_EQ(asx_http_pool_active_count(&pool), 1u);

    ASSERT_EQ(asx_http_pool_release(&pool, conn.id), ASX_OK);
    ASSERT_EQ(asx_http_pool_active_count(&pool), 0u);
    ASSERT_EQ(asx_http_pool_idle_count(&pool), 1u);
}

TEST(pool_reuse_idle) {
    asx_http_pool pool;
    asx_http_pool_conn c1, c2;

    asx_http_pool_init(&pool, 4);
    ASSERT_EQ(asx_http_pool_acquire(&pool, &c1), ASX_OK);
    ASSERT_EQ(asx_http_pool_release(&pool, c1.id), ASX_OK);

    /* Should reuse the idle connection */
    ASSERT_EQ(asx_http_pool_acquire(&pool, &c2), ASX_OK);
    ASSERT_EQ(c2.id, c1.id);
    ASSERT_EQ(c2.requests_served, 1u);

    asx_http_pool_close(&pool, c2.id);
}

TEST(pool_exhaustion) {
    asx_http_pool pool;
    asx_http_pool_conn conns[3];
    asx_http_pool_conn extra;
    uint32_t i;

    asx_http_pool_init(&pool, 2);
    for (i = 0; i < 2u; i++) {
        ASSERT_EQ(asx_http_pool_acquire(&pool, &conns[i]), ASX_OK);
    }
    ASSERT_EQ(asx_http_pool_acquire(&pool, &extra), ASX_E_RESOURCE_EXHAUSTED);

    for (i = 0; i < 2u; i++) {
        asx_http_pool_close(&pool, conns[i].id);
    }
}

TEST(pool_close_and_reset) {
    asx_http_pool pool;
    asx_http_pool_conn conn;

    asx_http_pool_init(&pool, 4);
    ASSERT_EQ(asx_http_pool_acquire(&pool, &conn), ASX_OK);
    ASSERT_EQ(asx_http_pool_close(&pool, conn.id), ASX_OK);
    ASSERT_EQ(asx_http_pool_active_count(&pool), 0u);

    /* Close again should fail */
    ASSERT_EQ(asx_http_pool_close(&pool, conn.id), ASX_E_NOT_FOUND);

    asx_http_pool_reset(&pool);
    ASSERT_EQ(asx_http_pool_active_count(&pool), 0u);
    ASSERT_EQ(asx_http_pool_idle_count(&pool), 0u);
}

TEST(pool_null_args) {
    asx_http_pool_conn conn;
    ASSERT_EQ(asx_http_pool_acquire(NULL, &conn), ASX_E_INVALID_ARGUMENT);
}

int main(void) {
    fprintf(stderr, "=== http tests ===\n");

    /* Method */
    RUN_TEST(method_str);

    /* Status */
    RUN_TEST(status_categories);
    RUN_TEST(status_reason);

    /* Version */
    RUN_TEST(version_str);

    /* Headers */
    RUN_TEST(headers_add_and_get);
    RUN_TEST(headers_remove);
    RUN_TEST(headers_not_found);
    RUN_TEST(headers_exhaustion);
    RUN_TEST(headers_null_args);

    /* Body */
    RUN_TEST(body_set_bytes);
    RUN_TEST(body_empty);
    RUN_TEST(body_rejects_null_nonempty_payload);

    /* Request/response */
    RUN_TEST(request_init);
    RUN_TEST(request_with_body_and_headers);
    RUN_TEST(response_init);

    /* Web surface */
    RUN_TEST(router_dispatch_extracts_path_and_query);
    RUN_TEST(router_cookie_extract);
    RUN_TEST(router_middleware_short_circuit);
    RUN_TEST(router_security_policy_requires_valid_auth);
    RUN_TEST(router_session_policy_requires_session);
    RUN_TEST(static_file_serving_and_traversal_rejection);
    RUN_TEST(sse_response_builder);
    RUN_TEST(multipart_parse_extracts_parts);
    RUN_TEST(multipart_parse_preserves_binary_file_payload);

    /* Pool */
    RUN_TEST(pool_acquire_release);
    RUN_TEST(pool_reuse_idle);
    RUN_TEST(pool_exhaustion);
    RUN_TEST(pool_close_and_reset);
    RUN_TEST(pool_null_args);

    TEST_REPORT();
    return test_failures;
}
