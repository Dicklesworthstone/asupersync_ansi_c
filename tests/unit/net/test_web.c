/*
 * test_web.c — unit tests for web router, middleware, extract, session, SSE,
 *              multipart, CORS, CSRF, and static file serving
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/net/web.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Test handler helpers                                                */
/* ------------------------------------------------------------------ */

static asx_status hello_handler(const asx_http_request *req, asx_http_response *resp,
                                 void *user_data) {
    (void)req;
    (void)user_data;
    asx_http_response_init(resp, ASX_HTTP_200_OK);
    asx_http_body_set_bytes(&resp->body, "hello", 5);
    return ASX_OK;
}

static asx_status custom_404(const asx_http_request *req, asx_http_response *resp,
                              void *user_data) {
    (void)req;
    (void)user_data;
    asx_http_response_init(resp, ASX_HTTP_404_NOT_FOUND);
    asx_http_body_set_bytes(&resp->body, "custom-404", 10);
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Router tests                                                        */
/* ------------------------------------------------------------------ */

TEST(router_dispatch_match) {
    asx_web_router router;
    asx_http_request req;
    asx_http_response resp;

    asx_web_router_init(&router);
    ASSERT_EQ(asx_web_router_add(&router, ASX_HTTP_GET, "/hello", hello_handler, NULL), ASX_OK);

    asx_http_request_init(&req, ASX_HTTP_GET, "/hello");
    memset(&resp, 0, sizeof(resp));
    ASSERT_EQ(asx_web_router_dispatch(&router, &req, &resp), ASX_OK);
    ASSERT_EQ(resp.status, ASX_HTTP_200_OK);
    ASSERT_EQ(asx_http_body_len(&resp.body), 5u);
}

TEST(router_dispatch_404) {
    asx_web_router router;
    asx_http_request req;
    asx_http_response resp;

    asx_web_router_init(&router);
    asx_web_router_add(&router, ASX_HTTP_GET, "/hello", hello_handler, NULL);

    asx_http_request_init(&req, ASX_HTTP_GET, "/missing");
    memset(&resp, 0, sizeof(resp));
    ASSERT_EQ(asx_web_router_dispatch(&router, &req, &resp), ASX_OK);
    ASSERT_EQ(resp.status, ASX_HTTP_404_NOT_FOUND);
}

TEST(router_dispatch_405) {
    asx_web_router router;
    asx_http_request req;
    asx_http_response resp;

    asx_web_router_init(&router);
    asx_web_router_add(&router, ASX_HTTP_GET, "/data", hello_handler, NULL);

    asx_http_request_init(&req, ASX_HTTP_POST, "/data");
    memset(&resp, 0, sizeof(resp));
    ASSERT_EQ(asx_web_router_dispatch(&router, &req, &resp), ASX_OK);
    ASSERT_EQ(resp.status, ASX_HTTP_405_METHOD_NOT_ALLOWED);
}

TEST(router_custom_not_found) {
    asx_web_router router;
    asx_http_request req;
    asx_http_response resp;

    asx_web_router_init(&router);
    asx_web_router_set_not_found(&router, custom_404, NULL);

    asx_http_request_init(&req, ASX_HTTP_GET, "/nope");
    memset(&resp, 0, sizeof(resp));
    ASSERT_EQ(asx_web_router_dispatch(&router, &req, &resp), ASX_OK);
    ASSERT_EQ(resp.status, ASX_HTTP_404_NOT_FOUND);
    ASSERT_EQ(asx_http_body_len(&resp.body), 10u);
}

TEST(router_query_string_ignored_in_match) {
    asx_web_router router;
    asx_http_request req;
    asx_http_response resp;

    asx_web_router_init(&router);
    asx_web_router_add(&router, ASX_HTTP_GET, "/api", hello_handler, NULL);

    asx_http_request_init(&req, ASX_HTTP_GET, "/api?foo=bar");
    memset(&resp, 0, sizeof(resp));
    ASSERT_EQ(asx_web_router_dispatch(&router, &req, &resp), ASX_OK);
    ASSERT_EQ(resp.status, ASX_HTTP_200_OK);
}

TEST(router_null_args) {
    asx_web_router router;
    asx_web_router_init(&router);
    ASSERT_EQ(asx_web_router_add(&router, ASX_HTTP_GET, NULL, hello_handler, NULL),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_web_router_add(&router, ASX_HTTP_GET, "/x", NULL, NULL),
              ASX_E_INVALID_ARGUMENT);
}

/* ------------------------------------------------------------------ */
/* Middleware tests                                                     */
/* ------------------------------------------------------------------ */

static asx_status add_header_mw(const asx_http_request *req, asx_http_response *resp,
                                 void *user_data, asx_web_handler_fn next, void *next_data) {
    asx_status st;
    (void)user_data;
    st = next(req, resp, next_data);
    asx_http_headers_add(&resp->headers, "X-Middleware", "applied");
    return st;
}

TEST(middleware_runs_and_modifies) {
    asx_web_middleware_stack stack;
    asx_http_request req;
    asx_http_response resp;
    const char *val;

    asx_web_middleware_init(&stack, hello_handler, NULL);
    ASSERT_EQ(asx_web_middleware_add(&stack, add_header_mw, NULL), ASX_OK);

    asx_http_request_init(&req, ASX_HTTP_GET, "/test");
    memset(&resp, 0, sizeof(resp));
    ASSERT_EQ(asx_web_middleware_run(&stack, &req, &resp), ASX_OK);
    ASSERT_EQ(resp.status, ASX_HTTP_200_OK);
    val = asx_http_headers_get(&resp.headers, "X-Middleware");
    ASSERT_TRUE(val != NULL);
    ASSERT_STR_EQ(val, "applied");
}

TEST(middleware_no_layers) {
    asx_web_middleware_stack stack;
    asx_http_request req;
    asx_http_response resp;

    asx_web_middleware_init(&stack, hello_handler, NULL);
    asx_http_request_init(&req, ASX_HTTP_GET, "/test");
    memset(&resp, 0, sizeof(resp));
    ASSERT_EQ(asx_web_middleware_run(&stack, &req, &resp), ASX_OK);
    ASSERT_EQ(resp.status, ASX_HTTP_200_OK);
}

/* ------------------------------------------------------------------ */
/* Query extraction tests                                              */
/* ------------------------------------------------------------------ */

TEST(query_parse_basic) {
    asx_web_query q;
    const char *val;

    ASSERT_EQ(asx_web_query_parse(&q, "/api?name=alice&age=30"), ASX_OK);
    ASSERT_EQ(q.count, 2u);

    val = asx_web_query_get(&q, "name");
    ASSERT_TRUE(val != NULL);
    ASSERT_STR_EQ(val, "alice");

    val = asx_web_query_get(&q, "age");
    ASSERT_TRUE(val != NULL);
    ASSERT_STR_EQ(val, "30");
}

TEST(query_parse_no_query) {
    asx_web_query q;
    ASSERT_EQ(asx_web_query_parse(&q, "/api"), ASX_OK);
    ASSERT_EQ(q.count, 0u);
}

TEST(query_get_missing) {
    asx_web_query q;
    ASSERT_EQ(asx_web_query_parse(&q, "/api?x=1"), ASX_OK);
    ASSERT_TRUE(asx_web_query_get(&q, "y") == NULL);
}

/* ------------------------------------------------------------------ */
/* Session tests                                                       */
/* ------------------------------------------------------------------ */

TEST(session_create_and_lookup) {
    asx_web_session_store store;
    char id[ASX_WEB_SESSION_ID_LEN + 1];
    asx_web_session *s;

    asx_web_session_store_init(&store);
    ASSERT_EQ(asx_web_session_create(&store, id, sizeof(id)), ASX_OK);
    ASSERT_TRUE(id[0] != '\0');

    s = asx_web_session_get(&store, id);
    ASSERT_TRUE(s != NULL);
    ASSERT_TRUE(s->active);
}

TEST(session_set_data) {
    asx_web_session_store store;
    char id[ASX_WEB_SESSION_ID_LEN + 1];
    asx_web_session *s;

    asx_web_session_store_init(&store);
    asx_web_session_create(&store, id, sizeof(id));
    s = asx_web_session_get(&store, id);
    ASSERT_TRUE(s != NULL);

    ASSERT_EQ(asx_web_session_set_data(s, "payload", 7), ASX_OK);
    ASSERT_EQ(s->data_len, 7u);
    ASSERT_TRUE(memcmp(s->data, "payload", 7) == 0);
}

TEST(session_set_data_rejects_null_nonempty_payload) {
    asx_web_session_store store;
    char id[ASX_WEB_SESSION_ID_LEN + 1];
    asx_web_session *s;

    asx_web_session_store_init(&store);
    asx_web_session_create(&store, id, sizeof(id));
    s = asx_web_session_get(&store, id);
    ASSERT_TRUE(s != NULL);

    ASSERT_EQ(asx_web_session_set_data(s, NULL, 1u), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(s->data_len, 0u);
}

TEST(session_destroy) {
    asx_web_session_store store;
    char id[ASX_WEB_SESSION_ID_LEN + 1];

    asx_web_session_store_init(&store);
    asx_web_session_create(&store, id, sizeof(id));
    ASSERT_TRUE(asx_web_session_get(&store, id) != NULL);

    ASSERT_EQ(asx_web_session_destroy(&store, id), ASX_OK);
    ASSERT_TRUE(asx_web_session_get(&store, id) == NULL);
}

TEST(session_destroy_missing) {
    asx_web_session_store store;
    asx_web_session_store_init(&store);
    ASSERT_EQ(asx_web_session_destroy(&store, "nonexistent"), ASX_E_NOT_FOUND);
}

/* ------------------------------------------------------------------ */
/* SSE tests                                                           */
/* ------------------------------------------------------------------ */

TEST(sse_push_and_poll) {
    asx_web_sse_stream stream;
    asx_web_sse_event evt;

    asx_web_sse_init(&stream);
    ASSERT_TRUE(asx_web_sse_is_open(&stream));

    ASSERT_EQ(asx_web_sse_push(&stream, "update", "data1"), ASX_OK);
    ASSERT_EQ(asx_web_sse_push(&stream, NULL, "data2"), ASX_OK);

    ASSERT_EQ(asx_web_sse_poll(&stream, &evt), ASX_OK);
    ASSERT_STR_EQ(evt.event, "update");
    ASSERT_STR_EQ(evt.data, "data1");
    ASSERT_EQ(evt.id, 1u);

    ASSERT_EQ(asx_web_sse_poll(&stream, &evt), ASX_OK);
    ASSERT_STR_EQ(evt.data, "data2");
    ASSERT_EQ(evt.id, 2u);

    ASSERT_EQ(asx_web_sse_poll(&stream, &evt), ASX_E_PENDING);
}

TEST(sse_push_rejects_overlong_event_name) {
    asx_web_sse_stream stream;
    char long_name[ASX_WEB_SSE_EVENT_NAME_MAX + 4u];

    memset(long_name, 'x', sizeof(long_name) - 1u);
    long_name[sizeof(long_name) - 1u] = '\0';

    asx_web_sse_init(&stream);
    ASSERT_EQ(asx_web_sse_push(&stream, long_name, "data"), ASX_E_BUFFER_TOO_SMALL);
    ASSERT_EQ(stream.count, 0u);
}

TEST(sse_close_rejects_push) {
    asx_web_sse_stream stream;

    asx_web_sse_init(&stream);
    asx_web_sse_close(&stream);
    ASSERT_FALSE(asx_web_sse_is_open(&stream));
    ASSERT_EQ(asx_web_sse_push(&stream, NULL, "nope"), ASX_E_INVALID_STATE);
}

/* ------------------------------------------------------------------ */
/* Multipart tests                                                     */
/* ------------------------------------------------------------------ */

TEST(multipart_add_field_and_get) {
    asx_web_multipart mp;
    const asx_web_multipart_part *part;

    asx_web_multipart_init(&mp);
    ASSERT_EQ(asx_web_multipart_add_field(&mp, "username", "alice", 5), ASX_OK);
    ASSERT_EQ(mp.count, 1u);

    part = asx_web_multipart_get(&mp, "username");
    ASSERT_TRUE(part != NULL);
    ASSERT_FALSE(part->is_file);
    ASSERT_EQ(part->data_len, 5u);
    ASSERT_TRUE(memcmp(part->data, "alice", 5) == 0);
}

TEST(multipart_add_file) {
    asx_web_multipart mp;
    const asx_web_multipart_part *part;
    uint8_t fdata[] = {0x89, 0x50, 0x4E, 0x47};

    asx_web_multipart_init(&mp);
    ASSERT_EQ(asx_web_multipart_add_file(&mp, "avatar", "pic.png", "image/png", fdata, 4), ASX_OK);

    part = asx_web_multipart_get(&mp, "avatar");
    ASSERT_TRUE(part != NULL);
    ASSERT_TRUE(part->is_file);
    ASSERT_STR_EQ(part->filename, "pic.png");
    ASSERT_STR_EQ(part->content_type, "image/png");
    ASSERT_EQ(part->data_len, 4u);
}

TEST(multipart_get_missing) {
    asx_web_multipart mp;
    asx_web_multipart_init(&mp);
    ASSERT_TRUE(asx_web_multipart_get(&mp, "nope") == NULL);
}

/* ------------------------------------------------------------------ */
/* CORS tests                                                          */
/* ------------------------------------------------------------------ */

TEST(cors_allow_all_by_default) {
    asx_web_cors_config cfg;
    asx_web_cors_config_init(&cfg);
    /* No origins added = allow all */
    ASSERT_TRUE(asx_web_cors_check_origin(&cfg, "http://example.com"));
}

TEST(cors_restrict_origin) {
    asx_web_cors_config cfg;
    asx_web_cors_config_init(&cfg);
    asx_web_cors_add_origin(&cfg, "http://allowed.com");

    ASSERT_TRUE(asx_web_cors_check_origin(&cfg, "http://allowed.com"));
    ASSERT_FALSE(asx_web_cors_check_origin(&cfg, "http://denied.com"));
}

TEST(cors_wildcard_origin) {
    asx_web_cors_config cfg;
    asx_web_cors_config_init(&cfg);
    asx_web_cors_add_origin(&cfg, "*");
    ASSERT_TRUE(asx_web_cors_check_origin(&cfg, "http://anything.com"));
}

TEST(cors_apply_headers) {
    asx_web_cors_config cfg;
    asx_http_response resp;
    const char *val;

    asx_web_cors_config_init(&cfg);
    asx_web_cors_add_origin(&cfg, "http://app.com");
    cfg.allow_credentials = 1u;

    asx_http_response_init(&resp, 200);
    ASSERT_EQ(asx_web_cors_apply(&cfg, "http://app.com", &resp), ASX_OK);

    val = asx_http_headers_get(&resp.headers, "Access-Control-Allow-Origin");
    ASSERT_TRUE(val != NULL);
    ASSERT_STR_EQ(val, "http://app.com");

    val = asx_http_headers_get(&resp.headers, "Access-Control-Allow-Credentials");
    ASSERT_TRUE(val != NULL);
    ASSERT_STR_EQ(val, "true");
}

/* ------------------------------------------------------------------ */
/* CSRF tests                                                          */
/* ------------------------------------------------------------------ */

TEST(csrf_deterministic_token) {
    asx_web_csrf csrf1, csrf2;
    asx_web_csrf_init(&csrf1, 42);
    asx_web_csrf_init(&csrf2, 42);
    /* Same seed = same token */
    ASSERT_STR_EQ(asx_web_csrf_token(&csrf1), asx_web_csrf_token(&csrf2));
    ASSERT_TRUE(asx_web_csrf_validate(&csrf1, asx_web_csrf_token(&csrf2)));
}

TEST(csrf_different_seeds) {
    asx_web_csrf csrf1, csrf2;
    asx_web_csrf_init(&csrf1, 1);
    asx_web_csrf_init(&csrf2, 2);
    ASSERT_FALSE(asx_web_csrf_validate(&csrf1, asx_web_csrf_token(&csrf2)));
}

TEST(csrf_reject_bad_token) {
    asx_web_csrf csrf;
    asx_web_csrf_init(&csrf, 99);
    ASSERT_FALSE(asx_web_csrf_validate(&csrf, "bad-token"));
    ASSERT_FALSE(asx_web_csrf_validate(&csrf, NULL));
}

/* ------------------------------------------------------------------ */
/* Static file tests                                                   */
/* ------------------------------------------------------------------ */

TEST(static_add_and_serve) {
    asx_web_static_files sf;
    asx_http_response resp;
    static const uint8_t html[] = "<h1>hi</h1>";
    const char *ct;

    asx_web_static_init(&sf);
    ASSERT_EQ(asx_web_static_add(&sf, "/index.html", "text/html", html, 11), ASX_OK);

    asx_http_response_init(&resp, 0);
    ASSERT_EQ(asx_web_static_serve(&sf, "/index.html", &resp), ASX_OK);
    ASSERT_EQ(resp.status, ASX_HTTP_200_OK);
    ct = asx_http_headers_get(&resp.headers, "Content-Type");
    ASSERT_TRUE(ct != NULL);
    ASSERT_STR_EQ(ct, "text/html");
    ASSERT_EQ(asx_http_body_len(&resp.body), 11u);
}

TEST(static_serve_missing) {
    asx_web_static_files sf;
    asx_http_response resp;

    asx_web_static_init(&sf);
    asx_http_response_init(&resp, 0);
    ASSERT_EQ(asx_web_static_serve(&sf, "/missing.css", &resp), ASX_OK);
    ASSERT_EQ(resp.status, ASX_HTTP_404_NOT_FOUND);
}

TEST(static_lookup) {
    asx_web_static_files sf;
    static const uint8_t data[] = "body";
    const asx_web_static_entry *entry;

    asx_web_static_init(&sf);
    asx_web_static_add(&sf, "/style.css", "text/css", data, 4);

    entry = asx_web_static_lookup(&sf, "/style.css");
    ASSERT_TRUE(entry != NULL);
    ASSERT_EQ(entry->data_len, 4u);

    ASSERT_TRUE(asx_web_static_lookup(&sf, "/nope") == NULL);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    fprintf(stderr, "=== web tests ===\n");

    /* Router */
    RUN_TEST(router_dispatch_match);
    RUN_TEST(router_dispatch_404);
    RUN_TEST(router_dispatch_405);
    RUN_TEST(router_custom_not_found);
    RUN_TEST(router_query_string_ignored_in_match);
    RUN_TEST(router_null_args);

    /* Middleware */
    RUN_TEST(middleware_runs_and_modifies);
    RUN_TEST(middleware_no_layers);

    /* Query extraction */
    RUN_TEST(query_parse_basic);
    RUN_TEST(query_parse_no_query);
    RUN_TEST(query_get_missing);

    /* Session */
    RUN_TEST(session_create_and_lookup);
    RUN_TEST(session_set_data);
    RUN_TEST(session_set_data_rejects_null_nonempty_payload);
    RUN_TEST(session_destroy);
    RUN_TEST(session_destroy_missing);

    /* SSE */
    RUN_TEST(sse_push_and_poll);
    RUN_TEST(sse_push_rejects_overlong_event_name);
    RUN_TEST(sse_close_rejects_push);

    /* Multipart */
    RUN_TEST(multipart_add_field_and_get);
    RUN_TEST(multipart_add_file);
    RUN_TEST(multipart_get_missing);

    /* CORS */
    RUN_TEST(cors_allow_all_by_default);
    RUN_TEST(cors_restrict_origin);
    RUN_TEST(cors_wildcard_origin);
    RUN_TEST(cors_apply_headers);

    /* CSRF */
    RUN_TEST(csrf_deterministic_token);
    RUN_TEST(csrf_different_seeds);
    RUN_TEST(csrf_reject_bad_token);

    /* Static files */
    RUN_TEST(static_add_and_serve);
    RUN_TEST(static_serve_missing);
    RUN_TEST(static_lookup);

    TEST_REPORT();
    return test_failures;
}
