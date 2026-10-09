/*
 * asx-conformance — runs DSL v2 scenarios through the C runtime and compares
 * them with Rust captures (beads W1.5/W1.6, bd-9kll.2.5/bd-9kll.2.6).
 *
 *   asx-conformance run <scenario.json>
 *       Print the C run's projection (canonical JSON) to stdout.
 *   asx-conformance compare <fixture.json>...
 *       Execute each asx.fixture.v2 file's embedded scenario through the C
 *       runtime and compare trace, snapshot, observations and the lab's
 *       dispatch order with the Rust capture, canonical byte for byte.
 *       Prints PASS/FAIL/ERROR per fixture and the first differences; exits
 *       1 unless every fixture passes.
 *   asx-conformance self-test <fixture.json>
 *       Negative control: compare against the fixture with one expected
 *       trace event corrupted in memory. Exits 0 only if that is reported
 *       as a FAIL, so a comparator that passes everything cannot go
 *       unnoticed.
 *
 * SPDX-License-Identifier: MIT
 */

#include "conformance/canon.h"
#include "conformance/interpreter.h"
#include "conformance/json.h"

#include <stdio.h>
#include <string.h>

#define FILE_CAP (4u * 1024u * 1024u)
#define BYTES_CAP (1024u * 1024u)

static char g_file[FILE_CAP];
static asx_json_doc g_fixture;
static asx_json_doc g_run;
static char g_a[BYTES_CAP];
static char g_b[BYTES_CAP];
static char g_error[1024];

static int load(const char *path, asx_json_doc *doc, uint32_t *root) {
    FILE *f = fopen(path, "rb");
    size_t n;
    asx_status st;
    if (f == NULL) {
        fprintf(stderr, "asx-conformance: cannot open %s\n", path);
        return 0;
    }
    n = fread(g_file, 1, FILE_CAP, f);
    fclose(f);
    if (n == FILE_CAP) {
        fprintf(stderr, "asx-conformance: %s is larger than %u bytes\n", path, FILE_CAP);
        return 0;
    }
    asx_json_doc_init(doc);
    st = asx_json_parse(doc, g_file, n, root);
    if (st != ASX_OK) {
        fprintf(stderr, "asx-conformance: %s: invalid JSON at byte %lu: %s\n", path,
                (unsigned long)doc->error_offset, doc->error != NULL ? doc->error : "?");
        return 0;
    }
    return 1;
}

static int canonical(const asx_json_doc *doc, uint32_t node, char *buf) {
    asx_json_out out;
    asx_json_out_init(&out, buf, BYTES_CAP);
    if (asx_json_write_canonical(doc, node, &out) != ASX_OK) return 0;
    return asx_json_out_finish(&out) == ASX_OK;
}

/* Print at most `max` bytes of a canonical value. */
static void show(const char *label, const char *bytes) {
    size_t len = strlen(bytes);
    const size_t max = 600u;
    fprintf(stdout, "      %s %.*s%s\n", label, (int)(len > max ? max : len), bytes,
            len > max ? " ..." : "");
}

/* Compare the members of two objects; report the differing ones. */
static int diff_members(const char *part, uint32_t want, uint32_t got) {
    int same = 1;
    uint32_t m;
    for (m = asx_json_item(&g_fixture, want, 0); m != ASX_JSON_NONE; m = g_fixture.nodes[m].next) {
        const char *key = asx_json_key(&g_fixture, m);
        uint32_t g = asx_json_get(&g_run, got, key);
        if (!canonical(&g_fixture, m, g_a)) return 0;
        if (g == ASX_JSON_NONE) {
            fprintf(stdout, "    %s.%s: missing in C\n", part, key);
            show("rust:", g_a);
            same = 0;
            continue;
        }
        if (!canonical(&g_run, g, g_b)) return 0;
        if (strcmp(g_a, g_b) != 0) {
            fprintf(stdout, "    %s.%s differs\n", part, key);
            show("rust:", g_a);
            show("c:   ", g_b);
            same = 0;
        }
    }
    for (m = asx_json_item(&g_run, got, 0); m != ASX_JSON_NONE; m = g_run.nodes[m].next) {
        const char *key = asx_json_key(&g_run, m);
        if (asx_json_get(&g_fixture, want, key) == ASX_JSON_NONE) {
            if (!canonical(&g_run, m, g_b)) return 0;
            fprintf(stdout, "    %s.%s: only in C\n", part, key);
            show("c:   ", g_b);
            same = 0;
        }
    }
    return same;
}

