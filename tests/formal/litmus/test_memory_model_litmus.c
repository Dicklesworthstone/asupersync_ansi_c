/*
 * test_memory_model_litmus.c — Memory-model litmus suite (bd-3vt.4)
 *
 * Verifies critical assumptions the asx runtime relies on across
 * compilers and optimization levels. Since the codebase is single-threaded
 * and synchronization-free, these tests focus on:
 *
 * 1. Type layout stability (sizes, alignment, endianness)
 * 2. Compiler optimization safety (observable side-effects preserved)
 * 3. Integer arithmetic assumptions (overflow, signedness, shifts)
 * 4. Struct packing and padding consistency
 * 5. Enum representation guarantees
 * 6. Function pointer call-through semantics
 *
 * These must pass on every compiler/target/optimization-level combination.
 *
 * SPDX-License-Identifier: MIT
 */

/* ASX_CHECKPOINT_WAIVER_FILE("Litmus test — memory model verification only") */

#include "../../test_harness.h"
#include <asx/asx.h>
#include <limits.h>
#include <string.h>

/* -----------------------------------------------------------------------
 * LITMUS-1: Type sizes match ABI contract
 * Assumption: handle types are exactly 8 bytes, status is 4 bytes
 * ----------------------------------------------------------------------- */
TEST(litmus_type_sizes) {
    ASSERT_EQ((int)sizeof(asx_region_id), 8);
    ASSERT_EQ((int)sizeof(asx_task_id), 8);
    ASSERT_EQ((int)sizeof(asx_obligation_id), 8);
    ASSERT_EQ((int)sizeof(asx_timer_id), 8);
    ASSERT_EQ((int)sizeof(asx_channel_id), 8);
    ASSERT_EQ((int)sizeof(asx_status), 4);
    ASSERT_EQ((int)sizeof(asx_time), 8);
}

/* -----------------------------------------------------------------------
 * LITMUS-2: Unsigned integer overflow wraps (C99 guarantees this)
 * Assumption: unsigned arithmetic wraps modulo 2^N
 * ----------------------------------------------------------------------- */
TEST(litmus_unsigned_wrap) {
    uint32_t a = UINT32_MAX;
    uint32_t b = a + 1;
    ASSERT_EQ((int)b, 0);

    uint16_t c = UINT16_MAX;
    uint16_t d = (uint16_t)(c + 1);
    ASSERT_EQ((int)d, 0);

    uint64_t e = UINT64_MAX;
    uint64_t f = e + 1;
    ASSERT_TRUE(f == 0);
}

/* -----------------------------------------------------------------------
 * LITMUS-3: Handle packing is endian-agnostic via shift/mask
 * Assumption: bit-field packing via shifts produces portable results
 * ----------------------------------------------------------------------- */
TEST(litmus_handle_pack_unpack) {
    /* Pack: [type_tag:16][state_mask:16][gen:16][slot:16] */
    uint16_t type_tag = 0x1234;
    uint16_t state_mask = 0x5678;
    uint16_t gen = 0x9ABC;
    uint16_t slot = 0xDEF0;

    uint64_t packed = ((uint64_t)type_tag << 48) | ((uint64_t)state_mask << 32) |
                      ((uint64_t)gen << 16) | (uint64_t)slot;

    /* Unpack */
    uint16_t out_type = (uint16_t)(packed >> 48);
    uint16_t out_state = (uint16_t)(packed >> 32);
    uint16_t out_gen = (uint16_t)(packed >> 16);
    uint16_t out_slot = (uint16_t)(packed);

    ASSERT_EQ((int)out_type, (int)type_tag);
    ASSERT_EQ((int)out_state, (int)state_mask);
    ASSERT_EQ((int)out_gen, (int)gen);
    ASSERT_EQ((int)out_slot, (int)slot);
}

/* -----------------------------------------------------------------------
 * LITMUS-4: Enum representation is int-compatible
 * Assumption: enums fit in int, values match explicit assignments
 * ----------------------------------------------------------------------- */
TEST(litmus_enum_representation) {
    /* Region states are contiguous 0..4 */
    ASSERT_EQ((int)ASX_REGION_OPEN, 0);
    ASSERT_EQ((int)ASX_REGION_CLOSED, 4);
    ASSERT_TRUE(sizeof(asx_region_state) <= sizeof(int));

    /* Task states are contiguous 0..5 */
    ASSERT_EQ((int)ASX_TASK_CREATED, 0);
    ASSERT_EQ((int)ASX_TASK_COMPLETED, 5);
    ASSERT_TRUE(sizeof(asx_task_state) <= sizeof(int));

    /* Status codes have specific values */
    ASSERT_EQ((int)ASX_OK, 0);
    ASSERT_EQ((int)ASX_E_PENDING, 1);
    ASSERT_EQ((int)ASX_E_INVALID_ARGUMENT, 100);
    ASSERT_EQ((int)ASX_E_INVALID_TRANSITION, 200);
}

