/*
 * ex_gen_server.c — OTP-style gen_server example
 *
 * Demonstrates starting a gen_server with call/cast/stop lifecycle.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <stdio.h>
#include <string.h>

/* --- Counter server callback module --- */

typedef struct {
    int64_t count;
} counter_state;

static counter_state g_state;

static asx_status counter_init(void *args, void **out_state) {
    (void)args;
    g_state.count = 0;
    *out_state = &g_state;
    return ASX_OK;
}

static asx_status counter_call(void *state, uint64_t request, uint64_t *reply) {
    counter_state *cs = (counter_state *)state;
    switch (request) {
    case 0: /* get */
        *reply = (uint64_t)cs->count;
        break;
    case 1: /* increment */
        cs->count++;
        *reply = (uint64_t)cs->count;
        break;
    case 2: /* decrement */
        cs->count--;
        *reply = (uint64_t)cs->count;
        break;
    default:
        return ASX_E_INVALID_ARGUMENT;
    }
    return ASX_OK;
}

static asx_status counter_cast(void *state, uint64_t message) {
    counter_state *cs = (counter_state *)state;
    cs->count += (int64_t)message;
    return ASX_OK;
}

static void counter_terminate(void *state, asx_status reason) {
    (void)state;
    (void)reason;
    printf("SCENARIO gen_server_terminated pass\n");
}

int main(void) {
    static const asx_gen_server_callbacks callbacks = {
        counter_init, counter_call, counter_cast, counter_terminate};
    asx_gen_server_ref ref;
    uint64_t reply;
    asx_status st;

    printf("SCENARIO gen_server_example\n");

    asx_gen_server_reset();
    st = asx_gen_server_start_named(&ref, "counter", &callbacks, NULL);
    if (st != ASX_OK) {
        printf("SCENARIO gen_server_start fail\n");
        return 1;
    }
    printf("SCENARIO gen_server_start pass\n");

    /* Call: increment 3 times */
    st = asx_gen_server_call(ref, 1, &reply); /* +1 = 1 */
    st = asx_gen_server_call(ref, 1, &reply); /* +1 = 2 */
    st = asx_gen_server_call(ref, 1, &reply); /* +1 = 3 */

    /* Call: get */
    st = asx_gen_server_call(ref, 0, &reply);
    printf("SCENARIO gen_server_count_%llu %s\n", (unsigned long long)reply,
           reply == 3 ? "pass" : "fail");

    /* Cast: add 10 */
    st = asx_gen_server_cast(ref, 10);

    /* Call: get */
    st = asx_gen_server_call(ref, 0, &reply);
    printf("SCENARIO gen_server_after_cast_%llu %s\n", (unsigned long long)reply,
           reply == 13 ? "pass" : "fail");

    /* Stop */
    asx_gen_server_stop(ref);
    printf("SCENARIO gen_server_stopped %s\n",
           asx_gen_server_is_finished(ref) ? "pass" : "fail");

    return 0;
}
