/*
 * test_service_stack.c — integration tests for composed service stacks
 *
 * Verifies that the service builder can materialize concrete middleware
 * stacks and that those stacks operate correctly over the deterministic
 * in-process memory transport.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/service/service.h>
#include <asx/transport/transport.h>
#include <string.h>

typedef struct {
    asx_transport *transport;
    asx_transport_conn client_conn;
    asx_transport_conn server_conn;
    char scratch[64];
} transport_echo_service_state;

typedef struct {
    char mapped[64];
} request_map_state;

typedef struct {
    int placeholder;
} string_service_state;

static asx_service_readiness transport_echo_poll_ready(void *state) {
    (void)state;
    return ASX_SERVICE_READY;
}

static asx_status transport_echo_call(void *state, const void *request, void *response) {
    transport_echo_service_state *svc = (transport_echo_service_state *)state;
    const char *message = (const char *)request;
    char *out = (char *)response;
    size_t message_len = strlen(message) + 1u;
    size_t written = 0;
    size_t read = 0;

    if (message_len > sizeof(svc->scratch)) { return ASX_E_INVALID_ARGUMENT; }

    if (asx_transport_write(svc->transport, svc->client_conn, message, message_len, &written) !=
        ASX_OK) {
        return ASX_E_DISCONNECTED;
    }
    if (written != message_len) { return ASX_E_INVALID_STATE; }

    if (asx_transport_read(svc->transport, svc->server_conn, svc->scratch, sizeof(svc->scratch),
                           &read) != ASX_OK) {
        return ASX_E_PENDING;
    }
    if (read != message_len) { return ASX_E_INVALID_STATE; }

    if (asx_transport_write(svc->transport, svc->server_conn, svc->scratch, read, &written) !=
        ASX_OK) {
        return ASX_E_DISCONNECTED;
    }
    if (written != read) { return ASX_E_INVALID_STATE; }

    if (asx_transport_read(svc->transport, svc->client_conn, out, 64u, &read) != ASX_OK) {
        return ASX_E_PENDING;
    }
    if (read != message_len) { return ASX_E_INVALID_STATE; }

    return ASX_OK;
}

static asx_service make_transport_echo_service(transport_echo_service_state *state,
                                               asx_transport *transport,
                                               asx_transport_conn client_conn,
                                               asx_transport_conn server_conn) {
    asx_service svc;
    memset(state, 0, sizeof(*state));
    state->transport = transport;
    state->client_conn = client_conn;
    state->server_conn = server_conn;
    svc.poll_ready = transport_echo_poll_ready;
    svc.call = transport_echo_call;
    svc.state = state;
    return svc;
}

static asx_service_readiness string_service_poll_ready(void *state) {
    (void)state;
    return ASX_SERVICE_READY;
}

static asx_status string_service_call(void *state, const void *request, void *response) {
    string_service_state *svc = (string_service_state *)state;
    const char *input = (const char *)request;
    char *out = (char *)response;

    (void)svc;
    if (strlen(input) + 1u > 64u) { return ASX_E_INVALID_ARGUMENT; }
    memcpy(out, input, strlen(input) + 1u);
    return ASX_OK;
}

static asx_service make_string_service(string_service_state *state) {
    asx_service svc;
    state->placeholder = 0;
    svc.poll_ready = string_service_poll_ready;
    svc.call = string_service_call;
    svc.state = state;
    return svc;
}

static const void *prefix_request(const void *request, void *user_data) {
    request_map_state *state = (request_map_state *)user_data;
    const char *input = (const char *)request;
    int written = snprintf(state->mapped, sizeof(state->mapped), "wire:%s", input);

    if (written < 0 || (size_t)written >= sizeof(state->mapped)) { return ""; }
    return state->mapped;
}

static void suffix_response(void *response, void *user_data) {
    char *buffer = (char *)response;
    const char *suffix = (const char *)user_data;
    size_t used = strlen(buffer);
    size_t remaining = 64u - used;

    if (remaining > 1u) { (void)strncat(buffer, suffix, remaining - 1u); }
}

TEST(builder_build_applies_map_layers_in_order) {
    asx_service_builder builder;
    asx_service_builder_runtime runtime;
    string_service_state string_state;
    asx_service base;
    asx_service built;
    request_map_state map_state;
    char response[64];

    base = make_string_service(&string_state);
    asx_service_builder_init(&builder);

    ASSERT_EQ(asx_service_builder_map_request(&builder, prefix_request, &map_state), ASX_OK);
    ASSERT_EQ(asx_service_builder_map_response(&builder, suffix_response, ":done"), ASX_OK);
    ASSERT_EQ(asx_service_builder_build(&built, &runtime, &builder, base), ASX_OK);

    memset(response, 0, sizeof(response));
    ASSERT_EQ(asx_service_call(&built, "ping", response), ASX_OK);
    ASSERT_TRUE(strcmp(response, "wire:ping:done") == 0);
}

TEST(builder_stack_roundtrips_over_memory_transport) {
    asx_transport transport;
    asx_mem_transport_state transport_state;
    asx_transport_listener listener;
    asx_transport_conn client_conn;
    asx_transport_conn server_conn;
    transport_echo_service_state base_state;
    request_map_state map_state;
    asx_service base;
    asx_service_builder builder;
    asx_service_builder_runtime runtime;
    asx_service built;
    char response[64];
    uint32_t addr = 4242u;

    asx_mem_transport_init(&transport, &transport_state);
    ASSERT_EQ(asx_transport_listen(&transport, &addr, sizeof(addr), &listener), ASX_OK);
    ASSERT_EQ(asx_transport_connect(&transport, &addr, sizeof(addr), &client_conn), ASX_OK);
    ASSERT_EQ(asx_transport_accept(&transport, listener, &server_conn), ASX_OK);

    base = make_transport_echo_service(&base_state, &transport, client_conn, server_conn);

    asx_service_builder_init(&builder);
    ASSERT_EQ(asx_service_builder_map_request(&builder, prefix_request, &map_state), ASX_OK);
    ASSERT_EQ(asx_service_builder_rate_limit(&builder, 1u, 1000000000ULL), ASX_OK);
    ASSERT_EQ(asx_service_builder_map_response(&builder, suffix_response, ":ok"), ASX_OK);
    ASSERT_EQ(asx_service_builder_build(&built, &runtime, &builder, base), ASX_OK);

    memset(response, 0, sizeof(response));
    ASSERT_EQ(asx_service_call(&built, "ping", response), ASX_OK);
    ASSERT_TRUE(strcmp(response, "wire:ping:ok") == 0);

    memset(response, 0, sizeof(response));
    ASSERT_EQ(asx_service_call(&built, "pong", response), ASX_E_RESOURCE_EXHAUSTED);
}

int main(void) {
    fprintf(stderr, "=== service stack tests ===\n");

    RUN_TEST(builder_build_applies_map_layers_in_order);
    RUN_TEST(builder_stack_roundtrips_over_memory_transport);

    TEST_REPORT();
    return test_failures;
}
