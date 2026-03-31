/*
 * test_types_parity.c — unit tests for public types/config/error/cancel parity
 *
 * Tests the new APIs added for bd-yx9r.4.1:
 *   - Handle type classification helpers
 *   - Cancel witness protocol
 *   - Cancel request + string helpers
 *   - Typed values
 *   - Symbol set iteration
 *   - Resource limits per class
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/asx_config.h>
#include <asx/asx_ids.h>
#include <asx/core/cancel.h>
#include <asx/core/symbol.h>
#include <string.h>

static asx_status st_sink_;
#define MUST_OK(expr)                                                                              \
    do {                                                                                           \
        st_sink_ = (expr);                                                                         \
        (void)st_sink_;                                                                            \
    } while (0)

/* ================================================================== */
/* Handle type classification                                          */
/* ================================================================== */

TEST(handle_type_region) {
    uint64_t h = asx_handle_pack(ASX_TYPE_REGION, 0, 0);
    ASSERT_TRUE(asx_handle_is_region(h));
    ASSERT_FALSE(asx_handle_is_task(h));
    ASSERT_FALSE(asx_handle_is_obligation(h));
    ASSERT_FALSE(asx_handle_is_timer(h));
    ASSERT_FALSE(asx_handle_is_channel(h));
    ASSERT_FALSE(asx_handle_is_cancel_witness(h));
}

TEST(handle_type_task) {
    uint64_t h = asx_handle_pack(ASX_TYPE_TASK, 0, 1);
    ASSERT_TRUE(asx_handle_is_task(h));
    ASSERT_FALSE(asx_handle_is_region(h));
}

TEST(handle_type_obligation) {
    uint64_t h = asx_handle_pack(ASX_TYPE_OBLIGATION, 0, 2);
    ASSERT_TRUE(asx_handle_is_obligation(h));
}

TEST(handle_type_cancel_witness) {
    uint64_t h = asx_handle_pack(ASX_TYPE_CANCEL_WITNESS, 0, 3);
    ASSERT_TRUE(asx_handle_is_cancel_witness(h));
}

TEST(handle_type_timer) {
    uint64_t h = asx_handle_pack(ASX_TYPE_TIMER, 0, 4);
    ASSERT_TRUE(asx_handle_is_timer(h));
}

TEST(handle_type_channel) {
    uint64_t h = asx_handle_pack(ASX_TYPE_CHANNEL, 0, 5);
    ASSERT_TRUE(asx_handle_is_channel(h));
}

TEST(handle_type_invalid) {
    ASSERT_FALSE(asx_handle_is_region(ASX_INVALID_ID));
    ASSERT_FALSE(asx_handle_is_task(ASX_INVALID_ID));
}

TEST(handle_type_str) {
    ASSERT_STR_EQ(asx_handle_type_str(ASX_TYPE_REGION), "region");
    ASSERT_STR_EQ(asx_handle_type_str(ASX_TYPE_TASK), "task");
    ASSERT_STR_EQ(asx_handle_type_str(ASX_TYPE_OBLIGATION), "obligation");
    ASSERT_STR_EQ(asx_handle_type_str(ASX_TYPE_CANCEL_WITNESS), "cancel_witness");
    ASSERT_STR_EQ(asx_handle_type_str(ASX_TYPE_TIMER), "timer");
    ASSERT_STR_EQ(asx_handle_type_str(ASX_TYPE_CHANNEL), "channel");
    ASSERT_STR_EQ(asx_handle_type_str(0xFFFF), "unknown");
}

/* ================================================================== */
/* Cancel witness protocol                                             */
/* ================================================================== */

TEST(witness_not_found) {
    asx_cancel_phase phase;
    ASSERT_EQ(asx_cancel_witness_phase(ASX_INVALID_ID, &phase), ASX_E_NOT_FOUND);
    ASSERT_FALSE(asx_cancel_witness_is_valid(ASX_INVALID_ID));
}

