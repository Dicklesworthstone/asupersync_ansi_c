/*
 * test_json.c — bounded JSON DOM and RFC 8785 canonical writer used by the
 * Rust-parity oracle (src/conformance/json.c, bead W1.5).
 *
 * The canonical-form vectors match tools/twin_run/src/canon.rs, the Rust
 * side's writer: the two engines' runs are compared by these bytes.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include "conformance/json.h"

#include <string.h>

static asx_json_doc g_doc;
static asx_json_doc g_doc2;
static char g_out[65536];

/* Parse `text` into g_doc and write its canonical form into g_out. */
static asx_status roundtrip(const char *text) {
    uint32_t root;
    asx_json_out out;
    asx_status st;
    asx_json_doc_init(&g_doc);
    st = asx_json_parse(&g_doc, text, strlen(text), &root);
    if (st != ASX_OK) return st;
    asx_json_out_init(&out, g_out, sizeof(g_out));
    st = asx_json_write_canonical(&g_doc, root, &out);
    if (st != ASX_OK) return st;
    return asx_json_out_finish(&out);
}

static asx_status parse_only(const char *text) {
    uint32_t root;
    asx_json_doc_init(&g_doc);
    return asx_json_parse(&g_doc, text, strlen(text), &root);
}

TEST(canonical_sorts_keys_and_escapes_like_twin_run) {
    ASSERT_EQ(roundtrip("{\"b\": 1, \"a\": [true, null, \"x\\\"y\\n\\u0001\"],"
                        " \"aa\": {\"z\": 0, \"y\": -2}}"),
              ASX_OK);
    ASSERT_STR_EQ(g_out,
                  "{\"a\":[true,null,\"x\\\"y\\n\\u0001\"],\"aa\":{\"y\":-2,\"z\":0},\"b\":1}");
}

TEST(canonical_escapes_short_forms_and_keeps_utf8_raw) {
    ASSERT_EQ(roundtrip("[\"\\b\\f\\r\\t\\/\\u001f\\u00e9\\u2028\"]"), ASX_OK);
    /* "/" needs no escape; U+00E9 and U+2028 stay raw UTF-8. */
    ASSERT_STR_EQ(g_out, "[\"\\b\\f\\r\\t/\\u001f\xc3\xa9\xe2\x80\xa8\"]");
}

TEST(keys_order_by_utf16_code_units_not_code_points) {
    /* U+1F600 is the surrogate pair D83D DE00, which sorts before U+E000 in
     * UTF-16 although its code point is larger. */
    ASSERT_EQ(roundtrip("{\"\\ue000\": 1, \"\\ud83d\\ude00\": 2}"), ASX_OK);
    ASSERT_STR_EQ(g_out, "{\"\xf0\x9f\x98\x80\":2,\"\xee\x80\x80\":1}");
    ASSERT_TRUE(asx_json_utf16_cmp("\xf0\x9f\x98\x80", 4, "\xee\x80\x80", 3) < 0);
    ASSERT_TRUE(asx_json_utf16_cmp("a", 1, "ab", 2) < 0);
    ASSERT_EQ(asx_json_utf16_cmp("ab", 2, "ab", 2), 0);
}

