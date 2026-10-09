/*
 * test_canon.c — footprints, independence, Foata layering and digests
 * (src/conformance/canon.c, bead W1.5).
 *
 * The vectors are the normative ones tools/twin_run/src/canon.rs is tested
 * with: tests/conformance/vocab_v2_independence_table.json and the worked
 * example in schemas/canonical_vocabulary_v2.json. Run from the repository
 * root (as `make test-unit` does).
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include "conformance/canon.h"

#include <stdio.h>
#include <string.h>

static asx_json_doc g_doc;
static asx_json_doc g_out_doc;
static char g_file[1u << 20];
static char g_a[65536];
static char g_b[65536];

static int load_json(const char *path, uint32_t *root) {
    FILE *f = fopen(path, "rb");
    size_t n;
    if (f == NULL) return 0;
    n = fread(g_file, 1, sizeof(g_file) - 1u, f);
    fclose(f);
    if (n == 0u || n >= sizeof(g_file) - 1u) return 0;
    asx_json_doc_init(&g_doc);
    return asx_json_parse(&g_doc, g_file, n, root) == ASX_OK;
}

static int canonical_of(const asx_json_doc *doc, uint32_t node, char *buf, size_t cap) {
    asx_json_out out;
    asx_json_out_init(&out, buf, cap);
    if (asx_json_write_canonical(doc, node, &out) != ASX_OK) return 0;
    return asx_json_out_finish(&out) == ASX_OK;
}

/* Substitute the table's "$reason" placeholder, as canon.rs's test does. */
static uint32_t resolve_reason(uint32_t ev, uint32_t reason_fixture) {
    const char *r = asx_json_get_string(&g_doc, ev, "reason");
    uint32_t copy;
    if (r == NULL || strcmp(r, "$reason") != 0) return ev;
    copy = asx_json_copy(&g_doc, &g_doc, ev);
    asx_json_set(&g_doc, copy, "reason", asx_json_copy(&g_doc, &g_doc, reason_fixture));
    return copy;
}

TEST(independence_table_vectors_both_orders) {
    uint32_t root;
    uint32_t cases;
    uint32_t reason;
    uint32_t i;
    ASSERT_TRUE(load_json("tests/conformance/vocab_v2_independence_table.json", &root));
    cases = asx_json_get(&g_doc, root, "cases");
    reason = asx_json_get(&g_doc, root, "reason_fixture");
    ASSERT_EQ(asx_json_count(&g_doc, cases), 22u);
    for (i = 0; i < asx_json_count(&g_doc, cases); i++) {
        uint32_t c = asx_json_item(&g_doc, cases, i);
        uint32_t a = resolve_reason(asx_json_get(&g_doc, c, "a"), reason);
        uint32_t b = resolve_reason(asx_json_get(&g_doc, c, "b"), reason);
        int expected = -1;
        int ab = -1;
        int ba = -1;
        ASSERT_TRUE(asx_json_bool(&g_doc, asx_json_get(&g_doc, c, "independent"), &expected));
        ASSERT_EQ(asx_canon_independent(&g_doc, a, b, &ab), ASX_OK);
        ASSERT_EQ(asx_canon_independent(&g_doc, b, a, &ba), ASX_OK);
        if (ab != expected || ba != expected) {
            fprintf(stderr, "    case %s: expected %d, got %d / %d\n",
                    asx_json_get_string(&g_doc, c, "id"), expected, ab, ba);
        }
        ASSERT_EQ(ab, expected);
        ASSERT_EQ(ba, expected);
    }
}