/* Compare two arrays element by element; report the first difference. */
static int diff_items(const char *part, uint32_t want, uint32_t got) {
    uint32_t nw = asx_json_count(&g_fixture, want);
    uint32_t ng = asx_json_count(&g_run, got);
    uint32_t i;
    for (i = 0; i < nw || i < ng; i++) {
        uint32_t w = asx_json_item(&g_fixture, want, i);
        uint32_t g = asx_json_item(&g_run, got, i);
        if (w != ASX_JSON_NONE && !canonical(&g_fixture, w, g_a)) return 0;
        if (g != ASX_JSON_NONE && !canonical(&g_run, g, g_b)) return 0;
        if (w == ASX_JSON_NONE || g == ASX_JSON_NONE || strcmp(g_a, g_b) != 0) {
            fprintf(stdout, "    %s[%u] differs (rust has %u, c has %u)\n", part, (unsigned)i,
                    (unsigned)nw, (unsigned)ng);
            if (w != ASX_JSON_NONE) show("rust:", g_a);
            if (g != ASX_JSON_NONE) show("c:   ", g_b);
            return 0;
        }
    }
    return 1;
}

static int same_part(uint32_t want, uint32_t got) {
    if (!canonical(&g_fixture, want, g_a) || !canonical(&g_run, got, g_b)) return 0;
    return strcmp(g_a, g_b) == 0;
}

static const char *dispatch_at(const asx_json_doc *doc, uint32_t list, uint32_t i) {
    uint32_t item = asx_json_item(doc, list, i);
    return item != ASX_JSON_NONE ? asx_json_string(doc, item) : NULL;
}

/* Report the first dispatch that differs, with three on either side. */
static void diff_dispatches(uint32_t want, uint32_t got) {
    uint32_t nw = asx_json_count(&g_fixture, want);
    uint32_t ng = asx_json_count(&g_run, got);
    uint32_t i;
    uint32_t k;
    for (i = 0; i < nw && i < ng; i++) {
        const char *w = dispatch_at(&g_fixture, want, i);
        const char *g = dispatch_at(&g_run, got, i);
        if (w == NULL || g == NULL || strcmp(w, g) != 0) break;
    }
    fprintf(stdout, "    dispatch %u differs (rust has %u, c has %u)\n", (unsigned)i, (unsigned)nw,
            (unsigned)ng);
    fprintf(stdout, "        %-30s %s\n", "rust", "c");
    for (k = i >= 3u ? i - 3u : 0u; k < i + 4u && (k < nw || k < ng); k++) {
        const char *w = dispatch_at(&g_fixture, want, k);
        const char *g = dispatch_at(&g_run, got, k);
        fprintf(stdout, "      %c %-30s %s\n", k == i ? '>' : ' ', w != NULL ? w : "-",
                g != NULL ? g : "-");
    }
}

typedef enum { CMP_PASS, CMP_FAIL, CMP_ERROR } cmp_result;

/* Run one fixture's scenario through C and compare. `mutate` corrupts the
 * expected trace first (the self-test). */