/* -----------------------------------------------------------------------
 * LITMUS-5: Signed/unsigned cast preserves bit pattern
 * Assumption: casting between signed and unsigned of same width
 * preserves bit pattern (C99 guarantees for two's complement)
 * ----------------------------------------------------------------------- */
TEST(litmus_signed_unsigned_cast) {
    int32_t neg = -1;
    uint32_t as_uint = (uint32_t)neg;
    ASSERT_EQ(as_uint, UINT32_MAX);

    uint32_t big = UINT32_MAX;
    int32_t as_int = (int32_t)big;
    ASSERT_EQ(as_int, -1);
}

/* -----------------------------------------------------------------------
 * LITMUS-6: memset zero produces valid zero-initialized structs
 * Assumption: memset(0) yields all-bits-zero which is a valid
 * representation for integers, pointers, and enums
 * ----------------------------------------------------------------------- */
TEST(litmus_memset_zero_init) {
    asx_budget b;
    memset(&b, 0, sizeof(b));
    ASSERT_EQ((int)b.poll_quota, 0);
    ASSERT_TRUE(b.cost_quota == 0);
    ASSERT_TRUE(b.deadline == 0);
    ASSERT_EQ((int)b.priority, 0);

    asx_outcome o;
    memset(&o, 0, sizeof(o));
    ASSERT_EQ((int)o.severity, (int)ASX_OUTCOME_OK);

    asx_cancel_reason r;
    memset(&r, 0, sizeof(r));
    ASSERT_EQ((int)r.kind, (int)ASX_CANCEL_USER);
}

/* -----------------------------------------------------------------------
 * LITMUS-7: Function pointer identity
 * Assumption: function pointers are comparable and non-NULL when
 * pointing to real functions
 * ----------------------------------------------------------------------- */
static asx_status dummy_poll(void *ud, asx_task_id self) {
    (void)ud;
    (void)self;
    return ASX_OK;
}

TEST(litmus_function_pointer_identity) {
    asx_status (*p1)(void *, asx_task_id) = dummy_poll;
    asx_status (*p2)(void *, asx_task_id) = dummy_poll;

    ASSERT_TRUE(p1 != NULL);
    ASSERT_TRUE(p1 == p2);

    asx_status (*null_fn)(void *, asx_task_id) = NULL;
    ASSERT_TRUE(null_fn == NULL);
}

/* -----------------------------------------------------------------------
 * LITMUS-8: Array indexing with enum values
 * Assumption: using enum values directly as array indices is safe
 * when values are contiguous and bounded
 * ----------------------------------------------------------------------- */
TEST(litmus_enum_as_array_index) {
    static const char *region_names[] = {"Open", "Closing", "Draining", "Finalizing", "Closed"};

    int i;
    for (i = (int)ASX_REGION_OPEN; i <= (int)ASX_REGION_CLOSED; i++) {
        ASSERT_TRUE(region_names[i] != NULL);
    }

    /* Verify contiguity */
    ASSERT_EQ((int)ASX_REGION_CLOSED - (int)ASX_REGION_OPEN, 4);
    ASSERT_EQ((int)ASX_TASK_COMPLETED - (int)ASX_TASK_CREATED, 5);
    ASSERT_EQ((int)ASX_OBLIGATION_LEAKED - (int)ASX_OBLIGATION_RESERVED, 3);
}

/* -----------------------------------------------------------------------
 * LITMUS-9: Struct size-field pattern for forward compatibility
 * Assumption: sizeof(struct) is stable per compilation unit
 * ----------------------------------------------------------------------- */
TEST(litmus_size_field_pattern) {
    asx_runtime_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.size = (uint32_t)sizeof(cfg);

    ASSERT_TRUE(cfg.size > 0);
    ASSERT_EQ(cfg.size, (uint32_t)sizeof(asx_runtime_config));

    /* Size is stable across multiple evaluations */
    ASSERT_EQ((uint32_t)sizeof(asx_runtime_config), (uint32_t)sizeof(asx_runtime_config));
}

/* -----------------------------------------------------------------------
 * LITMUS-10: Transition table lookup produces consistent results
 * Assumption: calling transition_check with same inputs always
 * returns the same result (no hidden state, no optimization clobber)
 * ----------------------------------------------------------------------- */
