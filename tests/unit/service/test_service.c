/*
 * test_service.c — unit tests for extended service middleware
 *
 * Tests identity service, map-request, map-response, rate-limit,
 * buffer, and service builder. Uses the asx_service trait from plan.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/service/service.h>
#include <string.h>

/* ================================================================== */
/* Helpers                                                             */
/* ================================================================== */

/* Echo service: copies request pointer into response buffer. */
static asx_service_readiness echo_poll_ready(void *state) {
    (void)state;
    return ASX_SERVICE_READY;
}

static asx_status echo_call(void *state, const void *request, void *response) {
    (void)state;
    /* Store request pointer in response */
    *(const void **)response = request;
    return ASX_OK;
}

static asx_service make_echo_service(void) {
    asx_service svc;
    svc.poll_ready = echo_poll_ready;
    svc.call = echo_call;
    svc.state = NULL;
    return svc;
}

/* Failing service: always returns a given error. */
typedef struct {
    asx_status error;
    uint32_t call_count;
} failing_service_state;

static asx_service_readiness failing_poll_ready(void *state) {
    (void)state;
    return ASX_SERVICE_READY;
}

static asx_status failing_call(void *state, const void *request, void *response) {
    failing_service_state *fs = (failing_service_state *)state;
    (void)request;
    (void)response;
    fs->call_count++;
    return fs->error;
}

static asx_service make_failing_service(failing_service_state *fstate, asx_status error) {
    asx_service svc;
    fstate->error = error;
    fstate->call_count = 0;
    svc.poll_ready = failing_poll_ready;
    svc.call = failing_call;
    svc.state = fstate;
    return svc;
}

/* Not-ready service: poll_ready returns NOT_READY. */
static asx_service_readiness not_ready_poll(void *state) {
    (void)state;
    return ASX_SERVICE_NOT_READY;
}

static asx_service make_not_ready_service(void) {
    asx_service svc;
    svc.poll_ready = not_ready_poll;
    svc.call = echo_call;
    svc.state = NULL;
    return svc;
}

/* Count service: counts calls and returns OK. */
typedef struct {
    uint32_t call_count;
} count_service_state;

static asx_service_readiness count_poll_ready(void *state) {
    (void)state;
    return ASX_SERVICE_READY;
}

static asx_status count_call(void *state, const void *request, void *response) {
    count_service_state *cs = (count_service_state *)state;
    cs->call_count++;
    *(const void **)response = request;
    return ASX_OK;
}

static asx_service make_count_service(count_service_state *cstate) {
    asx_service svc;
    cstate->call_count = 0;
    svc.poll_ready = count_poll_ready;
    svc.call = count_call;
    svc.state = cstate;
    return svc;
}

typedef struct {
    uint32_t poll_count;
    uint32_t call_count;
    asx_status first_call_status;
} staged_buffer_service_state;

static asx_service_readiness staged_buffer_poll_ready(void *state) {
    staged_buffer_service_state *svc = (staged_buffer_service_state *)state;
    svc->poll_count++;
    if (svc->poll_count == 1u) { return ASX_SERVICE_NOT_READY; }
    return ASX_SERVICE_READY;
}

static asx_status staged_buffer_call(void *state, const void *request, void *response) {
    staged_buffer_service_state *svc = (staged_buffer_service_state *)state;
    svc->call_count++;
    if (svc->call_count == 1u) { return svc->first_call_status; }
    *(const void **)response = request;
    return ASX_OK;
}

static asx_service make_staged_buffer_service(staged_buffer_service_state *state,
                                              asx_status first) {
    asx_service svc;
    state->poll_count = 0u;
    state->call_count = 0u;
    state->first_call_status = first;
    svc.poll_ready = staged_buffer_poll_ready;
    svc.call = staged_buffer_call;
    svc.state = state;
    return svc;
}

/* ================================================================== */
/* Identity service tests                                              */
/* ================================================================== */

TEST(identity_returns_request_as_response) {
    asx_service svc;
    asx_service_identity_state state;
    int req = 42;
    const void *resp = NULL;

    asx_service_identity_init(&svc, &state);

    ASSERT_EQ(asx_service_poll_ready(&svc), ASX_SERVICE_READY);
    ASSERT_EQ(asx_service_call(&svc, &req, &resp), ASX_OK);
    ASSERT_TRUE(resp == &req);
    ASSERT_EQ(*(const int *)resp, 42);
}

