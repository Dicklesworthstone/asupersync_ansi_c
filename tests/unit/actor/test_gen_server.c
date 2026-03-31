/*
 * test_gen_server.c — unit tests for OTP-style gen_server
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/actor/gen_server.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Test callback module: counter server                                */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t count;
} counter_state;

static counter_state g_counter;

static asx_status counter_init(void *args, void **out_state) {
    (void)args;
    g_counter.count = 0;
    *out_state = &g_counter;
    return ASX_OK;
}

static asx_status counter_handle_call(void *state, uint64_t request, uint64_t *reply) {
    counter_state *cs = (counter_state *)state;
    if (request == 1) {
        /* increment */
        cs->count++;
        *reply = cs->count;
    } else {
        /* get */
        *reply = cs->count;
    }
    return ASX_OK;
}

static asx_status counter_handle_cast(void *state, uint64_t message) {
    counter_state *cs = (counter_state *)state;
    cs->count += message;
    return ASX_OK;
}

static int g_terminated;
static void counter_terminate(void *state, asx_status reason) {
    (void)state;
    (void)reason;
    g_terminated = 1;
}

static const asx_gen_server_callbacks counter_callbacks = {counter_init, counter_handle_call,
                                                           counter_handle_cast, counter_terminate};

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

TEST(gen_server_start_and_call) {
    asx_gen_server_ref ref;
    uint64_t reply;

    asx_gen_server_reset();
    ASSERT_EQ(asx_gen_server_start(&ref, &counter_callbacks, NULL), ASX_OK);
    ASSERT_EQ(asx_gen_server_get_state(ref), ASX_GEN_SERVER_RUNNING);

    ASSERT_EQ(asx_gen_server_call(ref, 1, &reply), ASX_OK); /* increment */
    ASSERT_EQ(reply, 1u);
    ASSERT_EQ(asx_gen_server_call(ref, 1, &reply), ASX_OK); /* increment */
    ASSERT_EQ(reply, 2u);
    ASSERT_EQ(asx_gen_server_call(ref, 0, &reply), ASX_OK); /* get */
    ASSERT_EQ(reply, 2u);

    asx_gen_server_stop(ref);
}

TEST(gen_server_cast) {
    asx_gen_server_ref ref;
    uint64_t reply;

    asx_gen_server_reset();
    ASSERT_EQ(asx_gen_server_start(&ref, &counter_callbacks, NULL), ASX_OK);

    ASSERT_EQ(asx_gen_server_cast(ref, 10), ASX_OK);
    ASSERT_EQ(asx_gen_server_cast(ref, 5), ASX_OK);
    ASSERT_EQ(asx_gen_server_call(ref, 0, &reply), ASX_OK);
    ASSERT_EQ(reply, 15u);

    asx_gen_server_stop(ref);
}

TEST(gen_server_stop_calls_terminate) {
    asx_gen_server_ref ref;

    asx_gen_server_reset();
    g_terminated = 0;
    ASSERT_EQ(asx_gen_server_start(&ref, &counter_callbacks, NULL), ASX_OK);
    ASSERT_EQ(asx_gen_server_stop(ref), ASX_OK);
    ASSERT_TRUE(g_terminated);
    ASSERT_EQ(asx_gen_server_get_state(ref), ASX_GEN_SERVER_STOPPED);
}

TEST(gen_server_call_after_stop_fails) {
    asx_gen_server_ref ref;
    uint64_t reply;

    asx_gen_server_reset();
    ASSERT_EQ(asx_gen_server_start(&ref, &counter_callbacks, NULL), ASX_OK);
    asx_gen_server_stop(ref);
    ASSERT_EQ(asx_gen_server_call(ref, 0, &reply), ASX_E_INVALID_ARGUMENT);
}

TEST(gen_server_exhaustion) {
    asx_gen_server_ref refs[ASX_GEN_SERVER_MAX_INSTANCES + 1];
    uint32_t i;

    asx_gen_server_reset();
    for (i = 0; i < ASX_GEN_SERVER_MAX_INSTANCES; i++) {
        ASSERT_EQ(asx_gen_server_start(&refs[i], &counter_callbacks, NULL), ASX_OK);
    }
    ASSERT_EQ(asx_gen_server_start(&refs[ASX_GEN_SERVER_MAX_INSTANCES], &counter_callbacks, NULL),
              ASX_E_RESOURCE_EXHAUSTED);
    for (i = 0; i < ASX_GEN_SERVER_MAX_INSTANCES; i++) { asx_gen_server_stop(refs[i]); }
}

TEST(gen_server_null_args) {
    asx_gen_server_ref ref;
    ASSERT_EQ(asx_gen_server_start(NULL, &counter_callbacks, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_gen_server_start(&ref, NULL, NULL), ASX_E_INVALID_ARGUMENT);
}

int main(void) {
    fprintf(stderr, "=== gen_server tests ===\n");
    RUN_TEST(gen_server_start_and_call);
    RUN_TEST(gen_server_cast);
    RUN_TEST(gen_server_stop_calls_terminate);
    RUN_TEST(gen_server_call_after_stop_fails);
    RUN_TEST(gen_server_exhaustion);
    RUN_TEST(gen_server_null_args);
    TEST_REPORT();
    return test_failures;
}
