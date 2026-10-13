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

/* ------------------------------------------------------------------ */
/* Runtime tag (K18): mask bits 6-9                                    */
/* ------------------------------------------------------------------ */

TEST(runtime_tag_layout_constants_are_the_documented_ones) {
    ASSERT_EQ(ASX_HANDLE_STATE_BITS, 6u);
    ASSERT_EQ(ASX_HANDLE_STATE_BITS_MASK, (uint16_t)0x003F);
    ASSERT_EQ(ASX_HANDLE_RT_TAG_SHIFT, 6u);
    ASSERT_EQ(ASX_HANDLE_RT_TAG_BITS, 4u);
    ASSERT_EQ(ASX_HANDLE_RT_TAG_MASK, (uint16_t)0x03C0);
    ASSERT_EQ(ASX_HANDLE_RT_TAG_LIMIT, 15u);
}

TEST(runtime_tag_zero_leaves_every_issued_handle_byte_identical) {
    uint16_t s;

    /* Handles are issued with (1 << state) as the mask. */
    for (s = 0; s <= (uint16_t)ASX_REGION_CLOSED; s++) {
        uint64_t h = asx_handle_pack(ASX_TYPE_REGION, (uint16_t)(1u << s), 0x00030002u);
        ASSERT_EQ(asx_handle_runtime_tag(h), (uint16_t)0);
        ASSERT_EQ(asx_handle_with_runtime_tag(h, 0u), h);
    }
    for (s = 0; s <= (uint16_t)ASX_TASK_COMPLETED; s++) {
        uint64_t h = asx_handle_pack(ASX_TYPE_TASK, (uint16_t)(1u << s), 0x00030002u);
        ASSERT_EQ(asx_handle_runtime_tag(h), (uint16_t)0);
        ASSERT_EQ(asx_handle_with_runtime_tag(h, 0u), h);
    }
    for (s = 0; s <= (uint16_t)ASX_OBLIGATION_LEAKED; s++) {
        uint64_t h = asx_handle_pack(ASX_TYPE_OBLIGATION, (uint16_t)(1u << s), 0x00030002u);
        ASSERT_EQ(asx_handle_runtime_tag(h), (uint16_t)0);
        ASSERT_EQ(asx_handle_with_runtime_tag(h, 0u), h);
    }
}

TEST(runtime_tag_roundtrips_for_every_usable_tag_and_changes_only_its_bits) {
    uint64_t h = asx_handle_pack(ASX_TYPE_TASK, 0x0022, 0x00050007u);
    uint16_t tag;

    for (tag = 0; tag < (uint16_t)ASX_HANDLE_RT_TAG_LIMIT; tag++) {
        uint64_t t = asx_handle_with_runtime_tag(h, tag);
        ASSERT_EQ(asx_handle_runtime_tag(t), tag);
        ASSERT_EQ(asx_handle_type_tag(t), ASX_TYPE_TASK);
        ASSERT_EQ(asx_handle_index(t), (uint32_t)0x00050007u);
        ASSERT_EQ((uint16_t)(asx_handle_state_mask(t) & ASX_HANDLE_STATE_BITS_MASK),
                  (uint16_t)0x0022);
        ASSERT_EQ(asx_handle_with_runtime_tag(t, 0u), h);
    }
}

TEST(runtime_tag_never_admits_a_state_and_tagged_handles_differ) {
    uint64_t a = asx_handle_pack(ASX_TYPE_REGION, 0x0001, 0x00000000u);
    uint64_t b = asx_handle_with_runtime_tag(a, 1u);

    /* Same slot and generation, different runtime: different handles. */
    ASSERT_NE(a, b);
    ASSERT_EQ(asx_handle_slot(a), asx_handle_slot(b));
    ASSERT_EQ(asx_handle_generation(a), asx_handle_generation(b));
    ASSERT_TRUE(asx_handle_state_allowed(b, 0x0001));
    ASSERT_FALSE(asx_handle_state_allowed(b, ASX_HANDLE_RT_TAG_MASK));
    ASSERT_FALSE(asx_handle_state_allowed(b, (uint16_t)0xFFC0));
}

int main(void) {
    fprintf(stderr, "=== test_ids ===\n");
    RUN_TEST(runtime_tag_layout_constants_are_the_documented_ones);
    RUN_TEST(runtime_tag_zero_leaves_every_issued_handle_byte_identical);
    RUN_TEST(runtime_tag_roundtrips_for_every_usable_tag_and_changes_only_its_bits);
    RUN_TEST(runtime_tag_never_admits_a_state_and_tagged_handles_differ);
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