TEST(identity_always_ready) {
    asx_service svc;
    asx_service_identity_state state;
    int i;

    asx_service_identity_init(&svc, &state);

    for (i = 0; i < 10; i++) { ASSERT_EQ(asx_service_poll_ready(&svc), ASX_SERVICE_READY); }
}

/* ================================================================== */
/* Map-request tests                                                   */
/* ================================================================== */

static int doubled_val;

static const void *double_int_request(const void *request, void *user_data) {
    (void)user_data;
    doubled_val = *(const int *)request * 2;
    return &doubled_val;
}

TEST(map_request_transforms_input) {
    asx_service inner = make_echo_service();
    asx_service svc;
    asx_service_map_request_state state;
    int req = 21;
    const void *resp = NULL;

    asx_service_map_request_init(&svc, &state, inner, double_int_request, NULL);

    ASSERT_EQ(asx_service_call(&svc, &req, &resp), ASX_OK);
    ASSERT_EQ(*(const int *)resp, 42);
}

/* ================================================================== */
/* Map-response tests                                                  */
/* ================================================================== */

static void negate_response(void *response, void *user_data) {
    const void **rp = (const void **)response;
    static int negated;
    (void)user_data;
    negated = -(*(const int *)*rp);
    *rp = &negated;
}

TEST(map_response_transforms_output) {
    asx_service inner = make_echo_service();
    asx_service svc;
    asx_service_map_response_state state;
    int req = 7;
    const void *resp = NULL;

    asx_service_map_response_init(&svc, &state, inner, negate_response, NULL);

    ASSERT_EQ(asx_service_call(&svc, &req, &resp), ASX_OK);
    ASSERT_EQ(*(const int *)resp, -7);
}

/* ================================================================== */
/* Rate-limit tests (using existing plan.h service)                    */
/* ================================================================== */

TEST(rate_limit_allows_within_budget) {
    count_service_state cstate;
    asx_service inner = make_count_service(&cstate);
    asx_service svc;
    asx_service_rate_limit_state state;
    int req = 1;
    const void *resp = NULL;
    uint32_t i;

    /* Allow 5 calls per window. */
    asx_service_rate_limit_init(&svc, &state, inner, 5, 1000000000ULL);

    for (i = 0; i < 5; i++) { ASSERT_EQ(asx_service_call(&svc, &req, &resp), ASX_OK); }
    ASSERT_EQ(cstate.call_count, 5u);
}

