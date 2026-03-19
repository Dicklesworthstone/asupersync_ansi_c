/*
 * test_http.c — unit tests for HTTP surface
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/net/http.h>
#include <string.h>

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

    /* Request/response */
    RUN_TEST(request_init);
    RUN_TEST(request_with_body_and_headers);
    RUN_TEST(response_init);

    /* Pool */
    RUN_TEST(pool_acquire_release);
    RUN_TEST(pool_reuse_idle);
    RUN_TEST(pool_exhaustion);
    RUN_TEST(pool_close_and_reset);
    RUN_TEST(pool_null_args);

    TEST_REPORT();
    return test_failures;
}