TEST(witness_null_out) {
    ASSERT_EQ(asx_cancel_witness_phase(1, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_cancel_witness_reason(1, NULL), ASX_E_INVALID_ARGUMENT);
}

/* ================================================================== */
/* Cancel request                                                      */
/* ================================================================== */

TEST(cancel_request_null_reason) { ASSERT_EQ(asx_cancel_request(1, NULL), ASX_E_INVALID_ARGUMENT); }

TEST(cancel_request_invalid_task) {
    asx_cancel_reason r;
    memset(&r, 0, sizeof(r));
    r.kind = ASX_CANCEL_USER;
    ASSERT_EQ(asx_cancel_request(ASX_INVALID_ID, &r), ASX_E_NOT_FOUND);
}

TEST(cancel_request_valid) {
    asx_cancel_reason r;
    asx_status st;
    memset(&r, 0, sizeof(r));
    r.kind = ASX_CANCEL_USER;
    r.message = "test";
    /* Task ID 42 doesn't correspond to a spawned task, so the runtime
     * rejects it. This verifies cancel_request delegates to the runtime
     * instead of returning a hardcoded ASX_OK. */
    st = asx_cancel_request(42, &r);
    ASSERT_TRUE(st != ASX_OK);
}

/* ================================================================== */
/* Cancel string helpers                                               */
/* ================================================================== */

TEST(cancel_kind_str) {
    ASSERT_STR_EQ(asx_cancel_kind_str(ASX_CANCEL_USER), "user");
    ASSERT_STR_EQ(asx_cancel_kind_str(ASX_CANCEL_TIMEOUT), "timeout");
    ASSERT_STR_EQ(asx_cancel_kind_str(ASX_CANCEL_DEADLINE), "deadline");
    ASSERT_STR_EQ(asx_cancel_kind_str(ASX_CANCEL_POLL_QUOTA), "poll_quota");
    ASSERT_STR_EQ(asx_cancel_kind_str(ASX_CANCEL_COST_BUDGET), "cost_budget");
    ASSERT_STR_EQ(asx_cancel_kind_str(ASX_CANCEL_FAIL_FAST), "fail_fast");
    ASSERT_STR_EQ(asx_cancel_kind_str(ASX_CANCEL_RACE_LOST), "race_lost");
    ASSERT_STR_EQ(asx_cancel_kind_str(ASX_CANCEL_LINKED_EXIT), "linked_exit");
    ASSERT_STR_EQ(asx_cancel_kind_str(ASX_CANCEL_PARENT), "parent");
    ASSERT_STR_EQ(asx_cancel_kind_str(ASX_CANCEL_RESOURCE), "resource");
    ASSERT_STR_EQ(asx_cancel_kind_str(ASX_CANCEL_SHUTDOWN), "shutdown");
}

TEST(cancel_phase_str) {
    ASSERT_STR_EQ(asx_cancel_phase_str(ASX_CANCEL_PHASE_REQUESTED), "requested");
    ASSERT_STR_EQ(asx_cancel_phase_str(ASX_CANCEL_PHASE_CANCELLING), "cancelling");
    ASSERT_STR_EQ(asx_cancel_phase_str(ASX_CANCEL_PHASE_FINALIZING), "finalizing");
    ASSERT_STR_EQ(asx_cancel_phase_str(ASX_CANCEL_PHASE_COMPLETED), "completed");
}

/* ================================================================== */
/* Typed values                                                        */
/* ================================================================== */

TEST(typed_value_init) {
    asx_typed_value val;
    uint32_t data = 42;
    ASSERT_EQ(asx_typed_value_init(&val, 1, ASX_TYPE_KIND_U32, &data, sizeof(data)), ASX_OK);
    ASSERT_EQ(val.symbol, (asx_symbol_id)1);
    ASSERT_EQ(val.kind, ASX_TYPE_KIND_U32);
    ASSERT_EQ(val.payload_size, (uint32_t)sizeof(data));
}

TEST(typed_value_init_null) {
    ASSERT_EQ(asx_typed_value_init(NULL, 1, ASX_TYPE_KIND_VOID, NULL, 0), ASX_E_INVALID_ARGUMENT);
}

TEST(typed_value_equals_same) {
    uint32_t data = 42;
    asx_typed_value a, b;
    MUST_OK(asx_typed_value_init(&a, 1, ASX_TYPE_KIND_U32, &data, sizeof(data)));
    MUST_OK(asx_typed_value_init(&b, 1, ASX_TYPE_KIND_U32, &data, sizeof(data)));
    ASSERT_TRUE(asx_typed_value_equals(&a, &b));
}

TEST(typed_value_equals_different_symbol) {
    uint32_t data = 42;
    asx_typed_value a, b;
    MUST_OK(asx_typed_value_init(&a, 1, ASX_TYPE_KIND_U32, &data, sizeof(data)));
    MUST_OK(asx_typed_value_init(&b, 2, ASX_TYPE_KIND_U32, &data, sizeof(data)));
    ASSERT_FALSE(asx_typed_value_equals(&a, &b));
}

TEST(typed_value_equals_different_kind) {
    uint32_t data = 42;
    asx_typed_value a, b;
    MUST_OK(asx_typed_value_init(&a, 1, ASX_TYPE_KIND_U32, &data, sizeof(data)));
    MUST_OK(asx_typed_value_init(&b, 1, ASX_TYPE_KIND_I32, &data, sizeof(data)));
    ASSERT_FALSE(asx_typed_value_equals(&a, &b));
}

TEST(typed_value_equals_different_data) {
    uint32_t d1 = 42, d2 = 99;
    asx_typed_value a, b;
    MUST_OK(asx_typed_value_init(&a, 1, ASX_TYPE_KIND_U32, &d1, sizeof(d1)));
    MUST_OK(asx_typed_value_init(&b, 1, ASX_TYPE_KIND_U32, &d2, sizeof(d2)));
    ASSERT_FALSE(asx_typed_value_equals(&a, &b));
}

TEST(typed_value_equals_void) {
    asx_typed_value a, b;
    MUST_OK(asx_typed_value_init(&a, 1, ASX_TYPE_KIND_VOID, NULL, 0));
    MUST_OK(asx_typed_value_init(&b, 1, ASX_TYPE_KIND_VOID, NULL, 0));
    ASSERT_TRUE(asx_typed_value_equals(&a, &b));
}

TEST(typed_value_equals_null) {
    asx_typed_value a;
    MUST_OK(asx_typed_value_init(&a, 1, ASX_TYPE_KIND_VOID, NULL, 0));
    ASSERT_FALSE(asx_typed_value_equals(&a, NULL));
    ASSERT_FALSE(asx_typed_value_equals(NULL, &a));
    ASSERT_TRUE(asx_typed_value_equals(NULL, NULL));
}

/* ================================================================== */
/* Symbol set iteration                                                */
/* ================================================================== */

static int g_iter_count;
static asx_symbol_id g_iter_ids[16];

static int iter_collect(asx_symbol_id id, void *ctx) {
    (void)ctx;
    if (g_iter_count < 16) g_iter_ids[g_iter_count] = id;
    g_iter_count++;
    return 0;
}

TEST(symbol_set_iterate_empty) {
    asx_symbol_set set;
    asx_symbol_set_init(&set);
    g_iter_count = 0;
    asx_symbol_set_iterate(&set, iter_collect, NULL);
    ASSERT_EQ(g_iter_count, 0);
}

TEST(symbol_set_iterate_three) {
    asx_symbol_set set;
    asx_symbol_set_init(&set);
    asx_symbol_set_insert(&set, 3);
    asx_symbol_set_insert(&set, 1);
    asx_symbol_set_insert(&set, 7);
    g_iter_count = 0;
    asx_symbol_set_iterate(&set, iter_collect, NULL);
    ASSERT_EQ(g_iter_count, 3);
    /* Ascending order */
    ASSERT_EQ(g_iter_ids[0], (asx_symbol_id)1);
    ASSERT_EQ(g_iter_ids[1], (asx_symbol_id)3);
    ASSERT_EQ(g_iter_ids[2], (asx_symbol_id)7);
}

static int iter_stop_at_2(asx_symbol_id id, void *ctx) {
    int *count = (int *)ctx;
    (*count)++;
    return (*count >= 2) ? 1 : 0; /* stop after 2 */
}

TEST(symbol_set_iterate_early_stop) {
    asx_symbol_set set;
    int count = 0;
    asx_symbol_set_init(&set);
    asx_symbol_set_insert(&set, 1);
    asx_symbol_set_insert(&set, 2);
    asx_symbol_set_insert(&set, 3);
    asx_symbol_set_insert(&set, 4);
    asx_symbol_set_iterate(&set, iter_stop_at_2, &count);
    ASSERT_EQ(count, 2);
}

TEST(symbol_set_iterate_null_safety) {
    asx_symbol_set set;
    asx_symbol_set_init(&set);
    /* Should not crash */
    asx_symbol_set_iterate(NULL, iter_collect, NULL);
    asx_symbol_set_iterate(&set, NULL, NULL);
}

/* ================================================================== */
/* Resource limits per class                                           */
/* ================================================================== */

TEST(resource_limits_r1) {
    asx_resource_limits lim = asx_resource_limits_for_class(ASX_CLASS_R1);
    ASSERT_EQ(lim.max_regions, 4u);
    ASSERT_EQ(lim.max_tasks, 16u);
    ASSERT_EQ(lim.max_timers, 32u);
    ASSERT_EQ(lim.max_obligations, 16u);
    ASSERT_EQ(lim.max_channels, 8u);
    ASSERT_EQ(lim.max_trace_events, 64u);
}

TEST(resource_limits_r2) {
    asx_resource_limits lim = asx_resource_limits_for_class(ASX_CLASS_R2);
    ASSERT_EQ(lim.max_regions, 16u);
    ASSERT_EQ(lim.max_tasks, 64u);
    ASSERT_EQ(lim.max_timers, 128u);
}

TEST(resource_limits_r3) {
    asx_resource_limits lim = asx_resource_limits_for_class(ASX_CLASS_R3);
    ASSERT_EQ(lim.max_regions, 64u);
    ASSERT_EQ(lim.max_tasks, 256u);
    ASSERT_EQ(lim.max_timers, 512u);
}

TEST(resource_limits_monotone) {
    asx_resource_limits r1 = asx_resource_limits_for_class(ASX_CLASS_R1);
    asx_resource_limits r2 = asx_resource_limits_for_class(ASX_CLASS_R2);
    asx_resource_limits r3 = asx_resource_limits_for_class(ASX_CLASS_R3);
    ASSERT_TRUE(r1.max_regions <= r2.max_regions);
    ASSERT_TRUE(r2.max_regions <= r3.max_regions);
    ASSERT_TRUE(r1.max_tasks <= r2.max_tasks);
    ASSERT_TRUE(r2.max_tasks <= r3.max_tasks);
}

/* ================================================================== */
/* main                                                                */
/* ================================================================== */

int main(void) {
    fprintf(stderr, "test_types_parity\n");

    /* Handle type classification */
    RUN_TEST(handle_type_region);
    RUN_TEST(handle_type_task);
    RUN_TEST(handle_type_obligation);
    RUN_TEST(handle_type_cancel_witness);
    RUN_TEST(handle_type_timer);
    RUN_TEST(handle_type_channel);
    RUN_TEST(handle_type_invalid);
    RUN_TEST(handle_type_str);

    /* Cancel witness protocol */
    RUN_TEST(witness_not_found);
    RUN_TEST(witness_null_out);

    /* Cancel request */
    RUN_TEST(cancel_request_null_reason);
    RUN_TEST(cancel_request_invalid_task);
    RUN_TEST(cancel_request_valid);

    /* Cancel string helpers */
    RUN_TEST(cancel_kind_str);
    RUN_TEST(cancel_phase_str);

    /* Typed values */
    RUN_TEST(typed_value_init);
    RUN_TEST(typed_value_init_null);
    RUN_TEST(typed_value_equals_same);
    RUN_TEST(typed_value_equals_different_symbol);
    RUN_TEST(typed_value_equals_different_kind);
    RUN_TEST(typed_value_equals_different_data);
    RUN_TEST(typed_value_equals_void);
    RUN_TEST(typed_value_equals_null);

    /* Symbol set iteration */
    RUN_TEST(symbol_set_iterate_empty);
    RUN_TEST(symbol_set_iterate_three);
    RUN_TEST(symbol_set_iterate_early_stop);
    RUN_TEST(symbol_set_iterate_null_safety);

    /* Resource limits */
    RUN_TEST(resource_limits_r1);
    RUN_TEST(resource_limits_r2);
    RUN_TEST(resource_limits_r3);
    RUN_TEST(resource_limits_monotone);

    TEST_REPORT();
    return test_failures;
}