TEST(rate_limit_rejects_over_budget) {
    count_service_state cstate;
    asx_service inner = make_count_service(&cstate);
    asx_service svc;
    asx_service_rate_limit_state state;
    int req = 1;
    const void *resp = NULL;
    uint32_t i;

    asx_service_rate_limit_init(&svc, &state, inner, 3, 1000000000ULL);

    for (i = 0; i < 3; i++) { ASSERT_EQ(asx_service_call(&svc, &req, &resp), ASX_OK); }

    ASSERT_EQ(asx_service_call(&svc, &req, &resp), ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(cstate.call_count, 3u);
}

TEST(rate_limit_remaining_decrements) {
    count_service_state cstate;
    asx_service inner = make_count_service(&cstate);
    asx_service svc;
    asx_service_rate_limit_state state;
    int req = 1;
    const void *resp = NULL;

    asx_service_rate_limit_init(&svc, &state, inner, 4, 1000000000ULL);

    ASSERT_EQ(asx_service_rate_limit_remaining(&state), 4u);
    ASSERT_EQ(asx_service_call(&svc, &req, &resp), ASX_OK);
    ASSERT_EQ(asx_service_rate_limit_remaining(&state), 3u);
    ASSERT_EQ(asx_service_call(&svc, &req, &resp), ASX_OK);
    ASSERT_EQ(asx_service_rate_limit_remaining(&state), 2u);
}

/* ================================================================== */
/* Load-shed tests (using existing plan.h layer)                       */
/* ================================================================== */

TEST(load_shed_passes_when_ready) {
    asx_service inner = make_echo_service();
    asx_service svc;
    asx_load_shed_service_state state;
    int req = 5;
    const void *resp = NULL;

    asx_load_shed_layer_init(&svc, &state, inner);

    ASSERT_EQ(asx_service_call(&svc, &req, &resp), ASX_OK);
    ASSERT_EQ(*(const int *)resp, 5);
}

TEST(load_shed_rejects_when_not_ready) {
    asx_service inner = make_not_ready_service();
    asx_service svc;
    asx_load_shed_service_state state;
    int req = 5;
    const void *resp = NULL;

    asx_load_shed_layer_init(&svc, &state, inner);

    ASSERT_EQ(asx_service_call(&svc, &req, &resp), ASX_E_WOULD_BLOCK);
}

/* ================================================================== */
/* Buffer tests                                                        */
/* ================================================================== */

TEST(buffer_direct_call_when_ready) {
    asx_service inner = make_echo_service();
    asx_service svc;
    asx_service_buffer_state state;
    int req = 7;
    const void *resp = NULL;

    asx_service_buffer_init(&svc, &state, inner);

    ASSERT_EQ(asx_service_buffer_pending(&state), 0u);
    ASSERT_EQ(asx_service_call(&svc, &req, &resp), ASX_OK);
    ASSERT_EQ(*(const int *)resp, 7);
    ASSERT_EQ(asx_service_buffer_pending(&state), 0u);
}

TEST(buffer_enqueues_when_not_ready) {
    asx_service inner = make_not_ready_service();
    asx_service svc;
    asx_service_buffer_state state;
    int req = 3;
    const void *resp = NULL;

    asx_service_buffer_init(&svc, &state, inner);

    ASSERT_EQ(asx_service_call(&svc, &req, &resp), ASX_E_PENDING);
    ASSERT_EQ(asx_service_buffer_pending(&state), 1u);
}

TEST(buffer_flush_failure_does_not_drop_queued_request) {
    staged_buffer_service_state inner_state;
    asx_service inner = make_staged_buffer_service(&inner_state, ASX_E_PENDING);
    asx_service svc;
    asx_service_buffer_state state;
    int first_req = 3;
    int second_req = 4;
    const void *resp = NULL;

    asx_service_buffer_init(&svc, &state, inner);

    ASSERT_EQ(asx_service_call(&svc, &first_req, &resp), ASX_E_PENDING);
    ASSERT_EQ(asx_service_buffer_pending(&state), 1u);

    ASSERT_EQ(asx_service_call(&svc, &second_req, &resp), ASX_E_PENDING);
    ASSERT_EQ(asx_service_buffer_pending(&state), 1u);
    ASSERT_EQ(inner_state.call_count, 1u);
}

/* ================================================================== */
/* Retry tests (using existing plan.h retry layer)                     */
/* ================================================================== */

TEST(retry_succeeds_on_first_try) {
    asx_service inner = make_echo_service();
    asx_service svc;
    asx_retry_service_state state;
    int req = 99;
    const void *resp = NULL;

    asx_retry_layer_init(&svc, &state, inner, 3);

    ASSERT_EQ(asx_service_call(&svc, &req, &resp), ASX_OK);
    ASSERT_EQ(*(const int *)resp, 99);
}

TEST(retry_exhausts_retries) {
    failing_service_state fstate;
    asx_service inner = make_failing_service(&fstate, ASX_E_DISCONNECTED);
    asx_service svc;
    asx_retry_service_state state;
    int req = 1;
    const void *resp = NULL;
    asx_status st;

    asx_retry_layer_init(&svc, &state, inner, 3);

    st = asx_service_call(&svc, &req, &resp);
    /* ASX_E_DISCONNECTED is not retryable per taxonomy, so should fail immediately. */
    ASSERT_EQ(st, ASX_E_DISCONNECTED);
}

/* ================================================================== */
/* Service builder tests                                               */
/* ================================================================== */

TEST(builder_init_empty) {
    asx_service_builder builder;

    asx_service_builder_init(&builder);
    ASSERT_EQ(asx_service_builder_layer_count(&builder), 0u);
}

TEST(builder_add_layers) {
    asx_service_builder builder;
    asx_status st;

    asx_service_builder_init(&builder);

    st = asx_service_builder_timeout(&builder, 5000000000ULL);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_EQ(asx_service_builder_layer_count(&builder), 1u);

    st = asx_service_builder_retry(&builder, 3);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_EQ(asx_service_builder_layer_count(&builder), 2u);

    st = asx_service_builder_rate_limit(&builder, 100, 1000000000ULL);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_EQ(asx_service_builder_layer_count(&builder), 3u);

    st = asx_service_builder_load_shed(&builder);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_EQ(asx_service_builder_layer_count(&builder), 4u);

    st = asx_service_builder_buffer(&builder);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_EQ(asx_service_builder_layer_count(&builder), 5u);
}

TEST(builder_rejects_overflow) {
    asx_service_builder builder;
    uint32_t i;
    asx_status st;

    asx_service_builder_init(&builder);

    for (i = 0; i < ASX_SERVICE_BUILDER_MAX_LAYERS; i++) {
        st = asx_service_builder_load_shed(&builder);
        ASSERT_EQ(st, ASX_OK);
    }

    st = asx_service_builder_load_shed(&builder);
    ASSERT_EQ(st, ASX_E_RESOURCE_EXHAUSTED);
}

/* ================================================================== */
/* Composition tests                                                   */
/* ================================================================== */

TEST(map_request_wrapping_echo) {
    asx_service echo = make_echo_service();
    asx_service mapped;
    asx_service_map_request_state map_state;
    int req = 10;
    const void *resp = NULL;

    asx_service_map_request_init(&mapped, &map_state, echo, double_int_request, NULL);

    ASSERT_EQ(asx_service_call(&mapped, &req, &resp), ASX_OK);
    ASSERT_EQ(*(const int *)resp, 20);
}

/* ================================================================== */
/* Concurrency limit tests                                             */
/* ================================================================== */

TEST(concurrency_limit_allows_under_limit) {
    asx_service echo = make_echo_service();
    asx_service limited;
    asx_service_concurrency_limit_state cls;
    int req = 42;
    const void *resp = NULL;

    asx_service_concurrency_limit_init(&limited, &cls, echo, 2);
    ASSERT_EQ(asx_service_call(&limited, &req, &resp), ASX_OK);
    ASSERT_EQ(*(const int *)resp, 42);
    ASSERT_EQ(asx_service_concurrency_limit_inflight(&cls), 1u);
    asx_service_concurrency_limit_release(&cls);
    ASSERT_EQ(asx_service_concurrency_limit_inflight(&cls), 0u);
}

TEST(concurrency_limit_rejects_at_limit) {
    asx_service echo = make_echo_service();
    asx_service limited;
    asx_service_concurrency_limit_state cls;
    int req = 1;
    const void *resp = NULL;

    asx_service_concurrency_limit_init(&limited, &cls, echo, 1);
    ASSERT_EQ(asx_service_call(&limited, &req, &resp), ASX_OK);
    /* Now at limit — next call should be rejected */
    ASSERT_EQ(asx_service_call(&limited, &req, &resp), ASX_E_OVERLOADED);
}

TEST(concurrency_limit_release_frees_slot) {
    asx_service echo = make_echo_service();
    asx_service limited;
    asx_service_concurrency_limit_state cls;
    int req = 1;
    const void *resp = NULL;

    asx_service_concurrency_limit_init(&limited, &cls, echo, 1);
    ASSERT_EQ(asx_service_call(&limited, &req, &resp), ASX_OK);
    ASSERT_EQ(asx_service_call(&limited, &req, &resp), ASX_E_OVERLOADED);
    asx_service_concurrency_limit_release(&cls);
    ASSERT_EQ(asx_service_call(&limited, &req, &resp), ASX_OK);
}

/* ================================================================== */
/* Filter tests                                                        */
/* ================================================================== */

static int filter_positive(const void *request, void *user_data) {
    (void)user_data;
    return *(const int *)request > 0;
}

TEST(filter_passes_matching) {
    asx_service echo = make_echo_service();
    asx_service filtered;
    asx_service_filter_state fs;
    int req = 42;
    const void *resp = NULL;

    asx_service_filter_init(&filtered, &fs, echo, filter_positive, NULL);
    ASSERT_EQ(asx_service_call(&filtered, &req, &resp), ASX_OK);
    ASSERT_EQ(*(const int *)resp, 42);
}

TEST(filter_rejects_non_matching) {
    asx_service echo = make_echo_service();
    asx_service filtered;
    asx_service_filter_state fs;
    int req = -5;
    const void *resp = NULL;

    asx_service_filter_init(&filtered, &fs, echo, filter_positive, NULL);
    ASSERT_EQ(asx_service_call(&filtered, &req, &resp), ASX_E_PERMISSION_DENIED);
}

/* ================================================================== */
/* Load balance tests                                                  */
/* ================================================================== */

static int lb_call_count_a;
static int lb_call_count_b;

static asx_status lb_echo_a(void *state, const void *request, void *response) {
    (void)state;
    (void)request;
    (void)response;
    lb_call_count_a++;
    return ASX_OK;
}

static asx_status lb_echo_b(void *state, const void *request, void *response) {
    (void)state;
    (void)request;
    (void)response;
    lb_call_count_b++;
    return ASX_OK;
}

TEST(load_balance_round_robin) {
    asx_service lb;
    asx_service_load_balance_state lbs;
    asx_service a, b;
    int req = 1;
    int resp = 0;

    a.poll_ready = echo_poll_ready;
    a.call = lb_echo_a;
    a.state = NULL;
    b.poll_ready = echo_poll_ready;
    b.call = lb_echo_b;
    b.state = NULL;

    lb_call_count_a = 0;
    lb_call_count_b = 0;

    asx_service_load_balance_init(&lb, &lbs);
    ASSERT_EQ(asx_service_load_balance_add_backend(&lbs, a), ASX_OK);
    ASSERT_EQ(asx_service_load_balance_add_backend(&lbs, b), ASX_OK);
    ASSERT_EQ(asx_service_load_balance_backend_count(&lbs), 2u);

    /* 4 calls should alternate: a, b, a, b */
    ASSERT_EQ(asx_service_call(&lb, &req, &resp), ASX_OK);
    ASSERT_EQ(asx_service_call(&lb, &req, &resp), ASX_OK);
    ASSERT_EQ(asx_service_call(&lb, &req, &resp), ASX_OK);
    ASSERT_EQ(asx_service_call(&lb, &req, &resp), ASX_OK);
    ASSERT_EQ(lb_call_count_a, 2);
    ASSERT_EQ(lb_call_count_b, 2);
}

TEST(load_balance_empty_fails) {
    asx_service lb;
    asx_service_load_balance_state lbs;
    int req = 1;
    int resp = 0;

    asx_service_load_balance_init(&lb, &lbs);
    ASSERT_EQ(asx_service_call(&lb, &req, &resp), ASX_E_INVALID_STATE);
}

/* ================================================================== */
/* Reconnect tests                                                     */
/* ================================================================== */

static int reconnect_factory_calls;

static asx_status reconnect_make_echo(void *factory_data, asx_service *out) {
    (void)factory_data;
    reconnect_factory_calls++;
    out->poll_ready = echo_poll_ready;
    out->call = echo_call;
    out->state = NULL;
    return ASX_OK;
}

TEST(reconnect_retries_on_failure) {
    asx_service svc;
    asx_service_reconnect_state rs;
    failing_service_state fs;
    asx_service failing;
    int req = 42;
    const void *resp = NULL;

    reconnect_factory_calls = 0;
    failing = make_failing_service(&fs, ASX_E_DISCONNECTED);

    asx_service_reconnect_init(&svc, &rs, failing, reconnect_make_echo, NULL, 3);
    /* First call: inner fails, reconnects to echo, retries, succeeds */
    ASSERT_EQ(asx_service_call(&svc, &req, &resp), ASX_OK);
    ASSERT_EQ(*(const int *)resp, 42);
    ASSERT_EQ(reconnect_factory_calls, 1);
    ASSERT_EQ(asx_service_reconnect_count(&rs), 1u);
}

TEST(reconnect_respects_limit) {
    asx_service svc;
    asx_service_reconnect_state rs;
    failing_service_state fs;
    asx_service failing;
    int req = 1;
    const void *resp = NULL;

    reconnect_factory_calls = 0;
    failing = make_failing_service(&fs, ASX_E_DISCONNECTED);

    /* max_reconnects = 0 means unlimited */
    asx_service_reconnect_init(&svc, &rs, failing, reconnect_make_echo, NULL, 0);
    ASSERT_EQ(asx_service_call(&svc, &req, &resp), ASX_OK); /* reconnects */
    ASSERT_EQ(asx_service_reconnect_count(&rs), 1u);
}

/* ================================================================== */
/* Steer tests                                                         */
/* ================================================================== */

static uint32_t steer_by_value(const void *request, uint32_t backend_count, void *user_data) {
    int val = *(const int *)request;
    (void)user_data;
    return (uint32_t)(val % (int)backend_count);
}

TEST(steer_routes_by_key) {
    asx_service svc;
    asx_service_steer_state ss;
    asx_service a, b;
    int req;
    int resp = 0;

    a.poll_ready = echo_poll_ready;
    a.call = lb_echo_a;
    a.state = NULL;
    b.poll_ready = echo_poll_ready;
    b.call = lb_echo_b;
    b.state = NULL;

    lb_call_count_a = 0;
    lb_call_count_b = 0;

    asx_service_steer_init(&svc, &ss, steer_by_value, NULL);
    ASSERT_EQ(asx_service_steer_add_backend(&ss, a), ASX_OK);
    ASSERT_EQ(asx_service_steer_add_backend(&ss, b), ASX_OK);

    /* Even numbers go to a (idx 0), odd to b (idx 1) */
    req = 0;
    ASSERT_EQ(asx_service_call(&svc, &req, &resp), ASX_OK);
    req = 1;
    ASSERT_EQ(asx_service_call(&svc, &req, &resp), ASX_OK);
    req = 2;
    ASSERT_EQ(asx_service_call(&svc, &req, &resp), ASX_OK);
    req = 3;
    ASSERT_EQ(asx_service_call(&svc, &req, &resp), ASX_OK);
    ASSERT_EQ(lb_call_count_a, 2);
    ASSERT_EQ(lb_call_count_b, 2);
}

/* ================================================================== */
/* main                                                                */
/* ================================================================== */

int main(void) {
    fprintf(stderr, "=== service tests ===\n");

    /* Identity */
    RUN_TEST(identity_returns_request_as_response);
    RUN_TEST(identity_always_ready);

    /* Map-request */
    RUN_TEST(map_request_transforms_input);

    /* Map-response */
    RUN_TEST(map_response_transforms_output);

    /* Rate-limit */
    RUN_TEST(rate_limit_allows_within_budget);
    RUN_TEST(rate_limit_rejects_over_budget);
    RUN_TEST(rate_limit_remaining_decrements);

    /* Load-shed (plan.h layer) */
    RUN_TEST(load_shed_passes_when_ready);
    RUN_TEST(load_shed_rejects_when_not_ready);

    /* Buffer */
    RUN_TEST(buffer_direct_call_when_ready);
    RUN_TEST(buffer_enqueues_when_not_ready);
    RUN_TEST(buffer_flush_failure_does_not_drop_queued_request);

    /* Retry (plan.h layer) */
    RUN_TEST(retry_succeeds_on_first_try);
    RUN_TEST(retry_exhausts_retries);

    /* Builder */
    RUN_TEST(builder_init_empty);
    RUN_TEST(builder_add_layers);
    RUN_TEST(builder_rejects_overflow);

    /* Composition */
    RUN_TEST(map_request_wrapping_echo);

    /* Concurrency limit */
    RUN_TEST(concurrency_limit_allows_under_limit);
    RUN_TEST(concurrency_limit_rejects_at_limit);
    RUN_TEST(concurrency_limit_release_frees_slot);

    /* Filter */
    RUN_TEST(filter_passes_matching);
    RUN_TEST(filter_rejects_non_matching);

    /* Load balance */
    RUN_TEST(load_balance_round_robin);
    RUN_TEST(load_balance_empty_fails);

    /* Reconnect */
    RUN_TEST(reconnect_retries_on_failure);
    RUN_TEST(reconnect_respects_limit);

    /* Steer */
    RUN_TEST(steer_routes_by_key);

    TEST_REPORT();
    return test_failures;
}