TEST(foata_layers_match_the_spec_example) {
    /* One emission order of the spike scenario; the schema holds its
     * canonical layers. */
    static const char stream[] =
        "[{\"k\":\"region.created\",\"region\":\"root\",\"parent\":null},"
        "{\"k\":\"task.spawned\",\"task\":\"t.producer\",\"region\":\"root\"},"
        "{\"k\":\"task.spawned\",\"task\":\"t.consumer\",\"region\":\"root\"},"
        "{\"k\":\"task.spawned\",\"task\":\"t.waiter\",\"region\":\"root\"},"
        "{\"k\":\"user.trace\",\"message\":\"oneshot::reserve creating permit\"},"
        "{\"k\":\"obligation.reserved\",\"obligation\":\"t.consumer/o1\",\"task\":\"t.consumer\","
        "\"region\":\"root\",\"kind\":\"SendPermit\"},"
        "{\"k\":\"obligation.committed\",\"obligation\":\"t.consumer/o1\",\"task\":\"t.consumer\","
        "\"region\":\"root\",\"kind\":\"SendPermit\"},"
        "{\"k\":\"task.completed\",\"task\":\"t.producer\",\"region\":\"root\"},"
        "{\"k\":\"task.completed\",\"task\":\"t.consumer\",\"region\":\"root\"},"
        "{\"k\":\"user.trace\",\"message\":\"oneshot::recv received value\"},"
        "{\"k\":\"task.completed\",\"task\":\"t.waiter\",\"region\":\"root\"}]";
    uint32_t root;
    uint32_t expected;
    uint32_t events;
    uint32_t layers;
    ASSERT_TRUE(load_json("schemas/canonical_vocabulary_v2.json", &root));
    expected =
        asx_json_get(&g_doc, asx_json_item(&g_doc, asx_json_get(&g_doc, root, "examples"), 0),
                     "trace");
    ASSERT_TRUE(canonical_of(&g_doc, expected, g_a, sizeof(g_a)));

    asx_json_doc_init(&g_out_doc);
    ASSERT_EQ(asx_json_parse(&g_out_doc, stream, sizeof(stream) - 1u, &events), ASX_OK);
    ASSERT_EQ(asx_canon_trace(&g_out_doc, events, &g_out_doc, &layers), ASX_OK);
    ASSERT_TRUE(canonical_of(&g_out_doc, layers, g_b, sizeof(g_b)));
    ASSERT_STR_EQ(g_b, g_a);
}

TEST(foata_is_invariant_under_commuting_reorders) {
    /* Swapping independent events leaves the canonical form unchanged;
     * swapping dependent ones changes it. */
    static const char s1[] = "[{\"k\":\"task.spawned\",\"task\":\"t.a\",\"region\":\"r\"},"
                             "{\"k\":\"task.spawned\",\"task\":\"t.b\",\"region\":\"r\"},"
                             "{\"k\":\"task.completed\",\"task\":\"t.a\",\"region\":\"r\"}]";
    static const char s2[] = "[{\"k\":\"task.spawned\",\"task\":\"t.b\",\"region\":\"r\"},"
                             "{\"k\":\"task.spawned\",\"task\":\"t.a\",\"region\":\"r\"},"
                             "{\"k\":\"task.completed\",\"task\":\"t.a\",\"region\":\"r\"}]";
    static const char s3[] = "[{\"k\":\"task.completed\",\"task\":\"t.a\",\"region\":\"r\"},"
                             "{\"k\":\"task.spawned\",\"task\":\"t.b\",\"region\":\"r\"},"
                             "{\"k\":\"task.spawned\",\"task\":\"t.a\",\"region\":\"r\"}]";
    uint32_t ev;
    uint32_t layers;
    char c3[4096];
    asx_json_doc_init(&g_out_doc);
    ASSERT_EQ(asx_json_parse(&g_out_doc, s1, sizeof(s1) - 1u, &ev), ASX_OK);
    ASSERT_EQ(asx_canon_trace(&g_out_doc, ev, &g_out_doc, &layers), ASX_OK);
    ASSERT_TRUE(canonical_of(&g_out_doc, layers, g_a, sizeof(g_a)));
    asx_json_doc_init(&g_out_doc);
    ASSERT_EQ(asx_json_parse(&g_out_doc, s2, sizeof(s2) - 1u, &ev), ASX_OK);
    ASSERT_EQ(asx_canon_trace(&g_out_doc, ev, &g_out_doc, &layers), ASX_OK);
    ASSERT_TRUE(canonical_of(&g_out_doc, layers, g_b, sizeof(g_b)));
    ASSERT_STR_EQ(g_a, g_b);
    asx_json_doc_init(&g_out_doc);
    ASSERT_EQ(asx_json_parse(&g_out_doc, s3, sizeof(s3) - 1u, &ev), ASX_OK);
    ASSERT_EQ(asx_canon_trace(&g_out_doc, ev, &g_out_doc, &layers), ASX_OK);
    ASSERT_TRUE(canonical_of(&g_out_doc, layers, c3, sizeof(c3)));
    ASSERT_TRUE(strcmp(g_a, c3) != 0);
}