static cmp_result compare_one(const char *path, int mutate) {
    uint32_t root;
    uint32_t scenario;
    uint32_t run = ASX_JSON_NONE;
    uint32_t want_trace;
    uint32_t want_snapshot;
    uint32_t want_obs;
    uint32_t schedule;
    uint32_t want_dispatches = ASX_JSON_NONE;
    const char *id;
    const char *schema;
    asx_status st;
    int ok = 1;

    if (!load(path, &g_fixture, &root)) return CMP_ERROR;
    schema = asx_json_get_string(&g_fixture, root, "schema");
    id = asx_json_get_string(&g_fixture, root, "scenario_id");
    scenario = asx_json_get(&g_fixture, root, "scenario");
    want_trace = asx_json_get(&g_fixture, root, "trace");
    want_snapshot = asx_json_get(&g_fixture, root, "snapshot");
    want_obs = asx_json_get(&g_fixture, root, "observations");
    schedule = asx_json_get(&g_fixture, root, "schedule");
    if (schedule != ASX_JSON_NONE) {
        want_dispatches = asx_json_get(&g_fixture, schedule, "dispatches");
    }
    if (schema == NULL || strcmp(schema, "asx.fixture.v2") != 0 || id == NULL ||
        scenario == ASX_JSON_NONE || want_trace == ASX_JSON_NONE ||
        want_snapshot == ASX_JSON_NONE || want_obs == ASX_JSON_NONE ||
        want_dispatches == ASX_JSON_NONE) {
        fprintf(stdout,
                "ERROR %s: not an asx.fixture.v2 file with scenario, trace, snapshot, "
                "observations and schedule.dispatches\n",
                path);
        return CMP_ERROR;
    }
    if (mutate) {
        /* Rename the kind of the first event of the first layer. */
        uint32_t first = asx_json_item(&g_fixture, asx_json_item(&g_fixture, want_trace, 0), 0);
        if (first == ASX_JSON_NONE) {
            fprintf(stdout, "ERROR %s: self-test needs a non-empty trace\n", id);
            return CMP_ERROR;
        }
        asx_json_set(&g_fixture, first, "k", asx_json_new_string(&g_fixture, "self.test.mutation"));
    }

    asx_json_doc_init(&g_run);
    st = asx_conformance_run(&g_fixture, scenario, &g_run, &run, g_error, sizeof(g_error));
    if (st != ASX_OK) {
        fprintf(stdout, "ERROR %s: C run failed: %s\n", id, g_error[0] != '\0' ? g_error : "?");
        return CMP_ERROR;
    }

    if (!same_part(want_trace, asx_json_get(&g_run, run, "trace"))) {
        ok = 0;
        fprintf(stdout, "FAIL %s: trace\n", id);
        (void)diff_items("trace", want_trace, asx_json_get(&g_run, run, "trace"));
    }
    if (!same_part(want_snapshot, asx_json_get(&g_run, run, "snapshot"))) {
        uint32_t got = asx_json_get(&g_run, run, "snapshot");
        static const char *const parts[] = {"tasks", "regions", "obligations"};
        size_t i;
        if (ok) fprintf(stdout, "FAIL %s: snapshot\n", id);
        ok = 0;
        for (i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
            char label[32];
            (void)snprintf(label, sizeof(label), "snapshot.%s", parts[i]);
            (void)diff_members(label, asx_json_get(&g_fixture, want_snapshot, parts[i]),
                               asx_json_get(&g_run, got, parts[i]));
        }
        {
            static const char *const scalars[] = {"now_ns", "quiescent", "timers_pending",
                                                  "channels"};
            for (i = 0; i < sizeof(scalars) / sizeof(scalars[0]); i++) {
                uint32_t w = asx_json_get(&g_fixture, want_snapshot, scalars[i]);
                uint32_t g = asx_json_get(&g_run, got, scalars[i]);
                if (w == ASX_JSON_NONE || g == ASX_JSON_NONE || !same_part(w, g)) {
                    fprintf(stdout, "    snapshot.%s differs\n", scalars[i]);
                    if (w != ASX_JSON_NONE && canonical(&g_fixture, w, g_a)) show("rust:", g_a);
                    if (g != ASX_JSON_NONE && canonical(&g_run, g, g_b)) show("c:   ", g_b);
                }
            }
        }
    }
    if (!same_part(want_obs, asx_json_get(&g_run, run, "observations"))) {
        if (ok) fprintf(stdout, "FAIL %s: observations\n", id);
        ok = 0;
        (void)diff_items("observations", want_obs, asx_json_get(&g_run, run, "observations"));
    }
    /* The lab's dispatch order (bd-9kll.4.8): the canonical trace forgets
     * the order of independent events, so two runs can agree on it and
     * still have polled tasks differently. */
    if (!same_part(want_dispatches, asx_json_get(&g_run, run, "dispatches"))) {
        if (ok) fprintf(stdout, "FAIL %s: dispatches\n", id);
        ok = 0;
        diff_dispatches(want_dispatches, asx_json_get(&g_run, run, "dispatches"));
    }
    if (ok) {
        fprintf(stdout, "PASS %s %s\n", id, asx_json_get_string(&g_run, run, "semantic_digest"));
        return CMP_PASS;
    }
    return CMP_FAIL;
}

static int cmd_run(const char *path) {
    uint32_t root;
    uint32_t run = ASX_JSON_NONE;
    asx_status st;
    if (!load(path, &g_fixture, &root)) return 2;
    asx_json_doc_init(&g_run);
    st = asx_conformance_run(&g_fixture, root, &g_run, &run, g_error, sizeof(g_error));
    if (st != ASX_OK) {
        fprintf(stderr, "asx-conformance: C run failed: %s\n", g_error[0] != '\0' ? g_error : "?");
        return 1;
    }
    if (!canonical(&g_run, run, g_a)) {
        fprintf(stderr, "asx-conformance: projection does not fit the output buffer\n");
        return 1;
    }
    fprintf(stdout, "%s\n", g_a);
    return 0;
}

static int usage(void) {
    fprintf(stderr, "usage: asx-conformance run <scenario.json>\n"
                    "       asx-conformance compare <fixture.json>...\n"
                    "       asx-conformance self-test <fixture.json>\n");
    return 2;
}

int main(int argc, char **argv) {
    int i;
    unsigned pass = 0;
    unsigned fail = 0;
    unsigned error = 0;
    if (argc < 3) return usage();
    if (strcmp(argv[1], "run") == 0 && argc == 3) return cmd_run(argv[2]);
    if (strcmp(argv[1], "self-test") == 0 && argc == 3) {
        cmp_result r = compare_one(argv[2], 1);
        if (r == CMP_FAIL) {
            fprintf(stdout, "self-test: the corrupted expectation was reported (good)\n");
            return 0;
        }
        fprintf(stdout, "self-test: FAILED, the corrupted expectation was %s\n",
                r == CMP_PASS ? "accepted" : "not compared");
        return 1;
    }
    if (strcmp(argv[1], "compare") != 0) return usage();
    for (i = 2; i < argc; i++) {
        switch (compare_one(argv[i], 0)) {
        case CMP_PASS: pass++; break;
        case CMP_FAIL: fail++; break;
        case CMP_ERROR: error++; break;
        }
    }
    fprintf(stdout, "asx-conformance: compared=%u pass=%u fail=%u error=%u\n", (unsigned)(argc - 2),
            pass, fail, error);
    return (fail == 0u && error == 0u) ? 0 : 1;
}
