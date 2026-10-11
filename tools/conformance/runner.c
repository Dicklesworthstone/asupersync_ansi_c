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
 *   asx-conformance canon <cases.jsonl>
 *       Canonicalizer differential (bd-9kll.9.1): each line is
 *       {"events":[...],"canonical":[[...]],"digest":"sha256:..."} from
 *       `twin_run canon-fuzz` (Rust's canon.rs); C's canon.c must give the
 *       same Foata layers, byte for byte, and the same digest. Exits 1 on
 *       any difference.
 *   asx-conformance constants <rust_kernel_constants.json>
 *       Kernel constants and defaults (bd-9kll.2.13): every constant of the
 *       Rust document (`twin_run constants`) must have a C counterpart here
 *       with the same value, or be declared without one (N/A, with the
 *       reason), or be a recorded difference (KNOWN, with its bead or
 *       reason). Exits 1 on an unrecorded difference, on a recorded one
 *       that no longer differs, on a constant either side lacks, or when
 *       nothing was compared.
 *
 * SPDX-License-Identifier: MIT
 */

#include "conformance/canon.h"
#include "conformance/interpreter.h"
#include "conformance/json.h"
#include "runtime/runtime_internal.h"

#include <asx/actor/actor.h>
#include <asx/actor/supervisor.h>
#include <asx/asx_config.h>
#include <asx/core/budget.h>
#include <asx/core/cancel.h>
#include <asx/runtime/lab.h>
#include <asx/runtime/runtime.h>
#include <asx/time/timer_wheel.h>

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

/* Canonicalize each case's events with C's canon.c and compare with the
 * Rust layers and digest on the same line. */
static int cmd_canon(const char *path) {
    FILE *f = fopen(path, "rb");
    unsigned long line = 0;
    unsigned long pass = 0;
    unsigned long fail = 0;
    if (f == NULL) {
        fprintf(stderr, "asx-conformance: cannot open %s\n", path);
        return 1;
    }
    while (fgets(g_file, (int)FILE_CAP, f) != NULL) {
        size_t n = strlen(g_file);
        uint32_t root;
        uint32_t layers;
        const char *want_digest;
        char digest[ASX_CANON_DIGEST_LEN];
        line++;
        if (n == 0u || g_file[n - 1u] != '\n') {
            fprintf(stderr, "asx-conformance: %s:%lu: line too long or unterminated\n", path, line);
            fclose(f);
            return 1;
        }
        asx_json_doc_init(&g_fixture);
        if (asx_json_parse(&g_fixture, g_file, n, &root) != ASX_OK) {
            fprintf(stderr, "asx-conformance: %s:%lu: invalid JSON\n", path, line);
            fclose(f);
            return 1;
        }
        want_digest = asx_json_get_string(&g_fixture, root, "digest");
        if (want_digest == NULL ||
            !canonical(&g_fixture, asx_json_get(&g_fixture, root, "canonical"), g_b) ||
            asx_canon_trace(&g_fixture, asx_json_get(&g_fixture, root, "events"), &g_fixture,
                            &layers) != ASX_OK ||
            !canonical(&g_fixture, layers, g_a) ||
            asx_canon_digest(&g_fixture, layers, digest) != ASX_OK) {
            const char *why = asx_canon_error();
            fprintf(stdout, "ERROR %s:%lu: %s\n", path, line, why != NULL ? why : "malformed case");
            fail++;
            continue;
        }
        if (strcmp(g_a, g_b) == 0 && strcmp(digest, want_digest) == 0) {
            pass++;
            continue;
        }
        fail++;
        if (fail <= 5u) {
            fprintf(stdout, "FAIL %s:%lu: canonical form differs\n", path, line);
            show("rust:", g_b);
            show("c:   ", g_a);
        }
    }
    fclose(f);
    fprintf(stdout, "asx-conformance canon: traces=%lu pass=%lu fail=%lu\n", pass + fail, pass,
            fail);
    return fail == 0u && pass > 0u ? 0 : 1;
}

/* ---- constants (bd-9kll.2.13) ----------------------------------------- */

typedef enum { CK_U64, CK_NULL, CK_STR, CK_BOOL, CK_NA } ck_kind;

typedef struct {
    char key[64];
    ck_kind kind;
    uint64_t u;        /* CK_U64; CK_BOOL as 0/1 */
    char s[160];       /* CK_STR: the value; CK_NA: why C has no counterpart */
    const char *known; /* a recorded difference: its bead or reason */
    int as_set;        /* CK_STR from a list: compare the names sorted */
    int seen;
} ck_entry;

#define CK_NAMES_MAX 32u

/* `n` names joined by commas into `out`, sorted first when `sort`.
 * Returns 0 when they do not fit. */
static int ck_join(const char **names, size_t n, int sort, char *out, size_t cap) {
    size_t i;
    size_t used = 0;
    if (sort) {
        for (i = 1; i < n; i++) {
            const char *x = names[i];
            size_t j = i;
            while (j > 0u && strcmp(names[j - 1u], x) > 0) {
                names[j] = names[j - 1u];
                j--;
            }
            names[j] = x;
        }
    }
    out[0] = '\0';
    for (i = 0; i < n; i++) {
        int w = snprintf(out + used, cap - used, "%s%s", i > 0u ? "," : "", names[i]);
        if (w < 0 || (size_t)w >= cap - used) return 0;
        used += (size_t)w;
    }
    return 1;
}

#define CK_CAP 128u
static ck_entry g_ck[CK_CAP];
static uint32_t g_ck_n;
static int g_ck_overflow;

static ck_entry *ck_add(const char *key, ck_kind kind) {
    ck_entry *e;
    if (g_ck_n >= CK_CAP || strlen(key) >= sizeof(e->key)) {
        g_ck_overflow = 1;
        return NULL;
    }
    e = &g_ck[g_ck_n++];
    memset(e, 0, sizeof(*e));
    (void)snprintf(e->key, sizeof(e->key), "%s", key);
    e->kind = kind;
    return e;
}

static void ck_u64(const char *key, uint64_t v) {
    ck_entry *e = ck_add(key, CK_U64);
    if (e != NULL) e->u = v;
}

static void ck_str(const char *key, const char *v) {
    ck_entry *e = ck_add(key, CK_STR);
    if (e != NULL) (void)snprintf(e->s, sizeof(e->s), "%s", v);
}

static void ck_bool(const char *key, int v) {
    ck_entry *e = ck_add(key, CK_BOOL);
    if (e != NULL) e->u = v != 0 ? 1u : 0u;
}

static void ck_null(const char *key) { (void)ck_add(key, CK_NULL); }

static void ck_na(const char *key, const char *why) {
    ck_entry *e = ck_add(key, CK_NA);
    if (e != NULL) (void)snprintf(e->s, sizeof(e->s), "%s", why);
}

/* Record that `key` is known to differ from Rust: `why` names the bead
 * that tracks it, or the reason it is accepted. */
static void ck_known(const char *key, const char *why) {
    uint32_t i;
    for (i = 0; i < g_ck_n; i++) {
        if (strcmp(g_ck[i].key, key) == 0) {
            g_ck[i].known = why;
            return;
        }
    }
    g_ck_overflow = 1;
}

/* C's "unconstrained" encodings (deadline 0, cost UINT64_MAX) are Rust's
 * None. */
static void ck_budget(const char *prefix, asx_budget b) {
    char key[64];
    (void)snprintf(key, sizeof(key), "%s.deadline_ns", prefix);
    if (b.deadline == 0u) {
        ck_null(key);
    } else {
        ck_u64(key, (uint64_t)b.deadline);
    }
    (void)snprintf(key, sizeof(key), "%s.poll_quota", prefix);
    ck_u64(key, (uint64_t)b.poll_quota);
    (void)snprintf(key, sizeof(key), "%s.cost_quota", prefix);
    if (b.cost_quota == UINT64_MAX) {
        ck_null(key);
    } else {
        ck_u64(key, b.cost_quota);
    }
    (void)snprintf(key, sizeof(key), "%s.priority", prefix);
    ck_u64(key, (uint64_t)b.priority);
}

static const struct {
    const char *rust;
    asx_cancel_kind c;
} k_cancel_kinds[] = {
    {"User", ASX_CANCEL_USER},
    {"Timeout", ASX_CANCEL_TIMEOUT},
    {"Deadline", ASX_CANCEL_DEADLINE},
    {"PollQuota", ASX_CANCEL_POLL_QUOTA},
    {"CostBudget", ASX_CANCEL_COST_BUDGET},
    {"FailFast", ASX_CANCEL_FAIL_FAST},
    {"RaceLost", ASX_CANCEL_RACE_LOST},
    {"ParentCancelled", ASX_CANCEL_PARENT},
    {"ResourceUnavailable", ASX_CANCEL_RESOURCE},
    {"Shutdown", ASX_CANCEL_SHUTDOWN},
    {"LinkedExit", ASX_CANCEL_LINKED_EXIT},
};
#define K_CANCEL_KINDS (sizeof(k_cancel_kinds) / sizeof(k_cancel_kinds[0]))

typedef struct {
    const char *rust; /* Rust's variant name */
    int c;            /* the C enumerator */
} ck_variant;

#define CK_COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* The Rust names of a C enum's variants in C's declaration order, or
 * sorted when only the set matters. C-only enumerators are not in `v`. */
static void ck_variants(const char *key, const ck_variant *v, size_t n, int as_set) {
    const char *names[CK_NAMES_MAX];
    size_t count = 0;
    size_t i;
    int c;
    ck_entry *e;
    for (c = 0; c < 64 && count < n && count < CK_NAMES_MAX; c++) {
        for (i = 0; i < n; i++) {
            if (v[i].c == c && count < CK_NAMES_MAX) names[count++] = v[i].rust;
        }
    }
    e = ck_add(key, CK_STR);
    if (e == NULL) return;
    e->as_set = as_set;
    if (count != n || !ck_join(names, count, as_set, e->s, sizeof(e->s))) g_ck_overflow = 1;
}

static const ck_variant k_task_states[] = {
    {"Created", ASX_TASK_CREATED},
    {"Running", ASX_TASK_RUNNING},
    {"CancelRequested", ASX_TASK_CANCEL_REQUESTED},
    {"Cancelling", ASX_TASK_CANCELLING},
    {"Finalizing", ASX_TASK_FINALIZING},
    {"Completed", ASX_TASK_COMPLETED},
};
static const ck_variant k_region_states[] = {
    {"Open", ASX_REGION_OPEN},         {"Closing", ASX_REGION_CLOSING},
    {"Draining", ASX_REGION_DRAINING}, {"Finalizing", ASX_REGION_FINALIZING},
    {"Closed", ASX_REGION_CLOSED},
};
static const ck_variant k_obligation_states[] = {
    {"Reserved", ASX_OBLIGATION_RESERVED},
    {"Committed", ASX_OBLIGATION_COMMITTED},
    {"Aborted", ASX_OBLIGATION_ABORTED},
    {"Leaked", ASX_OBLIGATION_LEAKED},
};
/* C-only: ASX_OBLIGATION_KIND_GENERIC (no kind given). */
static const ck_variant k_obligation_kinds[] = {
    {"SendPermit", ASX_OBLIGATION_KIND_SEND_PERMIT},
    {"Ack", ASX_OBLIGATION_KIND_ACK},
    {"Lease", ASX_OBLIGATION_KIND_LEASE},
    {"IoOp", ASX_OBLIGATION_KIND_IO_OP},
    {"SemaphorePermit", ASX_OBLIGATION_KIND_SEMAPHORE_PERMIT},
    {"Transaction", ASX_OBLIGATION_KIND_TRANSACTION},
};
/* C-only: ASX_OBLIGATION_ABORT_NONE (not aborted). */
static const ck_variant k_abort_reasons[] = {
    {"Explicit", ASX_OBLIGATION_ABORT_EXPLICIT},
    {"Cancel", ASX_OBLIGATION_ABORT_CANCEL},
    {"Error", ASX_OBLIGATION_ABORT_ERROR},
};
static const ck_variant k_cancel_phases[] = {
    {"Requested", ASX_CANCEL_PHASE_REQUESTED},
    {"Cancelling", ASX_CANCEL_PHASE_CANCELLING},
    {"Finalizing", ASX_CANCEL_PHASE_FINALIZING},
    {"Completed", ASX_CANCEL_PHASE_COMPLETED},
};
static const ck_variant k_leak_responses[] = {
    {"Panic", ASX_LEAK_PANIC},
    {"Log", ASX_LEAK_LOG},
    {"Silent", ASX_LEAK_SILENT},
    {"Recover", ASX_LEAK_RECOVER},
};
static const ck_variant k_finalizer_escalations[] = {
    {"Soft", ASX_FINALIZER_SOFT},
    {"BoundedLog", ASX_FINALIZER_BOUNDED_LOG},
    {"BoundedPanic", ASX_FINALIZER_BOUNDED_PANIC},
};
static const ck_variant k_restart_policies[] = {
    {"OneForOne", ASX_SUPERVISOR_ONE_FOR_ONE},
    {"OneForAll", ASX_SUPERVISOR_ONE_FOR_ALL},
    {"RestForOne", ASX_SUPERVISOR_REST_FOR_ONE},
};
static const ck_variant k_escalation_policies[] = {
    {"Stop", ASX_ESCALATION_STOP},
    {"Escalate", ASX_ESCALATION_ESCALATE},
    {"ResetCounter", ASX_ESCALATION_RESET_COUNTER},
};
static const ck_variant k_backoffs[] = {
    {"None", ASX_RESTART_BACKOFF_NONE},
    {"Fixed", ASX_RESTART_BACKOFF_FIXED},
    {"Exponential", ASX_RESTART_BACKOFF_EXPONENTIAL},
};
static const ck_variant k_restart_modes[] = {
    {"Permanent", ASX_CHILD_PERMANENT},
    {"Transient", ASX_CHILD_TRANSIENT},
    {"Temporary", ASX_CHILD_TEMPORARY},
};

static const char *leak_name(asx_leak_response r) {
    switch (r) {
    case ASX_LEAK_PANIC: return "Panic";
    case ASX_LEAK_LOG: return "Log";
    case ASX_LEAK_SILENT: return "Silent";
    case ASX_LEAK_RECOVER: return "Recover";
    }
    return "?";
}

static const char *finalizer_name(asx_finalizer_escalation e) {
    switch (e) {
    case ASX_FINALIZER_SOFT: return "Soft";
    case ASX_FINALIZER_BOUNDED_LOG: return "BoundedLog";
    case ASX_FINALIZER_BOUNDED_PANIC: return "BoundedPanic";
    }
    return "?";
}

static const char *strategy_name(asx_supervisor_strategy s) {
    switch (s) {
    case ASX_SUPERVISOR_ONE_FOR_ONE: return "OneForOne";
    case ASX_SUPERVISOR_ONE_FOR_ALL: return "OneForAll";
    case ASX_SUPERVISOR_REST_FOR_ONE: return "RestForOne";
    }
    return "?";
}

static const char *escalation_name(asx_supervisor_escalation e) {
    switch (e) {
    case ASX_ESCALATION_STOP: return "Stop";
    case ASX_ESCALATION_ESCALATE: return "Escalate";
    case ASX_ESCALATION_RESET_COUNTER: return "ResetCounter";
    }
    return "?";
}

static const char *backoff_name(asx_restart_backoff_kind k) {
    switch (k) {
    case ASX_RESTART_BACKOFF_NONE: return "None";
    case ASX_RESTART_BACKOFF_FIXED: return "Fixed";
    case ASX_RESTART_BACKOFF_EXPONENTIAL: return "Exponential";
    }
    return "?";
}

/* The C side of every constant in schemas/rust_kernel_constants.json. */
static void ck_build(void) {
    asx_runtime_config rt;
    asx_lab_config lab;
    asx_supervisor_config sup;
    asx_cancel_reason reason;
    char key[64];
    char order[160];
    size_t used = 0;
    uint32_t i;
    int ordinal;

    g_ck_n = 0;
    g_ck_overflow = 0;

    /* CancelKind: C's declaration order, by Rust name, then each kind. */
    order[0] = '\0';
    for (ordinal = 0; ordinal < (int)K_CANCEL_KINDS; ordinal++) {
        for (i = 0; i < (uint32_t)K_CANCEL_KINDS; i++) {
            if ((int)k_cancel_kinds[i].c == ordinal) {
                int n = snprintf(order + used, sizeof(order) - used, "%s%s", used > 0u ? "," : "",
                                 k_cancel_kinds[i].rust);
                if (n > 0 && (size_t)n < sizeof(order) - used) used += (size_t)n;
            }
        }
    }
    ck_str("cancel_kind.variants", order);
    for (i = 0; i < (uint32_t)K_CANCEL_KINDS; i++) {
        asx_budget cleanup = asx_cancel_cleanup_budget(k_cancel_kinds[i].c);
        (void)snprintf(key, sizeof(key), "cancel_kind.%s.ordinal", k_cancel_kinds[i].rust);
        ck_u64(key, (uint64_t)k_cancel_kinds[i].c);
        (void)snprintf(key, sizeof(key), "cancel_kind.%s.severity", k_cancel_kinds[i].rust);
        ck_u64(key, (uint64_t)asx_cancel_severity(k_cancel_kinds[i].c));
        (void)snprintf(key, sizeof(key), "cancel_kind.%s.cleanup_poll_quota",
                       k_cancel_kinds[i].rust);
        ck_u64(key, (uint64_t)cleanup.poll_quota);
        (void)snprintf(key, sizeof(key), "cancel_kind.%s.cleanup_priority", k_cancel_kinds[i].rust);
        ck_u64(key, (uint64_t)cleanup.priority);
    }
    reason = asx_cancel_reason_default(ASX_CANCEL_USER, NULL);
    ck_u64("cancel_reason.default_timestamp_ns", (uint64_t)reason.timestamp);

    asx_runtime_config_init(&rt);
    ck_u64("cancel_attribution.max_depth", (uint64_t)rt.max_cancel_chain_depth);
    ck_u64("cancel_attribution.max_memory", (uint64_t)rt.max_cancel_chain_memory);
    ck_u64("task.max_mask_depth", (uint64_t)ASX_MAX_MASK_DEPTH);
    ck_str("runtime.default.obligation_leak_response", leak_name(rt.leak_response));
    ck_u64("finalizer.poll_budget", (uint64_t)rt.finalizer_poll_budget);
    ck_u64("finalizer.time_budget_ns", rt.finalizer_time_budget_ns);
    ck_str("finalizer.default_escalation", finalizer_name(rt.finalizer_escalation));

    ck_budget("budget.infinite", asx_budget_infinite());
    ck_budget("budget.zero", asx_budget_zero());
    ck_budget("budget.default", asx_budget_new());

    ck_u64("timer.max_duration_ns", (uint64_t)ASX_TIMER_MAX_DURATION_NS);
    ck_u64("timer.level0_resolution_ns", (uint64_t)LAB_WHEEL_TICK_NS);

    asx_supervisor_config_init(&sup, "constants", 0u, 0u);
    ck_str("supervision.default.restart_policy", strategy_name(sup.strategy));
    ck_str("supervision.default.escalation", escalation_name(sup.escalation));
    ck_na("supervision.default.max_restarts", "asx_supervisor_config_init takes it as an argument");
    ck_na("supervision.default.restart_window_ns",
          "asx_supervisor_config_init takes it as an argument");
    ck_str("supervision.default.backoff.kind", backoff_name(sup.backoff.kind));
    ck_u64("supervision.default.backoff.initial_ns", sup.backoff.initial_ns);
    ck_u64("supervision.default.backoff.max_ns", sup.backoff.max_ns);
    ck_u64("supervision.default.backoff.multiplier", (uint64_t)sup.backoff.multiplier);

    /* C's mailbox capacity is a spawn argument bounded by this maximum,
     * which must hold Rust's default. */
    ck_u64("gen_server.default_mailbox_capacity", (uint64_t)ASX_ACTOR_MAILBOX_CAPACITY);
    ck_u64("gen_server.yield_interval", (uint64_t)ACTOR_YIELD_INTERVAL);
    ck_na("actor.default_mailbox_capacity", "C ports GenServers only, not the plain Actor");
    ck_na("actor.yield_interval", "C ports GenServers only, not the plain Actor");

    asx_lab_config_init(&lab);
    ck_u64("lab.default.seed", lab.seed);
    ck_na("lab.default.max_steps",
          "asx_lab counts scenario steps (ASX_LAB_MAX_STEPS), not scheduler steps");
    ck_bool("lab.default.panic_on_obligation_leak", rt.leak_response == ASX_LEAK_PANIC);
    ck_u64("lab.cancel_streak_limit", (uint64_t)LAB_CANCEL_STREAK_LIMIT);
    ck_u64("lab.runtime_cancel_streak_limit", (uint64_t)LAB_CANCEL_STREAK_LIMIT);
    ck_u64("lab.handle_cancel_batch", (uint64_t)LAB_HANDLE_BATCH);
    ck_u64("lab.region_command_batch", (uint64_t)LAB_REGION_BATCH);
    ck_u64("rwlock.max_consecutive_writers", (uint64_t)RW_MAX_WRITER_STREAK);

    /* Enum variants: in declaration order where the order means something
     * (states, phases, and policies whose ordinals C keeps), as a set where
     * only the variants do (kinds and abort reasons, encoded by name). */
    ck_variants("task_state.variants", k_task_states, CK_COUNT(k_task_states), 0);
    ck_variants("region_state.variants", k_region_states, CK_COUNT(k_region_states), 0);
    ck_variants("obligation_state.variants", k_obligation_states, CK_COUNT(k_obligation_states), 0);
    ck_variants("obligation_kind.variants", k_obligation_kinds, CK_COUNT(k_obligation_kinds), 1);
    ck_variants("obligation_abort_reason.variants", k_abort_reasons, CK_COUNT(k_abort_reasons), 1);
    ck_variants("cancel_phase.variants", k_cancel_phases, CK_COUNT(k_cancel_phases), 0);
    ck_variants("leak_response.variants", k_leak_responses, CK_COUNT(k_leak_responses), 0);
    ck_variants("finalizer_escalation.variants", k_finalizer_escalations,
                CK_COUNT(k_finalizer_escalations), 0);
    ck_variants("supervision.restart_policy.variants", k_restart_policies,
                CK_COUNT(k_restart_policies), 0);
    ck_variants("supervision.escalation_policy.variants", k_escalation_policies,
                CK_COUNT(k_escalation_policies), 0);
    ck_variants("supervision.backoff.variants", k_backoffs, CK_COUNT(k_backoffs), 0);
    ck_variants("supervision.restart_mode.variants", k_restart_modes, CK_COUNT(k_restart_modes), 0);

    /* Recorded differences. */
    ck_known("cancel_kind.variants", "bd-9kll.2.18: C declares LinkedExit seventh");
    ck_known("cancel_kind.ParentCancelled.ordinal", "bd-9kll.2.18");
    ck_known("cancel_kind.ResourceUnavailable.ordinal", "bd-9kll.2.18");
    ck_known("cancel_kind.Shutdown.ordinal", "bd-9kll.2.18");
    ck_known("cancel_kind.LinkedExit.ordinal", "bd-9kll.2.18");
    ck_known("budget.zero.deadline_ns",
             "accepted: C encodes no deadline as 0, so its earliest deadline is 1 ns");
    ck_known("runtime.default.obligation_leak_response", "bd-9kll.2.19: C defaults to Log");
    ck_known("lab.default.panic_on_obligation_leak", "bd-9kll.2.19");
}

/* The JSON value as C text, for messages. */
static void ck_rust_text(uint32_t v, char *buf, size_t cap) {
    if (!canonical(&g_fixture, v, g_a)) {
        (void)snprintf(buf, cap, "?");
        return;
    }
    (void)snprintf(buf, cap, "%s", g_a);
}

static void ck_c_text(const ck_entry *e, char *buf, size_t cap) {
    switch (e->kind) {
    case CK_U64: (void)snprintf(buf, cap, "%llu", (unsigned long long)e->u); return;
    case CK_NULL: (void)snprintf(buf, cap, "null"); return;
    case CK_STR: (void)snprintf(buf, cap, "\"%s\"", e->s); return;
    case CK_BOOL: (void)snprintf(buf, cap, "%s", e->u != 0u ? "true" : "false"); return;
    case CK_NA: (void)snprintf(buf, cap, "n/a"); return;
    }
}

/* Whether the Rust value `v` equals the C entry. An array of strings is
 * compared as its comma-joined text. */
static int ck_equal(uint32_t v, const ck_entry *e) {
    asx_json_type t = asx_json_type_of(&g_fixture, v);
    uint64_t u = 0;
    int b = 0;
    switch (e->kind) {
    case CK_U64: return t == ASX_JSON_INT && asx_json_u64(&g_fixture, v, &u) && u == e->u;
    case CK_NULL: return t == ASX_JSON_NULL;
    case CK_BOOL:
        return t == ASX_JSON_BOOL && asx_json_bool(&g_fixture, v, &b) && (uint64_t)b == e->u;
    case CK_STR:
        if (t == ASX_JSON_STRING) return strcmp(asx_json_string(&g_fixture, v), e->s) == 0;
        if (t == ASX_JSON_ARRAY) {
            const char *names[CK_NAMES_MAX];
            char joined[160];
            size_t n = 0;
            uint32_t m;
            for (m = asx_json_item(&g_fixture, v, 0); m != ASX_JSON_NONE;
                 m = g_fixture.nodes[m].next) {
                if (n >= CK_NAMES_MAX) return 0;
                names[n] = asx_json_string(&g_fixture, m);
                if (names[n] == NULL) return 0;
                n++;
            }
            return ck_join(names, n, e->as_set, joined, sizeof(joined)) &&
                   strcmp(joined, e->s) == 0;
        }
        return 0;
    case CK_NA: return 0;
    }
    return 0;
}

static int cmd_constants(const char *path) {
    uint32_t root;
    uint32_t constants;
    uint32_t m;
    uint32_t i;
    const char *schema;
    unsigned ok = 0;
    unsigned known = 0;
    unsigned na = 0;
    unsigned fail = 0;
    char rust[256];
    char c[256];

    if (!load(path, &g_fixture, &root)) return 2;
    schema = asx_json_get_string(&g_fixture, root, "schema");
    constants = asx_json_get(&g_fixture, root, "constants");
    if (schema == NULL || strcmp(schema, "asx.rust_kernel_constants.v1") != 0 ||
        asx_json_type_of(&g_fixture, constants) != ASX_JSON_OBJECT) {
        fprintf(stderr, "asx-conformance: %s is not an asx.rust_kernel_constants.v1 document\n",
                path);
        return 1;
    }
    ck_build();
    if (g_ck_overflow) {
        fprintf(stderr, "asx-conformance: the C constants table overflowed or names an "
                        "unknown key\n");
        return 1;
    }
    for (m = asx_json_item(&g_fixture, constants, 0); m != ASX_JSON_NONE;
         m = g_fixture.nodes[m].next) {
        const char *key = asx_json_key(&g_fixture, m);
        uint32_t value = asx_json_get(&g_fixture, m, "value");
        const char *source = asx_json_get_string(&g_fixture, m, "source");
        ck_entry *e = NULL;
        for (i = 0; i < g_ck_n; i++) {
            if (strcmp(g_ck[i].key, key) == 0) e = &g_ck[i];
        }
        ck_rust_text(value, rust, sizeof(rust));
        if (e == NULL) {
            fprintf(stdout, "FAIL %s: rust=%s (%s) has no C counterpart declared\n", key, rust,
                    source != NULL ? source : "?");
            fail++;
            continue;
        }
        e->seen = 1;
        if (e->kind == CK_NA) {
            fprintf(stdout, "N/A  %s: rust=%s; %s\n", key, rust, e->s);
            na++;
            continue;
        }
        ck_c_text(e, c, sizeof(c));
        if (ck_equal(value, e)) {
            if (e->known != NULL) {
                fprintf(stdout,
                        "FAIL %s: c=%s now equals rust; remove the recorded difference (%s)\n", key,
                        c, e->known);
                fail++;
            } else {
                ok++;
            }
        } else if (e->known != NULL) {
            fprintf(stdout, "KNOWN %s: rust=%s c=%s (%s)\n", key, rust, c, e->known);
            known++;
        } else {
            fprintf(stdout, "FAIL %s: rust=%s c=%s (rust: %s)\n", key, rust, c,
                    source != NULL ? source : "?");
            fail++;
        }
    }
    for (i = 0; i < g_ck_n; i++) {
        if (!g_ck[i].seen) {
            fprintf(stdout, "FAIL %s: C declares it, the Rust document does not\n", g_ck[i].key);
            fail++;
        }
    }
    fprintf(stdout,
            "asx-conformance constants: rust=%s compared=%u ok=%u known=%u n/a=%u fail=%u\n",
            asx_json_get_string(&g_fixture, root, "asupersync_rev") != NULL
                ? asx_json_get_string(&g_fixture, root, "asupersync_rev")
                : "?",
            ok + known + fail, ok, known, na, fail);
    return fail == 0u && ok > 0u ? 0 : 1;
}

static int usage(void) {
    fprintf(stderr, "usage: asx-conformance run <scenario.json>\n"
                    "       asx-conformance compare <fixture.json>...\n"
                    "       asx-conformance self-test <fixture.json>\n"
                    "       asx-conformance canon <cases.jsonl>\n"
                    "       asx-conformance constants <rust_kernel_constants.json>\n");
    return 2;
}

int main(int argc, char **argv) {
    int i;
    unsigned pass = 0;
    unsigned fail = 0;
    unsigned error = 0;
    if (argc < 3) return usage();
    if (strcmp(argv[1], "run") == 0 && argc == 3) return cmd_run(argv[2]);
    if (strcmp(argv[1], "canon") == 0 && argc == 3) return cmd_canon(argv[2]);
    if (strcmp(argv[1], "constants") == 0 && argc == 3) return cmd_constants(argv[2]);
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