TEST(integer_boundaries) {
    ASSERT_EQ(roundtrip("[18446744073709551615, -9223372036854775808, -0, 0]"), ASX_OK);
    ASSERT_STR_EQ(g_out, "[18446744073709551615,-9223372036854775808,0,0]");
    ASSERT_EQ(parse_only("18446744073709551616"), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(parse_only("-9223372036854775809"), ASX_E_INVALID_ARGUMENT);
}

TEST(rejects_what_canonical_form_cannot_represent) {
    ASSERT_EQ(parse_only("1.5"), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(parse_only("1e3"), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(parse_only("{\"a\":1,\"a\":2}"), ASX_E_INVALID_ARGUMENT);
}

TEST(rejects_malformed_input) {
    ASSERT_EQ(parse_only(""), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(parse_only("[1,]"), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(parse_only("{\"a\" 1}"), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(parse_only("[1] x"), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(parse_only("01"), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(parse_only("\"tab\there\""), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(parse_only("\"\\x\""), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(parse_only("\"\\ud83d\""), ASX_E_INVALID_ARGUMENT);          /* lone high surrogate */
    ASSERT_EQ(parse_only("\"\\ude00\""), ASX_E_INVALID_ARGUMENT);          /* lone low surrogate */
    ASSERT_EQ(parse_only("\"\xc0\x80\""), ASX_E_INVALID_ARGUMENT);         /* overlong NUL */
    ASSERT_EQ(parse_only("\"\xed\xa0\x80\""), ASX_E_INVALID_ARGUMENT);     /* encoded surrogate */
    ASSERT_EQ(parse_only("\"\xf4\x90\x80\x80\""), ASX_E_INVALID_ARGUMENT); /* > U+10FFFF */
    ASSERT_TRUE(g_doc.error != NULL);
}

TEST(depth_limit_fails_closed) {
    char deep[2 * (ASX_JSON_MAX_DEPTH + 1u) + 1u];
    size_t i;
    size_t n = ASX_JSON_MAX_DEPTH;
    for (i = 0; i < n; i++) deep[i] = '[';
    for (i = 0; i < n; i++) deep[n + i] = ']';
    deep[2u * n] = '\0';
    ASSERT_EQ(parse_only(deep), ASX_OK);
    n = ASX_JSON_MAX_DEPTH + 1u;
    for (i = 0; i < n; i++) deep[i] = '[';
    for (i = 0; i < n; i++) deep[n + i] = ']';
    deep[2u * n] = '\0';
    ASSERT_EQ(parse_only(deep), ASX_E_RESOURCE_EXHAUSTED);
}

TEST(output_overflow_is_reported_not_truncated) {
    uint32_t root;
    asx_json_out out;
    char small[8];
    asx_json_doc_init(&g_doc);
    ASSERT_EQ(asx_json_parse(&g_doc, "{\"key\":\"value\"}", 15, &root), ASX_OK);
    asx_json_out_init(&out, small, sizeof(small));
    ASSERT_EQ(asx_json_write_canonical(&g_doc, root, &out), ASX_E_BUFFER_TOO_SMALL);
    ASSERT_EQ(asx_json_out_finish(&out), ASX_E_BUFFER_TOO_SMALL);
}

TEST(copy_preserves_keys_with_embedded_nul) {
    /* Regression: the copy path once re-measured keys with strlen and turned
     * {"a\u0000b":1} into {"a":1}. */
    uint32_t root;
    uint32_t copy;
    asx_json_out out;
    asx_json_doc_init(&g_doc);
    asx_json_doc_init(&g_doc2);
    ASSERT_EQ(asx_json_parse(&g_doc, "{\"a\\u0000b\":1,\"a\":2}", 20, &root), ASX_OK);
    copy = asx_json_copy(&g_doc2, &g_doc, root);
    ASSERT_NE(copy, ASX_JSON_NONE);
    ASSERT_EQ(asx_json_count(&g_doc2, copy), 2u);
    asx_json_out_init(&out, g_out, sizeof(g_out));
    ASSERT_EQ(asx_json_write_canonical(&g_doc2, copy, &out), ASX_OK);
    ASSERT_EQ(asx_json_out_finish(&out), ASX_OK);
    ASSERT_TRUE(out.len == 20u && memcmp(g_out, "{\"a\":2,\"a\\u0000b\":1}", 20) == 0);
}

TEST(builders_and_accessors) {
    uint32_t obj;
    uint32_t arr;
    uint64_t v = 0;
    int b = 0;
    asx_json_out out;
    asx_json_doc_init(&g_doc);
    obj = asx_json_new_object(&g_doc);
    arr = asx_json_new_array(&g_doc);
    asx_json_push(&g_doc, arr, asx_json_new_u64(&g_doc, 7u));
    asx_json_push(&g_doc, arr, asx_json_new_bool(&g_doc, 1));
    asx_json_set(&g_doc, obj, "z", arr);
    asx_json_set(&g_doc, obj, "m", asx_json_new_string(&g_doc, "x"));
    asx_json_set(&g_doc, obj, "m", asx_json_new_string(&g_doc, "y")); /* replaces */
    asx_json_set(&g_doc, obj, "n", asx_json_new_null(&g_doc));
    ASSERT_TRUE(asx_json_doc_ok(&g_doc));
    ASSERT_EQ(asx_json_count(&g_doc, obj), 3u);
    ASSERT_STR_EQ(asx_json_get_string(&g_doc, obj, "m"), "y");
    ASSERT_TRUE(asx_json_is_null(&g_doc, asx_json_get(&g_doc, obj, "n")));
    ASSERT_TRUE(asx_json_u64(&g_doc, asx_json_item(&g_doc, arr, 0), &v) && v == 7u);
    ASSERT_TRUE(asx_json_bool(&g_doc, asx_json_item(&g_doc, arr, 1), &b) && b == 1);
    ASSERT_EQ(asx_json_get(&g_doc, obj, "absent"), ASX_JSON_NONE);
    ASSERT_EQ(asx_json_item(&g_doc, arr, 2), ASX_JSON_NONE);
    asx_json_out_init(&out, g_out, sizeof(g_out));
    ASSERT_EQ(asx_json_write_canonical(&g_doc, obj, &out), ASX_OK);
    ASSERT_EQ(asx_json_out_finish(&out), ASX_OK);
    ASSERT_STR_EQ(g_out, "{\"m\":\"y\",\"n\":null,\"z\":[7,true]}");
}

TEST(node_pool_exhaustion_is_reported) {
    uint32_t i;
    uint32_t arr;
    asx_json_doc_init(&g_doc);
    arr = asx_json_new_array(&g_doc);
    for (i = 0; i < ASX_JSON_MAX_NODES; i++) {
        uint32_t n = asx_json_new_null(&g_doc);
        if (n == ASX_JSON_NONE) break;
        asx_json_push(&g_doc, arr, n);
    }
    ASSERT_TRUE(i < ASX_JSON_MAX_NODES);
    ASSERT_FALSE(asx_json_doc_ok(&g_doc));
}

int main(void) {
    RUN_TEST(canonical_sorts_keys_and_escapes_like_twin_run);
    RUN_TEST(canonical_escapes_short_forms_and_keeps_utf8_raw);
    RUN_TEST(keys_order_by_utf16_code_units_not_code_points);
    RUN_TEST(integer_boundaries);
    RUN_TEST(rejects_what_canonical_form_cannot_represent);
    RUN_TEST(rejects_malformed_input);
    RUN_TEST(depth_limit_fails_closed);
    RUN_TEST(output_overflow_is_reported_not_truncated);
    RUN_TEST(copy_preserves_keys_with_embedded_nul);
    RUN_TEST(builders_and_accessors);
    RUN_TEST(node_pool_exhaustion_is_reported);
    TEST_REPORT();
    return test_failures;
}
