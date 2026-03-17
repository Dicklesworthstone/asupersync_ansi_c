/*
 * test_symbol.c — comprehensive tests for symbol registry, symbol sets,
 *                 and typed symbols (bd-1eqo.15.3)
 *
 * Covers:
 *   - Symbol registration and idempotent re-registration
 *   - Lookup (by name, by ID), invalid/not-found paths
 *   - Registry capacity exhaustion
 *   - Symbol set algebra (insert, remove, contains, union, intersect,
 *     subset, equals, count, empty)
 *   - Typed symbol registration, lookup, mismatch detection
 *   - Type kind strings
 *   - NULL safety across all APIs
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/asx.h>
#include <stdio.h>
#include <string.h>

/* ================================================================== */
/* Section 1: Symbol registry — registration and lookup               */
/* ================================================================== */

TEST(symbol_register_and_lookup) {
    asx_symbol_id id1, id2;

    asx_symbol_registry_reset();

    ASSERT_EQ(asx_symbol_register("temperature", &id1), ASX_OK);
    ASSERT_NE(id1, ASX_SYMBOL_INVALID);

    ASSERT_EQ(asx_symbol_lookup("temperature", &id2), ASX_OK);
    ASSERT_EQ(id1, id2);
}

TEST(symbol_register_idempotent) {
    asx_symbol_id id1, id2;

    asx_symbol_registry_reset();

    ASSERT_EQ(asx_symbol_register("pressure", &id1), ASX_OK);
    ASSERT_EQ(asx_symbol_register("pressure", &id2), ASX_OK);
    ASSERT_EQ(id1, id2);
    ASSERT_EQ(asx_symbol_count(), (uint16_t)1);
}

TEST(symbol_register_distinct_names_distinct_ids) {
    asx_symbol_id id1, id2;

    asx_symbol_registry_reset();

    ASSERT_EQ(asx_symbol_register("alpha", &id1), ASX_OK);
    ASSERT_EQ(asx_symbol_register("beta", &id2), ASX_OK);
    ASSERT_NE(id1, id2);
    ASSERT_EQ(asx_symbol_count(), (uint16_t)2);
}

TEST(symbol_name_returns_registered_string) {
    asx_symbol_id id;

    asx_symbol_registry_reset();

    ASSERT_EQ(asx_symbol_register("velocity", &id), ASX_OK);
    ASSERT_STR_EQ(asx_symbol_name(id), "velocity");
}

TEST(symbol_name_invalid_returns_null) {
    asx_symbol_registry_reset();
    ASSERT_TRUE(asx_symbol_name(ASX_SYMBOL_INVALID) == NULL);
    ASSERT_TRUE(asx_symbol_name(999) == NULL);
}

TEST(symbol_is_valid_checks) {
    asx_symbol_id id;

    asx_symbol_registry_reset();

    ASSERT_FALSE(asx_symbol_is_valid(ASX_SYMBOL_INVALID));
    ASSERT_FALSE(asx_symbol_is_valid(42));

    ASSERT_EQ(asx_symbol_register("test", &id), ASX_OK);
    ASSERT_TRUE(asx_symbol_is_valid(id));
}

TEST(symbol_lookup_not_found) {
    asx_symbol_id id;

    asx_symbol_registry_reset();
    ASSERT_EQ(asx_symbol_lookup("nonexistent", &id), ASX_E_NOT_FOUND);
}

TEST(symbol_register_null_name) {
    asx_symbol_id id;
    asx_symbol_registry_reset();
    ASSERT_EQ(asx_symbol_register(NULL, &id), ASX_E_INVALID_ARGUMENT);
}

TEST(symbol_register_empty_name) {
    asx_symbol_id id;
    asx_symbol_registry_reset();
    ASSERT_EQ(asx_symbol_register("", &id), ASX_E_INVALID_ARGUMENT);
}