TEST(unknown_kind_and_missing_fields_fail_closed) {
    static const char bad_kind[] = "[{\"k\":\"task.polled\",\"task\":\"t\"}]";
    static const char no_region[] = "[{\"k\":\"task.spawned\",\"task\":\"t\"}]";
    uint32_t ev;
    uint32_t layers;
    int ind = -1;
    asx_json_doc_init(&g_out_doc);
    ASSERT_EQ(asx_json_parse(&g_out_doc, bad_kind, sizeof(bad_kind) - 1u, &ev), ASX_OK);
    ASSERT_EQ(asx_canon_trace(&g_out_doc, ev, &g_out_doc, &layers), ASX_E_INVALID_ARGUMENT);
    ASSERT_TRUE(asx_canon_error() != NULL);
    ASSERT_EQ(asx_canon_independent(&g_out_doc, asx_json_item(&g_out_doc, ev, 0),
                                    asx_json_item(&g_out_doc, ev, 0), &ind),
              ASX_E_INVALID_ARGUMENT);
    asx_json_doc_init(&g_out_doc);
    ASSERT_EQ(asx_json_parse(&g_out_doc, no_region, sizeof(no_region) - 1u, &ev), ASX_OK);
    ASSERT_EQ(asx_canon_trace(&g_out_doc, ev, &g_out_doc, &layers), ASX_E_INVALID_ARGUMENT);
}

TEST(empty_trace_is_empty_array) {
    uint32_t ev;
    uint32_t layers;
    asx_json_doc_init(&g_out_doc);
    ASSERT_EQ(asx_json_parse(&g_out_doc, "[]", 2, &ev), ASX_OK);
    ASSERT_EQ(asx_canon_trace(&g_out_doc, ev, &g_out_doc, &layers), ASX_OK);
    ASSERT_TRUE(canonical_of(&g_out_doc, layers, g_a, sizeof(g_a)));
    ASSERT_STR_EQ(g_a, "[]");
}

TEST(digest_is_sha256_of_canonical_bytes) {
    char d[ASX_CANON_DIGEST_LEN];
    uint32_t root;
    uint32_t i;
    uint32_t big;
    asx_json_doc_init(&g_out_doc);
    ASSERT_EQ(asx_json_parse(&g_out_doc, "{}", 2, &root), ASX_OK);
    ASSERT_EQ(asx_canon_digest(&g_out_doc, root, d), ASX_OK);
    /* sha256("{}"), the vector canon.rs asserts */
    ASSERT_STR_EQ(d, "sha256:44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a");
    /* Key order does not change the digest. */
    ASSERT_EQ(asx_json_parse(&g_out_doc, "{\"b\":1,\"a\":2}", 13, &root), ASX_OK);
    ASSERT_EQ(asx_canon_digest(&g_out_doc, root, d), ASX_OK);
    ASSERT_EQ(asx_json_parse(&g_out_doc, "{\"a\":2,\"b\":1}", 13, &root), ASX_OK);
    ASSERT_EQ(asx_canon_digest(&g_out_doc, root, g_a), ASX_OK);
    ASSERT_STR_EQ(d, g_a);
    /* A value larger than the streaming chunk digests without truncation:
     * two values differing only in their last element differ. */
    big = asx_json_new_array(&g_out_doc);
    for (i = 0; i < 3000u; i++) asx_json_push(&g_out_doc, big, asx_json_new_u64(&g_out_doc, i));
    ASSERT_EQ(asx_canon_digest(&g_out_doc, big, d), ASX_OK);
    asx_json_push(&g_out_doc, big, asx_json_new_u64(&g_out_doc, 1u));
    ASSERT_EQ(asx_canon_digest(&g_out_doc, big, g_a), ASX_OK);
    ASSERT_TRUE(strcmp(d, g_a) != 0);
}

int main(void) {
    RUN_TEST(independence_table_vectors_both_orders);
    RUN_TEST(foata_layers_match_the_spec_example);
    RUN_TEST(foata_is_invariant_under_commuting_reorders);
    RUN_TEST(unknown_kind_and_missing_fields_fail_closed);
    RUN_TEST(empty_trace_is_empty_array);
    RUN_TEST(digest_is_sha256_of_canonical_bytes);
    TEST_REPORT();
    return test_failures;
}
