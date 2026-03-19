/*
 * test_grpc.c — unit tests for gRPC codec, service, client/server,
 *               reflection, and health
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/net/grpc.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Test handler                                                        */
/* ------------------------------------------------------------------ */

static asx_status echo_handler(const asx_grpc_message *req, asx_grpc_message *resp,
                                const asx_grpc_metadata *md, asx_grpc_code *out_code,
                                void *user_data) {
    (void)md;
    (void)user_data;
    asx_grpc_message_set(resp, req->data, req->len);
    *out_code = ASX_GRPC_OK;
    return ASX_OK;
}

static asx_status fail_handler(const asx_grpc_message *req, asx_grpc_message *resp,
                                const asx_grpc_metadata *md, asx_grpc_code *out_code,
                                void *user_data) {
    (void)req;
    (void)resp;
    (void)md;
    (void)user_data;
    *out_code = ASX_GRPC_PERMISSION_DENIED;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Status code tests                                                   */
/* ------------------------------------------------------------------ */

TEST(grpc_code_str) {
    ASSERT_STR_EQ(asx_grpc_code_str(ASX_GRPC_OK), "OK");
    ASSERT_STR_EQ(asx_grpc_code_str(ASX_GRPC_NOT_FOUND), "NOT_FOUND");
    ASSERT_STR_EQ(asx_grpc_code_str(ASX_GRPC_UNIMPLEMENTED), "UNIMPLEMENTED");
    ASSERT_STR_EQ(asx_grpc_code_str(ASX_GRPC_UNAUTHENTICATED), "UNAUTHENTICATED");
}

/* ------------------------------------------------------------------ */
/* Message tests                                                       */
/* ------------------------------------------------------------------ */

TEST(grpc_message_set_and_read) {
    asx_grpc_message msg;
    asx_grpc_message_init(&msg);
    ASSERT_EQ(msg.len, 0u);

    ASSERT_EQ(asx_grpc_message_set(&msg, "hello-grpc", 10), ASX_OK);
    ASSERT_EQ(msg.len, 10u);
    ASSERT_TRUE(memcmp(msg.data, "hello-grpc", 10) == 0);
}

TEST(grpc_message_null_args) {
    ASSERT_EQ(asx_grpc_message_set(NULL, "x", 1), ASX_E_INVALID_ARGUMENT);
}

/* ------------------------------------------------------------------ */
/* Metadata tests                                                      */
/* ------------------------------------------------------------------ */

TEST(grpc_metadata_add_and_get) {
    asx_grpc_metadata md;
    const char *val;

    asx_grpc_metadata_init(&md);
    ASSERT_EQ(asx_grpc_metadata_add(&md, "authorization", "Bearer tok"), ASX_OK);
    ASSERT_EQ(md.count, 1u);

    val = asx_grpc_metadata_get(&md, "authorization");
    ASSERT_TRUE(val != NULL);
    ASSERT_STR_EQ(val, "Bearer tok");
    ASSERT_TRUE(asx_grpc_metadata_get(&md, "missing") == NULL);
}

/* ------------------------------------------------------------------ */
/* Service tests                                                       */
/* ------------------------------------------------------------------ */

TEST(grpc_service_add_method) {
    asx_grpc_service svc;
    asx_grpc_service_init(&svc, "TestService");
    ASSERT_STR_EQ(svc.name, "TestService");

    ASSERT_EQ(asx_grpc_service_add_unary(&svc, "Echo", echo_handler, NULL), ASX_OK);
    ASSERT_EQ(svc.method_count, 1u);
    ASSERT_STR_EQ(svc.methods[0].full_name, "/TestService/Echo");
    ASSERT_EQ(svc.methods[0].method_type, ASX_GRPC_METHOD_UNARY);
}

TEST(grpc_service_null_args) {
    asx_grpc_service svc;
    asx_grpc_service_init(&svc, "Svc");
    ASSERT_EQ(asx_grpc_service_add_unary(&svc, NULL, echo_handler, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_grpc_service_add_unary(&svc, "M", NULL, NULL), ASX_E_INVALID_ARGUMENT);
}

/* ------------------------------------------------------------------ */
/* Server tests                                                        */
/* ------------------------------------------------------------------ */

TEST(grpc_server_dispatch_unary) {
    asx_grpc_server srv;
    asx_grpc_service svc;
    asx_grpc_message req, resp;
    asx_grpc_code code;

    asx_grpc_server_init(&srv);
    asx_grpc_service_init(&svc, "EchoService");
    asx_grpc_service_add_unary(&svc, "Echo", echo_handler, NULL);
    ASSERT_EQ(asx_grpc_server_add_service(&srv, &svc), ASX_OK);

    asx_grpc_message_init(&req);
    asx_grpc_message_set(&req, "ping", 4);
    asx_grpc_message_init(&resp);

    ASSERT_EQ(asx_grpc_server_call(&srv, "/EchoService/Echo", &req, &resp, NULL, &code), ASX_OK);
    ASSERT_EQ(code, ASX_GRPC_OK);
    ASSERT_EQ(resp.len, 4u);
    ASSERT_TRUE(memcmp(resp.data, "ping", 4) == 0);
    ASSERT_EQ(asx_grpc_server_calls_handled(&srv), 1u);
}

TEST(grpc_server_method_not_found) {
    asx_grpc_server srv;
    asx_grpc_service svc;
    asx_grpc_message req, resp;
    asx_grpc_code code;

    asx_grpc_server_init(&srv);
    asx_grpc_service_init(&svc, "Svc");
    asx_grpc_service_add_unary(&svc, "M", echo_handler, NULL);
    asx_grpc_server_add_service(&srv, &svc);

    asx_grpc_message_init(&req);
    asx_grpc_message_init(&resp);
    ASSERT_EQ(asx_grpc_server_call(&srv, "/Svc/Missing", &req, &resp, NULL, &code), ASX_OK);
    ASSERT_EQ(code, ASX_GRPC_UNIMPLEMENTED);
}

TEST(grpc_server_handler_sets_code) {
    asx_grpc_server srv;
    asx_grpc_service svc;
    asx_grpc_message req, resp;
    asx_grpc_code code;

    asx_grpc_server_init(&srv);
    asx_grpc_service_init(&svc, "AuthService");
    asx_grpc_service_add_unary(&svc, "Check", fail_handler, NULL);
    asx_grpc_server_add_service(&srv, &svc);

    asx_grpc_message_init(&req);
    asx_grpc_message_init(&resp);
    ASSERT_EQ(asx_grpc_server_call(&srv, "/AuthService/Check", &req, &resp, NULL, &code), ASX_OK);
    ASSERT_EQ(code, ASX_GRPC_PERMISSION_DENIED);
}

/* ------------------------------------------------------------------ */
/* Client channel tests                                                */
/* ------------------------------------------------------------------ */

TEST(grpc_channel_call) {
    asx_grpc_server srv;
    asx_grpc_service svc;
    asx_grpc_channel ch;
    asx_grpc_message req, resp;
    asx_grpc_code code;

    asx_grpc_server_init(&srv);
    asx_grpc_service_init(&svc, "Greeter");
    asx_grpc_service_add_unary(&svc, "SayHello", echo_handler, NULL);
    asx_grpc_server_add_service(&srv, &svc);

    asx_grpc_channel_init(&ch, "localhost:50051", &srv);
    ASSERT_STR_EQ(ch.target, "localhost:50051");

    asx_grpc_message_init(&req);
    asx_grpc_message_set(&req, "world", 5);
    asx_grpc_message_init(&resp);

    ASSERT_EQ(asx_grpc_channel_call(&ch, "/Greeter/SayHello", &req, &resp, NULL, &code), ASX_OK);
    ASSERT_EQ(code, ASX_GRPC_OK);
    ASSERT_EQ(resp.len, 5u);
    ASSERT_TRUE(memcmp(resp.data, "world", 5) == 0);
}

TEST(grpc_channel_null_server) {
    asx_grpc_channel ch;
    asx_grpc_message req, resp;
    asx_grpc_code code;

    asx_grpc_channel_init(&ch, "target", NULL);
    asx_grpc_message_init(&req);
    asx_grpc_message_init(&resp);
    ASSERT_EQ(asx_grpc_channel_call(&ch, "/Svc/M", &req, &resp, NULL, &code),
              ASX_E_INVALID_ARGUMENT);
}

/* ------------------------------------------------------------------ */
/* Health check tests                                                  */
/* ------------------------------------------------------------------ */

TEST(grpc_health_set_and_check) {
    asx_grpc_health health;

    asx_grpc_health_init(&health);
    ASSERT_EQ(asx_grpc_health_check(&health, ""), ASX_GRPC_HEALTH_SERVICE_UNKNOWN);

    ASSERT_EQ(asx_grpc_health_set(&health, "", ASX_GRPC_HEALTH_SERVING), ASX_OK);
    ASSERT_EQ(asx_grpc_health_check(&health, ""), ASX_GRPC_HEALTH_SERVING);

    ASSERT_EQ(asx_grpc_health_set(&health, "MyService", ASX_GRPC_HEALTH_NOT_SERVING), ASX_OK);
    ASSERT_EQ(asx_grpc_health_check(&health, "MyService"), ASX_GRPC_HEALTH_NOT_SERVING);

    /* Update existing */
    ASSERT_EQ(asx_grpc_health_set(&health, "MyService", ASX_GRPC_HEALTH_SERVING), ASX_OK);
    ASSERT_EQ(asx_grpc_health_check(&health, "MyService"), ASX_GRPC_HEALTH_SERVING);
}

TEST(grpc_health_unknown_service) {
    asx_grpc_health health;
    asx_grpc_health_init(&health);
    ASSERT_EQ(asx_grpc_health_check(&health, "Unknown"), ASX_GRPC_HEALTH_SERVICE_UNKNOWN);
}

/* ------------------------------------------------------------------ */
/* Reflection tests                                                    */
/* ------------------------------------------------------------------ */

TEST(grpc_reflect_lists_services) {
    asx_grpc_server srv;
    asx_grpc_service svc1, svc2;
    asx_grpc_reflection refl;

    asx_grpc_server_init(&srv);
    asx_grpc_service_init(&svc1, "ServiceA");
    asx_grpc_service_init(&svc2, "ServiceB");
    asx_grpc_server_add_service(&srv, &svc1);
    asx_grpc_server_add_service(&srv, &svc2);

    ASSERT_EQ(asx_grpc_reflect(&srv, &refl), ASX_OK);
    ASSERT_EQ(refl.count, 2u);
    ASSERT_TRUE(asx_grpc_reflect_has_service(&refl, "ServiceA"));
    ASSERT_TRUE(asx_grpc_reflect_has_service(&refl, "ServiceB"));
    ASSERT_FALSE(asx_grpc_reflect_has_service(&refl, "ServiceC"));
}

TEST(grpc_reflect_empty_server) {
    asx_grpc_server srv;
    asx_grpc_reflection refl;

    asx_grpc_server_init(&srv);
    ASSERT_EQ(asx_grpc_reflect(&srv, &refl), ASX_OK);
    ASSERT_EQ(refl.count, 0u);
}

/* ------------------------------------------------------------------ */
/* Metadata with call tests                                            */
/* ------------------------------------------------------------------ */

static asx_status md_check_handler(const asx_grpc_message *req, asx_grpc_message *resp,
                                    const asx_grpc_metadata *md, asx_grpc_code *out_code,
                                    void *user_data) {
    const char *auth;
    (void)req;
    (void)resp;
    (void)user_data;

    if (md == NULL) {
        *out_code = ASX_GRPC_UNAUTHENTICATED;
        return ASX_OK;
    }
    auth = asx_grpc_metadata_get(md, "authorization");
    *out_code = (auth != NULL) ? ASX_GRPC_OK : ASX_GRPC_UNAUTHENTICATED;
    return ASX_OK;
}

TEST(grpc_server_with_metadata) {
    asx_grpc_server srv;
    asx_grpc_service svc;
    asx_grpc_message req, resp;
    asx_grpc_metadata md;
    asx_grpc_code code;

    asx_grpc_server_init(&srv);
    asx_grpc_service_init(&svc, "Auth");
    asx_grpc_service_add_unary(&svc, "Verify", md_check_handler, NULL);
    asx_grpc_server_add_service(&srv, &svc);

    /* Without metadata */
    asx_grpc_message_init(&req);
    asx_grpc_message_init(&resp);
    asx_grpc_server_call(&srv, "/Auth/Verify", &req, &resp, NULL, &code);
    ASSERT_EQ(code, ASX_GRPC_UNAUTHENTICATED);

    /* With metadata */
    asx_grpc_metadata_init(&md);
    asx_grpc_metadata_add(&md, "authorization", "Bearer tok123");
    asx_grpc_message_init(&resp);
    asx_grpc_server_call(&srv, "/Auth/Verify", &req, &resp, &md, &code);
    ASSERT_EQ(code, ASX_GRPC_OK);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    fprintf(stderr, "=== grpc tests ===\n");

    /* Status codes */
    RUN_TEST(grpc_code_str);

    /* Message */
    RUN_TEST(grpc_message_set_and_read);
    RUN_TEST(grpc_message_null_args);

    /* Metadata */
    RUN_TEST(grpc_metadata_add_and_get);

    /* Service */
    RUN_TEST(grpc_service_add_method);
    RUN_TEST(grpc_service_null_args);

    /* Server */
    RUN_TEST(grpc_server_dispatch_unary);
    RUN_TEST(grpc_server_method_not_found);
    RUN_TEST(grpc_server_handler_sets_code);

    /* Client channel */
    RUN_TEST(grpc_channel_call);
    RUN_TEST(grpc_channel_null_server);

    /* Health */
    RUN_TEST(grpc_health_set_and_check);
    RUN_TEST(grpc_health_unknown_service);

    /* Reflection */
    RUN_TEST(grpc_reflect_lists_services);
    RUN_TEST(grpc_reflect_empty_server);

    /* Metadata with calls */
    RUN_TEST(grpc_server_with_metadata);

    TEST_REPORT();
    return test_failures;
}