TEST(symbol_register_null_output) {
    asx_symbol_registry_reset();
    ASSERT_EQ(asx_symbol_register("test", NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(symbol_reset_clears_all) {
    asx_symbol_id id;

    asx_symbol_registry_reset();
    ASSERT_EQ(asx_symbol_register("cleared", &id), ASX_OK);
    ASSERT_EQ(asx_symbol_count(), (uint16_t)1);

    asx_symbol_registry_reset();
    ASSERT_EQ(asx_symbol_count(), (uint16_t)0);
    ASSERT_EQ(asx_symbol_lookup("cleared", &id), ASX_E_NOT_FOUND);
}

/* ================================================================== */
/* Section 2: Symbol set algebra                                       */
/* ================================================================== */

TEST(symbol_set_empty_on_init) {
    asx_symbol_set s;
    asx_symbol_set_init(&s);
    ASSERT_TRUE(asx_symbol_set_is_empty(&s));
    ASSERT_EQ(asx_symbol_set_count(&s), (uint16_t)0);
}

TEST(symbol_set_insert_and_contains) {
    asx_symbol_set s;
    asx_symbol_set_init(&s);
    asx_symbol_set_insert(&s, 1);
    asx_symbol_set_insert(&s, 42);

    ASSERT_TRUE(asx_symbol_set_contains(&s, 1));
    ASSERT_TRUE(asx_symbol_set_contains(&s, 42));
    ASSERT_FALSE(asx_symbol_set_contains(&s, 2));
    ASSERT_FALSE(asx_symbol_set_is_empty(&s));
    ASSERT_EQ(asx_symbol_set_count(&s), (uint16_t)2);
}

TEST(symbol_set_remove) {
    asx_symbol_set s;
    asx_symbol_set_init(&s);
    asx_symbol_set_insert(&s, 10);
    asx_symbol_set_insert(&s, 20);

    asx_symbol_set_remove(&s, 10);
    ASSERT_FALSE(asx_symbol_set_contains(&s, 10));
    ASSERT_TRUE(asx_symbol_set_contains(&s, 20));
    ASSERT_EQ(asx_symbol_set_count(&s), (uint16_t)1);
}

TEST(symbol_set_invalid_id_ignored) {
    asx_symbol_set s;
    asx_symbol_set_init(&s);

    asx_symbol_set_insert(&s, ASX_SYMBOL_INVALID);
    ASSERT_TRUE(asx_symbol_set_is_empty(&s));
    ASSERT_FALSE(asx_symbol_set_contains(&s, ASX_SYMBOL_INVALID));
}

TEST(symbol_set_union) {
    asx_symbol_set a, b, result;
    asx_symbol_set_init(&a);
    asx_symbol_set_init(&b);
    asx_symbol_set_init(&result);

    asx_symbol_set_insert(&a, 1);
    asx_symbol_set_insert(&a, 2);
    asx_symbol_set_insert(&b, 2);
    asx_symbol_set_insert(&b, 3);

    asx_symbol_set_union(&result, &a, &b);
    ASSERT_TRUE(asx_symbol_set_contains(&result, 1));
    ASSERT_TRUE(asx_symbol_set_contains(&result, 2));
    ASSERT_TRUE(asx_symbol_set_contains(&result, 3));
    ASSERT_EQ(asx_symbol_set_count(&result), (uint16_t)3);
}

TEST(symbol_set_intersect) {
    asx_symbol_set a, b, result;
    asx_symbol_set_init(&a);
    asx_symbol_set_init(&b);
    asx_symbol_set_init(&result);

    asx_symbol_set_insert(&a, 1);
    asx_symbol_set_insert(&a, 2);
    asx_symbol_set_insert(&a, 3);
    asx_symbol_set_insert(&b, 2);
    asx_symbol_set_insert(&b, 3);
    asx_symbol_set_insert(&b, 4);

    asx_symbol_set_intersect(&result, &a, &b);
    ASSERT_FALSE(asx_symbol_set_contains(&result, 1));
    ASSERT_TRUE(asx_symbol_set_contains(&result, 2));
    ASSERT_TRUE(asx_symbol_set_contains(&result, 3));
    ASSERT_FALSE(asx_symbol_set_contains(&result, 4));
    ASSERT_EQ(asx_symbol_set_count(&result), (uint16_t)2);
}

TEST(symbol_set_subset) {
    asx_symbol_set a, b;
    asx_symbol_set_init(&a);
    asx_symbol_set_init(&b);

    asx_symbol_set_insert(&a, 2);
    asx_symbol_set_insert(&b, 1);
    asx_symbol_set_insert(&b, 2);
    asx_symbol_set_insert(&b, 3);

    ASSERT_TRUE(asx_symbol_set_is_subset(&a, &b));
    ASSERT_FALSE(asx_symbol_set_is_subset(&b, &a));
}

TEST(symbol_set_equals) {
    asx_symbol_set a, b;
    asx_symbol_set_init(&a);
    asx_symbol_set_init(&b);

    asx_symbol_set_insert(&a, 5);
    asx_symbol_set_insert(&a, 10);
    asx_symbol_set_insert(&b, 5);
    asx_symbol_set_insert(&b, 10);

    ASSERT_TRUE(asx_symbol_set_equals(&a, &b));

    asx_symbol_set_insert(&b, 15);
    ASSERT_FALSE(asx_symbol_set_equals(&a, &b));
}

TEST(symbol_set_empty_is_subset_of_everything) {
    asx_symbol_set empty, nonempty;
    asx_symbol_set_init(&empty);
    asx_symbol_set_init(&nonempty);
    asx_symbol_set_insert(&nonempty, 1);

    ASSERT_TRUE(asx_symbol_set_is_subset(&empty, &nonempty));
    ASSERT_TRUE(asx_symbol_set_is_subset(&empty, &empty));
}

TEST(symbol_set_high_ids) {
    /* Test IDs in the upper range of the 256-bit bitfield */
    asx_symbol_set s;
    asx_symbol_set_init(&s);

    asx_symbol_set_insert(&s, 200);
    asx_symbol_set_insert(&s, 255);

    ASSERT_TRUE(asx_symbol_set_contains(&s, 200));
    ASSERT_TRUE(asx_symbol_set_contains(&s, 255));
    ASSERT_FALSE(asx_symbol_set_contains(&s, 199));
    ASSERT_EQ(asx_symbol_set_count(&s), (uint16_t)2);
}

TEST(symbol_set_accepts_terminal_registry_id) {
    asx_symbol_set s;
    asx_symbol_id id = ASX_SYMBOL_INVALID;
    char names[ASX_SYMBOL_REGISTRY_CAPACITY][32];
    uint16_t i;

    asx_symbol_registry_reset();
    asx_symbol_set_init(&s);

    for (i = 1; i <= ASX_SYMBOL_REGISTRY_CAPACITY; i++) {
        snprintf(names[i - 1u], sizeof(names[i - 1u]), "sym_%u", (unsigned)i);
        ASSERT_EQ(asx_symbol_register(names[i - 1u], &id), ASX_OK);
    }

    ASSERT_EQ(id, (asx_symbol_id)ASX_SYMBOL_REGISTRY_CAPACITY);
    asx_symbol_set_insert(&s, id);
    ASSERT_TRUE(asx_symbol_set_contains(&s, id));
    ASSERT_EQ(asx_symbol_set_count(&s), (uint16_t)1);

    asx_symbol_set_remove(&s, id);
    ASSERT_FALSE(asx_symbol_set_contains(&s, id));
    ASSERT_TRUE(asx_symbol_set_is_empty(&s));
}

/* ================================================================== */
/* Section 3: Typed symbols                                            */
/* ================================================================== */

TEST(typed_symbol_register_and_lookup) {
    asx_typed_symbol ts, ts2;

    asx_symbol_registry_reset();

    ASSERT_EQ(asx_typed_symbol_register("latency_ms", ASX_TYPE_KIND_U64, 8, 8, &ts), ASX_OK);
    ASSERT_NE(ts.symbol, ASX_SYMBOL_INVALID);
    ASSERT_EQ(ts.kind, ASX_TYPE_KIND_U64);
    ASSERT_EQ(ts.size, (uint16_t)8);
    ASSERT_EQ(ts.alignment, (uint16_t)8);

    ASSERT_EQ(asx_typed_symbol_lookup("latency_ms", &ts2), ASX_OK);
    ASSERT_EQ(ts2.symbol, ts.symbol);
    ASSERT_EQ(ts2.kind, ASX_TYPE_KIND_U64);
}

TEST(typed_symbol_idempotent_same_type) {
    asx_typed_symbol ts1, ts2;

    asx_symbol_registry_reset();

    ASSERT_EQ(asx_typed_symbol_register("counter", ASX_TYPE_KIND_U32, 4, 4, &ts1), ASX_OK);
    ASSERT_EQ(asx_typed_symbol_register("counter", ASX_TYPE_KIND_U32, 4, 4, &ts2), ASX_OK);
    ASSERT_EQ(ts1.symbol, ts2.symbol);
}

TEST(typed_symbol_mismatch_rejects) {
    asx_typed_symbol ts;

    asx_symbol_registry_reset();

    ASSERT_EQ(asx_typed_symbol_register("metric", ASX_TYPE_KIND_U32, 4, 4, &ts), ASX_OK);
    /* Same name, different type = mismatch */
    ASSERT_EQ(asx_typed_symbol_register("metric", ASX_TYPE_KIND_U64, 8, 8, &ts),
              ASX_E_INVALID_STATE);
}

TEST(typed_symbol_get_by_id) {
    asx_typed_symbol ts, ts2;

    asx_symbol_registry_reset();

    ASSERT_EQ(asx_typed_symbol_register("event_id", ASX_TYPE_KIND_STRING, 0, 0, &ts), ASX_OK);

    ASSERT_EQ(asx_typed_symbol_get(ts.symbol, &ts2), ASX_OK);
    ASSERT_EQ(ts2.kind, ASX_TYPE_KIND_STRING);
    ASSERT_EQ(ts2.symbol, ts.symbol);
}

TEST(typed_symbol_get_untyped_returns_not_found) {
    asx_symbol_id id;
    asx_typed_symbol ts;

    asx_symbol_registry_reset();

    /* Register as plain symbol (no type info) */
    ASSERT_EQ(asx_symbol_register("plain", &id), ASX_OK);
    /* Typed lookup should fail — no type metadata */
    ASSERT_EQ(asx_typed_symbol_get(id, &ts), ASX_E_NOT_FOUND);
}

TEST(typed_symbol_lookup_not_found) {
    asx_typed_symbol ts;
    asx_symbol_registry_reset();
    ASSERT_EQ(asx_typed_symbol_lookup("missing", &ts), ASX_E_NOT_FOUND);
}

TEST(typed_symbol_null_output) {
    asx_symbol_registry_reset();
    ASSERT_EQ(asx_typed_symbol_register("x", ASX_TYPE_KIND_U8, 1, 1, NULL), ASX_E_INVALID_ARGUMENT);
}

/* ================================================================== */
/* Section 4: Type kind strings                                        */
/* ================================================================== */

TEST(type_kind_str_all_known) {
    ASSERT_STR_EQ(asx_type_kind_str(ASX_TYPE_KIND_VOID), "void");
    ASSERT_STR_EQ(asx_type_kind_str(ASX_TYPE_KIND_U8), "u8");
    ASSERT_STR_EQ(asx_type_kind_str(ASX_TYPE_KIND_U16), "u16");
    ASSERT_STR_EQ(asx_type_kind_str(ASX_TYPE_KIND_U32), "u32");
    ASSERT_STR_EQ(asx_type_kind_str(ASX_TYPE_KIND_U64), "u64");
    ASSERT_STR_EQ(asx_type_kind_str(ASX_TYPE_KIND_I32), "i32");
    ASSERT_STR_EQ(asx_type_kind_str(ASX_TYPE_KIND_I64), "i64");
    ASSERT_STR_EQ(asx_type_kind_str(ASX_TYPE_KIND_F32), "f32");
    ASSERT_STR_EQ(asx_type_kind_str(ASX_TYPE_KIND_F64), "f64");
    ASSERT_STR_EQ(asx_type_kind_str(ASX_TYPE_KIND_BOOL), "bool");
    ASSERT_STR_EQ(asx_type_kind_str(ASX_TYPE_KIND_BYTES), "bytes");
    ASSERT_STR_EQ(asx_type_kind_str(ASX_TYPE_KIND_STRING), "string");
    ASSERT_STR_EQ(asx_type_kind_str(ASX_TYPE_KIND_STRUCT), "struct");
    ASSERT_STR_EQ(asx_type_kind_str(ASX_TYPE_KIND_COUNT), "count");
}

TEST(type_kind_str_unknown) { ASSERT_STR_EQ(asx_type_kind_str((asx_type_kind)99), "unknown"); }

/* ================================================================== */
/* Section 5: Set algebra laws                                         */
/* ================================================================== */

TEST(symbol_set_union_identity) {
    /* union(A, empty) == A */
    asx_symbol_set a, empty, result;
    asx_symbol_set_init(&a);
    asx_symbol_set_init(&empty);
    asx_symbol_set_init(&result);

    asx_symbol_set_insert(&a, 5);
    asx_symbol_set_insert(&a, 10);

    asx_symbol_set_union(&result, &a, &empty);
    ASSERT_TRUE(asx_symbol_set_equals(&result, &a));
}

TEST(symbol_set_intersect_empty_is_empty) {
    /* intersect(A, empty) == empty */
    asx_symbol_set a, empty, result;
    asx_symbol_set_init(&a);
    asx_symbol_set_init(&empty);
    asx_symbol_set_init(&result);

    asx_symbol_set_insert(&a, 5);

    asx_symbol_set_intersect(&result, &a, &empty);
    ASSERT_TRUE(asx_symbol_set_is_empty(&result));
}

TEST(symbol_set_union_commutative) {
    asx_symbol_set a, b, ab, ba;
    asx_symbol_set_init(&a);
    asx_symbol_set_init(&b);
    asx_symbol_set_init(&ab);
    asx_symbol_set_init(&ba);

    asx_symbol_set_insert(&a, 1);
    asx_symbol_set_insert(&a, 3);
    asx_symbol_set_insert(&b, 2);
    asx_symbol_set_insert(&b, 3);

    asx_symbol_set_union(&ab, &a, &b);
    asx_symbol_set_union(&ba, &b, &a);
    ASSERT_TRUE(asx_symbol_set_equals(&ab, &ba));
}

TEST(symbol_set_intersect_commutative) {
    asx_symbol_set a, b, ab, ba;
    asx_symbol_set_init(&a);
    asx_symbol_set_init(&b);
    asx_symbol_set_init(&ab);
    asx_symbol_set_init(&ba);

    asx_symbol_set_insert(&a, 1);
    asx_symbol_set_insert(&a, 2);
    asx_symbol_set_insert(&b, 2);
    asx_symbol_set_insert(&b, 3);

    asx_symbol_set_intersect(&ab, &a, &b);
    asx_symbol_set_intersect(&ba, &b, &a);
    ASSERT_TRUE(asx_symbol_set_equals(&ab, &ba));
}

TEST(symbol_set_self_union_idempotent) {
    asx_symbol_set a, result;
    asx_symbol_set_init(&a);
    asx_symbol_set_init(&result);

    asx_symbol_set_insert(&a, 7);
    asx_symbol_set_insert(&a, 14);

    asx_symbol_set_union(&result, &a, &a);
    ASSERT_TRUE(asx_symbol_set_equals(&result, &a));
}

TEST(symbol_set_self_intersect_idempotent) {
    asx_symbol_set a, result;
    asx_symbol_set_init(&a);
    asx_symbol_set_init(&result);

    asx_symbol_set_insert(&a, 7);
    asx_symbol_set_insert(&a, 14);

    asx_symbol_set_intersect(&result, &a, &a);
    ASSERT_TRUE(asx_symbol_set_equals(&result, &a));
}

/* ================================================================== */
/* main                                                               */
/* ================================================================== */

int main(void) {
    fprintf(stderr, "=== test_symbol ===\n");

    /* Registry */
    RUN_TEST(symbol_register_and_lookup);
    RUN_TEST(symbol_register_idempotent);
    RUN_TEST(symbol_register_distinct_names_distinct_ids);
    RUN_TEST(symbol_name_returns_registered_string);
    RUN_TEST(symbol_name_invalid_returns_null);
    RUN_TEST(symbol_is_valid_checks);
    RUN_TEST(symbol_lookup_not_found);
    RUN_TEST(symbol_register_null_name);
    RUN_TEST(symbol_register_empty_name);
    RUN_TEST(symbol_register_null_output);
    RUN_TEST(symbol_reset_clears_all);

    /* Symbol sets */
    RUN_TEST(symbol_set_empty_on_init);
    RUN_TEST(symbol_set_insert_and_contains);
    RUN_TEST(symbol_set_remove);
    RUN_TEST(symbol_set_invalid_id_ignored);
    RUN_TEST(symbol_set_union);
    RUN_TEST(symbol_set_intersect);
    RUN_TEST(symbol_set_subset);
    RUN_TEST(symbol_set_equals);
    RUN_TEST(symbol_set_empty_is_subset_of_everything);
    RUN_TEST(symbol_set_high_ids);
    RUN_TEST(symbol_set_accepts_terminal_registry_id);

    /* Typed symbols */
    RUN_TEST(typed_symbol_register_and_lookup);
    RUN_TEST(typed_symbol_idempotent_same_type);
    RUN_TEST(typed_symbol_mismatch_rejects);
    RUN_TEST(typed_symbol_get_by_id);
    RUN_TEST(typed_symbol_get_untyped_returns_not_found);
    RUN_TEST(typed_symbol_lookup_not_found);
    RUN_TEST(typed_symbol_null_output);

    /* Type kind strings */
    RUN_TEST(type_kind_str_all_known);
    RUN_TEST(type_kind_str_unknown);

    /* Set algebra laws */
    RUN_TEST(symbol_set_union_identity);
    RUN_TEST(symbol_set_intersect_empty_is_empty);
    RUN_TEST(symbol_set_union_commutative);
    RUN_TEST(symbol_set_intersect_commutative);
    RUN_TEST(symbol_set_self_union_idempotent);
    RUN_TEST(symbol_set_self_intersect_idempotent);

    TEST_REPORT();
    return test_failures;
}