TEST(litmus_transition_determinism) {
    int i;
    /* Call the same transition check 100 times — must be identical */
    for (i = 0; i < 100; i++) {
        asx_status st1 = asx_region_transition_check(ASX_REGION_OPEN, ASX_REGION_CLOSING);
        asx_status st2 = asx_region_transition_check(ASX_REGION_CLOSED, ASX_REGION_OPEN);
        ASSERT_EQ(st1, ASX_OK);
        ASSERT_EQ(st2, ASX_E_INVALID_TRANSITION);
    }
}

/* -----------------------------------------------------------------------
 * LITMUS-11: NULL pointer is detectable
 * Assumption: NULL == 0 and is always false in boolean context
 * ----------------------------------------------------------------------- */
TEST(litmus_null_pointer) {
    void *p = NULL;
    ASSERT_TRUE(p == NULL);
    ASSERT_TRUE(!p);
    ASSERT_TRUE(p == 0);
}

/* -----------------------------------------------------------------------
 * LITMUS-12: Bitwise operations on uint64_t
 * Assumption: bitwise OR, AND, XOR, and shifts work correctly
 * on 64-bit types across all targets
 * ----------------------------------------------------------------------- */
TEST(litmus_bitwise_uint64) {
    uint64_t a = 0xDEADBEEFCAFEBABEull;
    uint64_t b = 0x1234567890ABCDEFull;

    /* OR */
    uint64_t or_result = a | b;
    ASSERT_TRUE((or_result & a) == a);
    ASSERT_TRUE((or_result & b) == b);

    /* AND */
    uint64_t and_result = a & b;
    ASSERT_TRUE((and_result | a) == a);

    /* XOR self is zero */
    ASSERT_TRUE((a ^ a) == 0);

    /* Shift roundtrip */
    uint64_t val = 0x42ull;
    ASSERT_TRUE((val << 32 >> 32) == val);
}

/* -----------------------------------------------------------------------
 * LITMUS-13: CHAR_BIT is 8
 * Assumption: the codebase assumes 8-bit bytes throughout
 * ----------------------------------------------------------------------- */
TEST(litmus_char_bit_is_8) {
    ASSERT_EQ(CHAR_BIT, 8);
    ASSERT_EQ((int)sizeof(uint8_t), 1);
    ASSERT_EQ((int)sizeof(uint16_t), 2);
    ASSERT_EQ((int)sizeof(uint32_t), 4);
    ASSERT_EQ((int)sizeof(uint64_t), 8);
}

/* -----------------------------------------------------------------------
 * LITMUS-14: Outcome join is stable across optimization levels
 * Assumption: compiler optimization doesn't change observable behavior
 * of the outcome join operation
 * ----------------------------------------------------------------------- */
TEST(litmus_outcome_join_stability) {
    asx_outcome a, b, result;
    int i;

    for (i = 0; i < 100; i++) {
        a.severity = ASX_OUTCOME_ERR;
        b.severity = ASX_OUTCOME_PANICKED;
        result = asx_outcome_join(&a, &b);
        ASSERT_EQ((int)result.severity, (int)ASX_OUTCOME_PANICKED);

        result = asx_outcome_join(&b, &a);
        ASSERT_EQ((int)result.severity, (int)ASX_OUTCOME_PANICKED);
    }
}

/* -----------------------------------------------------------------------
 * LITMUS-15: Cancel severity lookup is pure
 * Assumption: asx_cancel_severity() is a pure function with no side
 * effects — same input always gives same output
 * ----------------------------------------------------------------------- */
TEST(litmus_cancel_severity_purity) {
    int k, i;
    for (k = 0; k <= 10; k++) {
        int first = asx_cancel_severity((asx_cancel_kind)k);
        for (i = 0; i < 50; i++) {
            int again = asx_cancel_severity((asx_cancel_kind)k);
            ASSERT_EQ(first, again);
        }
    }
}

/* --- Main --- */

int main(void) {
    fprintf(stderr, "[formal] memory-model litmus suite (bd-3vt.4)\n"
                    "[formal] verifying C99 assumptions across compiler/target\n");

    RUN_TEST(litmus_type_sizes);
    RUN_TEST(litmus_unsigned_wrap);
    RUN_TEST(litmus_handle_pack_unpack);
    RUN_TEST(litmus_enum_representation);
    RUN_TEST(litmus_signed_unsigned_cast);
    RUN_TEST(litmus_memset_zero_init);
    RUN_TEST(litmus_function_pointer_identity);
    RUN_TEST(litmus_enum_as_array_index);
    RUN_TEST(litmus_size_field_pattern);
    RUN_TEST(litmus_transition_determinism);
    RUN_TEST(litmus_null_pointer);
    RUN_TEST(litmus_bitwise_uint64);
    RUN_TEST(litmus_char_bit_is_8);
    RUN_TEST(litmus_outcome_join_stability);
    RUN_TEST(litmus_cancel_severity_purity);

    TEST_REPORT();
    return test_failures;
}
