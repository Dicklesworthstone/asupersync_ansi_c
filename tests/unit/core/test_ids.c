/*
 * test_ids.c — unit tests for handle packing/unpacking and type tags
 *
 * SPDX-License-Identifier: MIT
 */

#include "test_harness.h"
#include <asx/asx_ids.h>

/* ------------------------------------------------------------------ */
/* Handle packing round-trip                                           */
/* ------------------------------------------------------------------ */

TEST(handle_pack_unpack_roundtrip) {
    uint16_t type = ASX_TYPE_TASK;
    uint16_t mask = 0x0007;
    uint32_t idx = 42;
    uint64_t h = asx_handle_pack(type, mask, idx);
    ASSERT_EQ(asx_handle_type_tag(h), type);
    ASSERT_EQ(asx_handle_state_mask(h), mask);
    ASSERT_EQ(asx_handle_index(h), idx);
}

TEST(handle_pack_max_values) {
    uint64_t h = asx_handle_pack((uint16_t)0xFFFF, (uint16_t)0xFFFF, (uint32_t)0xFFFFFFFF);
    ASSERT_EQ(asx_handle_type_tag(h), (uint16_t)0xFFFF);
    ASSERT_EQ(asx_handle_state_mask(h), (uint16_t)0xFFFF);
    ASSERT_EQ(asx_handle_index(h), (uint32_t)0xFFFFFFFF);
}

TEST(handle_pack_zero) {
    uint64_t h = asx_handle_pack((uint16_t)0, (uint16_t)0, (uint32_t)0);
    ASSERT_EQ(h, ASX_INVALID_ID);
    ASSERT_EQ(asx_handle_type_tag(h), (uint16_t)0);
    ASSERT_EQ(asx_handle_state_mask(h), (uint16_t)0);
    ASSERT_EQ(asx_handle_index(h), (uint32_t)0);
}

/* ------------------------------------------------------------------ */
/* Validity checks                                                     */
/* ------------------------------------------------------------------ */

TEST(handle_invalid_id) { ASSERT_FALSE(asx_handle_is_valid(ASX_INVALID_ID)); }

TEST(handle_valid_nonzero) {
    uint64_t h = asx_handle_pack(ASX_TYPE_REGION, 0x0001, 1);
    ASSERT_TRUE(asx_handle_is_valid(h));
}

/* ------------------------------------------------------------------ */
/* State admission check                                               */
/* ------------------------------------------------------------------ */

TEST(handle_state_allowed_match) {
    uint64_t h = asx_handle_pack(ASX_TYPE_TASK, 0x0002, 0);
    /* 0x0002 & 0x0003 == 0x0002, nonzero → allowed */
    ASSERT_TRUE(asx_handle_state_allowed(h, 0x0003));
}

TEST(handle_state_allowed_no_match) {
    uint64_t h = asx_handle_pack(ASX_TYPE_TASK, 0x0002, 0);
    /* 0x0002 & 0x0004 == 0x0000, zero → not allowed */
    ASSERT_FALSE(asx_handle_state_allowed(h, 0x0004));
}

/* ------------------------------------------------------------------ */
/* Type tag constants                                                  */
/* ------------------------------------------------------------------ */

TEST(type_tags_distinct) {
    ASSERT_NE(ASX_TYPE_REGION, ASX_TYPE_TASK);
    ASSERT_NE(ASX_TYPE_TASK, ASX_TYPE_OBLIGATION);
    ASSERT_NE(ASX_TYPE_OBLIGATION, ASX_TYPE_CANCEL_WITNESS);
    ASSERT_NE(ASX_TYPE_CANCEL_WITNESS, ASX_TYPE_TIMER);
    ASSERT_NE(ASX_TYPE_TIMER, ASX_TYPE_CHANNEL);
}

TEST(type_tags_nonzero) {
    ASSERT_NE(ASX_TYPE_REGION, (uint16_t)0);
    ASSERT_NE(ASX_TYPE_TASK, (uint16_t)0);
    ASSERT_NE(ASX_TYPE_OBLIGATION, (uint16_t)0);
    ASSERT_NE(ASX_TYPE_CANCEL_WITNESS, (uint16_t)0);
    ASSERT_NE(ASX_TYPE_TIMER, (uint16_t)0);
    ASSERT_NE(ASX_TYPE_CHANNEL, (uint16_t)0);
}

int main(void) {
    fprintf(stderr, "=== test_ids ===\n");
    RUN_TEST(handle_pack_unpack_roundtrip);
    RUN_TEST(handle_pack_max_values);
    RUN_TEST(handle_pack_zero);
    RUN_TEST(handle_invalid_id);
    RUN_TEST(handle_valid_nonzero);
    RUN_TEST(handle_state_allowed_match);
    RUN_TEST(handle_state_allowed_no_match);
    RUN_TEST(type_tags_distinct);
    RUN_TEST(type_tags_nonzero);
    TEST_REPORT();
    return test_failures;
}
