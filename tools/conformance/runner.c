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
#include <asx/core/ghost.h>
#include <asx/core/transition.h>
#include <asx/runtime/lab.h>
#include <asx/runtime/runtime.h>
#include <asx/time/timer_wheel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FILE_CAP (4u * 1024u * 1024u)
#define BYTES_CAP (1024u * 1024u)

static char g_file[FILE_CAP];
static asx_json_doc g_fixture;
static asx_json_doc g_run;
static asx_json_doc g_digest;
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

/* Ghost-monitor assertion (bd-9kll.2.21). Built with
 * -DASX_CONFORMANCE_GHOST_CHECK (make conformance-ghost) against a library
 * that has the ghost monitors compiled in, a scenario whose run recorded a
 * ghost violation FAILs, naming the first. The one exception is a
 * linearity_leak of an obligation whose Rust trace has obligation.leaked: a
 * scenario that leaks on purpose, where Rust reports the same leak. Every
 * other violation, a leak Rust did not report included, still FAILs. The
 * plain runner does not look: the monitors judge the runtime's own
 * protocol, which no Rust capture carries, so a clean corpus here shows
 * legal lifecycle transitions and linear obligations in these scenarios and
 * nothing about memory safety (the sanitizer lane, bd-9kll.2.22, covers
 * that). The runtime of a run is still alive when asx_conformance_run
 * returns and the next run's init resets the monitors, so the violations
 * read here are that run's own. */
#ifdef ASX_CONFORMANCE_GHOST_CHECK
#if !(defined(ASX_DEBUG_GHOST) && ASX_DEBUG_GHOST)
#error "ASX_CONFORMANCE_GHOST_CHECK needs the ghost monitors: build with -DASX_DEBUG_GHOST=1"
#endif

static const char *ghost_state_name(asx_ghost_violation_kind kind, int state) {
    if (kind == ASX_GHOST_PROTOCOL_REGION) return asx_region_state_str((asx_region_state)state);
    if (kind == ASX_GHOST_PROTOCOL_TASK) return asx_task_state_str((asx_task_state)state);
    if (kind == ASX_GHOST_PROTOCOL_OBLIGATION) {
        return asx_obligation_state_str((asx_obligation_state)state);
    }
    return "-";
}

/* Whether Rust's captured trace has `obligation.leaked` for the obligation
 * named `name` ("<holder>/o<k>"): a scenario that leaks on purpose. */
static int fixture_expects_leak(uint32_t want_trace, const char *name) {
    uint32_t layer;
    uint32_t ev;
    if (want_trace == ASX_JSON_NONE || name == NULL) return 0;
    for (layer = asx_json_item(&g_fixture, want_trace, 0); layer != ASX_JSON_NONE;
         layer = g_fixture.nodes[layer].next) {
        for (ev = asx_json_item(&g_fixture, layer, 0); ev != ASX_JSON_NONE;
             ev = g_fixture.nodes[ev].next) {
            const char *k = asx_json_get_string(&g_fixture, ev, "k");
            const char *o = asx_json_get_string(&g_fixture, ev, "obligation");
            if (k != NULL && o != NULL && strcmp(k, "obligation.leaked") == 0 &&
                strcmp(o, name) == 0) {
                return 1;
            }
        }
    }
    return 0;
}

/* 1 when every violation the monitors recorded since the run began is a
 * leak Rust's trace `want_trace` (ASX_JSON_NONE: none) expects; otherwise
 * print a FAIL line for scenario `id` (the first violation not expected,
 * oldest retained) to `out` and return 0. `where` names the run that was
 * checked. Violations the ring no longer holds were not examined and count
 * against the run. */
static int ghost_clean(FILE *out, const char *id, const char *where, uint32_t want_trace) {
    asx_ghost_violation v;
    asx_ghost_violation first;
    const char *first_name = NULL;
    uint32_t total = asx_ghost_violation_count();
    uint32_t unexpected = 0u;
    uint32_t accepted = 0u;
    uint32_t i;
    int have_first = 0;
    char note[64];

    if (total == 0u) return 1;
    memset(&first, 0, sizeof(first));
    for (i = 0u; i < total && asx_ghost_violation_get(i, &v); i++) {
        const char *name = NULL;
        if (v.kind == ASX_GHOST_LINEARITY_LEAK) {
            name = asx_conformance_obligation_name(v.entity_id);
            if (fixture_expects_leak(want_trace, name)) {
                accepted++;
                continue;
            }
        }
        if (!have_first) {
            first = v;
            first_name = name;
            have_first = 1;
        }
        unexpected++;
    }
    unexpected += total - i;
    if (unexpected == 0u) {
        if (accepted > 0u) {
            fprintf(out,
                    "NOTE %s: ghost monitor accepted %u linearity_leak violation(s) in %s that "
                    "Rust's trace expects (obligation.leaked)\n",
                    id, (unsigned)accepted, where);
        }
        return 1;
    }
    note[0] = '\0';
    if (accepted > 0u) {
        (void)snprintf(note, sizeof(note), " [%u expected leak(s) accepted]", (unsigned)accepted);
    }
    if (!have_first) {
        fprintf(out,
                "FAIL %s: ghost monitor recorded %u violation(s) in %s beyond what its ring "
                "holds, %u not examined%s\n",
                id, (unsigned)total, where, (unsigned)unexpected, note);
        return 0;
    }
    fprintf(out,
            "FAIL %s: ghost monitor recorded %u violation(s) in %s%s; first: %s entity=0x%llx%s%s "
            "%s -> %s (violation sequence %u)%s\n",
            id, (unsigned)unexpected, where,
            asx_ghost_ring_overflowed() ? " (ring overflowed)" : "",
            asx_ghost_violation_kind_str(first.kind), (unsigned long long)first.entity_id,
            first.kind == ASX_GHOST_LINEARITY_LEAK ? " obligation=" : "",
            first.kind == ASX_GHOST_LINEARITY_LEAK ? (first_name != NULL ? first_name : "?") : "",
            ghost_state_name(first.kind, first.from_state),
            ghost_state_name(first.kind, first.to_state), (unsigned)first.sequence, note);
    return 0;
}
#else
#define ghost_clean(out, id, where, want_trace)                                                    \
    ((void)(out), (void)(id), (void)(where), (void)(want_trace), 1)
#endif

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

/* The lab's ScheduleCertificate (bd-9kll.4.8): a running hash of every
 * dispatch's Rust arena id, lane and step (runtime/scheduler/priority.rs,
 * ScheduleCertificate::record). The dispatch strings name tasks by scenario
 * name, so two runs can agree on them and still have polled different arena
 * slots; the certificate also counts the dispatches. Returns 1 when the run's
 * hash and count equal the fixture's, else prints both and returns 0. */
static int certificate_matches(const char *id, uint32_t want_schedule, uint32_t run) {
    const char *want_hash = asx_json_get_string(&g_fixture, want_schedule, "certificate_hash");
    const char *got_hash = asx_json_get_string(&g_run, run, "certificate_hash");
    uint64_t want_n = 0u;
    uint64_t got_n = 0u;
    int have_want_n =
        asx_json_u64(&g_fixture, asx_json_get(&g_fixture, want_schedule, "decisions"), &want_n);
    int have_got_n = asx_json_u64(&g_run, asx_json_get(&g_run, run, "decisions"), &got_n);

    if (want_hash == NULL || !have_want_n) {
        fprintf(stdout, "FAIL %s: fixture has no schedule.certificate_hash / decisions\n", id);
        return 0;
    }
    if (got_hash == NULL || !have_got_n) {
        fprintf(stdout, "FAIL %s: the C run produced no schedule certificate\n", id);
        return 0;
    }
    if (strcmp(want_hash, got_hash) == 0 && want_n == got_n) return 1;
    fprintf(stdout, "FAIL %s: schedule certificate\n", id);
    fprintf(stdout, "    rust: %s (%llu decisions)\n", want_hash, (unsigned long long)want_n);
    fprintf(stdout, "    c:    %s (%llu decisions)\n", got_hash, (unsigned long long)got_n);
    return 0;
}

typedef enum { CMP_PASS, CMP_FAIL, CMP_ERROR, CMP_SKIPPED } cmp_result;

/* Only a Rust capture is evidence (oracle self-test, bd-9kll.2.7): the
 * fixture must name twin_run as its producer, and its semantic digest must
 * recompute from its own trace, snapshot and observations, so a fixture
 * written by another engine or edited after capture (to make C pass) is an
 * ERROR, never a comparison. Returns 1 when authentic. */
static int fixture_authentic(uint32_t root, const char *id) {
    uint32_t prov = asx_json_get(&g_fixture, root, "provenance");
    const char *producer =
        prov != ASX_JSON_NONE ? asx_json_get_string(&g_fixture, prov, "producer") : NULL;
    const char *want = asx_json_get_string(&g_fixture, root, "semantic_digest");
    const char *vocabulary = asx_json_get_string(&g_fixture, root, "vocabulary");
    char got[ASX_CANON_DIGEST_LEN];
    uint32_t semantic;

    if (producer == NULL || strcmp(producer, "tools/twin_run") != 0) {
        fprintf(stdout, "ERROR %s: provenance.producer is not tools/twin_run: not a Rust capture\n",
                id);
        return 0;
    }
    asx_json_doc_init(&g_digest);
    semantic = asx_json_new_object(&g_digest);
    asx_json_set(&g_digest, semantic, "observations",
                 asx_json_copy(&g_digest, &g_fixture,
                               asx_json_get(&g_fixture, root, "observations")));
    asx_json_set(&g_digest, semantic, "snapshot",
                 asx_json_copy(&g_digest, &g_fixture, asx_json_get(&g_fixture, root, "snapshot")));
    asx_json_set(&g_digest, semantic, "trace",
                 asx_json_copy(&g_digest, &g_fixture, asx_json_get(&g_fixture, root, "trace")));
    asx_json_set(&g_digest, semantic, "vocabulary",
                 asx_json_new_string(&g_digest, vocabulary != NULL ? vocabulary : ""));
    if (want == NULL || !asx_json_doc_ok(&g_digest) ||
        asx_canon_digest(&g_digest, semantic, got) != ASX_OK || strcmp(want, got) != 0) {
        fprintf(stdout,
                "ERROR %s: semantic_digest does not recompute from the fixture's trace, snapshot "
                "and observations (edited after capture?)\n",
                id);
        return 0;
    }
    return 1;
}

/* Binary codec round trip of the run just executed (bd-9kll.2.9): the
 * runtime's own trace exported in the binary format must import back
 * and verify against itself (asx_trace_continuity_check), and the same
 * buffer with one event byte flipped must be refused. Returns 0 and
 * prints why on failure. */
static int trace_binary_round_trip(const char *id) {
    static uint8_t buf[ASX_TRACE_BINARY_HEADER + ASX_TRACE_BINARY_EVENT * ASX_TRACE_CAPACITY];
    uint32_t len = 0;
    asx_status st = asx_trace_export_binary(buf, (uint32_t)sizeof(buf), &len);
    if (st != ASX_OK) {
        fprintf(stdout, "FAIL %s: binary trace export: %s\n", id, asx_status_name(st));
        return 0;
    }
    st = asx_trace_continuity_check(buf, len);
    if (st != ASX_OK) {
        fprintf(stdout, "FAIL %s: binary trace round trip: %s\n", id, asx_status_name(st));
        return 0;
    }
    if (len > ASX_TRACE_BINARY_HEADER) {
        buf[len - 1u] = (uint8_t)(buf[len - 1u] ^ 0xFFu);
        st = asx_trace_continuity_check(buf, len);
        buf[len - 1u] = (uint8_t)(buf[len - 1u] ^ 0xFFu);
        if (st == ASX_OK) {
            fprintf(stdout, "FAIL %s: a corrupted binary trace was accepted\n", id);
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Replaying Rust's schedule in C (bd-9kll.9.3, bd-9kll.4.8)           */
/* ------------------------------------------------------------------ */

#define FORCED_MAX_DISPATCHES 16384u
#define FORCED_NAME_CAP 64u
#define FORCED_MAX_BYTES                                                                           \
    (ASX_FORCED_SCHEDULE_HEADER_BYTES +                                                            \
     FORCED_MAX_DISPATCHES * ASX_FORCED_SCHEDULE_DISPATCH_BYTES +                                  \
     ASX_FORCED_SCHEDULE_TERMINAL_BYTES + ASX_FORCED_SCHEDULE_CHECKSUM_BYTES)

static asx_forced_dispatch g_forced_storage[FORCED_MAX_DISPATCHES];
static uint8_t g_forced_bytes[FORCED_MAX_BYTES];
static uint8_t g_forced_again[FORCED_MAX_BYTES];
static char g_forced_names[FORCED_MAX_DISPATCHES][FORCED_NAME_CAP];
static const char *g_forced_name_ptrs[FORCED_MAX_DISPATCHES];
static unsigned g_forced_replayed;
static unsigned g_forced_unavailable;

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* The bytes of a lowercase hex string; 0 when it is not one that fits. */
static size_t hex_decode(const char *hex, uint8_t *out, size_t cap) {
    size_t n = strlen(hex);
    size_t i;
    if (n == 0u || n % 2u != 0u || n / 2u > cap) return 0u;
    for (i = 0; i < n / 2u; i++) {
        int hi = hex_nibble(hex[2u * i]);
        int lo = hex_nibble(hex[2u * i + 1u]);
        if (hi < 0 || lo < 0) return 0u;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return n / 2u;
}

/* The task of a "<step>@<ns> <task> <lane>" dispatch string. */
static int dispatch_task_name(const char *s, char *out, size_t cap) {
    const char *p = strchr(s, ' ');
    const char *q;
    size_t n;
    if (p == NULL) return 0;
    p++;
    q = strchr(p, ' ');
    if (q == NULL) return 0;
    n = (size_t)(q - p);
    if (n == 0u || n >= cap) return 0;
    memcpy(out, p, n);
    out[n] = '\0';
    return 1;
}

/* Replay the fixture's recorded Rust schedule on the C lab: every step's
 * task comes from the schedule (found by the name its dispatch string
 * gives), and the run must end where Rust's receipt says and project the
 * fixture's semantic digest. Also checks the artifact against itself: it
 * decodes, re-encodes to the same bytes, its terminal hash is the
 * fixture's certificate_hash and C's fold over its dispatches, and its
 * dispatches are the fixture's dispatch list. Returns 1 when all hold (or
 * the source was not quiescent and Rust wrote no schedule), else prints
 * why and returns 0. */
static int forced_replay(const char *id, uint32_t root, uint32_t scenario, uint32_t schedule,
                         uint32_t dispatches) {
    const char *hex = asx_json_get_string(&g_fixture, schedule, "forced_schedule");
    const char *want_cert = asx_json_get_string(&g_fixture, schedule, "certificate_hash");
    const char *want_sem = asx_json_get_string(&g_fixture, root, "semantic_digest");
    asx_forced_decode_limits limits;
    asx_forced_schedule sched;
    asx_forced_error ferr;
    size_t len;
    size_t again_len = 0;
    uint32_t run = ASX_JSON_NONE;
    uint32_t n;
    uint32_t i;
    char cert[17];
    const char *got_sem;
    asx_status st;

    if (hex == NULL) {
        g_forced_unavailable++;
        return 1;
    }
    len = hex_decode(hex, g_forced_bytes, sizeof(g_forced_bytes));
    if (len == 0u) {
        fprintf(stdout, "FAIL %s: schedule.forced_schedule is not hex bytes that fit\n", id);
        return 0;
    }
    limits.max_encoded_bytes = sizeof(g_forced_bytes);
    limits.max_dispatches = FORCED_MAX_DISPATCHES;
    memset(&ferr, 0, sizeof(ferr));
    st = asx_forced_schedule_decode(g_forced_bytes, len, &limits, g_forced_storage,
                                    FORCED_MAX_DISPATCHES, &sched, &ferr);
    if (st != ASX_OK) {
        fprintf(stdout, "FAIL %s: Rust's forced schedule does not decode: %s\n", id,
                asx_forced_error_kind_name(ferr.kind));
        return 0;
    }
    st = asx_forced_schedule_encode(&sched, g_forced_again, sizeof(g_forced_again), &again_len,
                                    &ferr);
    if (st != ASX_OK || again_len != len || memcmp(g_forced_again, g_forced_bytes, len) != 0) {
        fprintf(stdout, "FAIL %s: Rust's forced schedule does not re-encode to the same bytes\n",
                id);
        return 0;
    }
    (void)snprintf(cert, sizeof(cert), "%016llx", (unsigned long long)sched.terminal_schedule_hash);
    if (want_cert != NULL && strcmp(cert, want_cert) != 0) {
        fprintf(stdout,
                "FAIL %s: forced schedule's terminal hash %s is not the fixture's "
                "certificate_hash %s\n",
                id, cert, want_cert);
        return 0;
    }
    if (asx_forced_schedule_certificate(sched.dispatches, sched.dispatch_count) !=
        sched.terminal_schedule_hash) {
        fprintf(stdout,
                "FAIL %s: C's certificate fold over Rust's dispatches is not Rust's terminal "
                "hash\n",
                id);
        return 0;
    }
    n = asx_json_count(&g_fixture, dispatches);
    if (n != sched.dispatch_count) {
        fprintf(stdout, "FAIL %s: schedule.dispatches has %u entries, the forced schedule %u\n", id,
                (unsigned)n, (unsigned)sched.dispatch_count);
        return 0;
    }
    for (i = 0; i < n; i++) {
        const char *d = dispatch_at(&g_fixture, dispatches, i);
        if (d == NULL || !dispatch_task_name(d, g_forced_names[i], FORCED_NAME_CAP)) {
            fprintf(stdout, "FAIL %s: schedule.dispatches[%u] names no task\n", id, (unsigned)i);
            return 0;
        }
        g_forced_name_ptrs[i] = g_forced_names[i];
    }

    asx_conformance_force_schedule(&sched, g_forced_name_ptrs);
    asx_json_doc_init(&g_digest);
    st = asx_conformance_run(&g_fixture, scenario, &g_digest, &run, g_error, sizeof(g_error));
    asx_conformance_force_schedule(NULL, NULL);
    if (st != ASX_OK) {
        fprintf(stdout, "FAIL %s: replaying Rust's schedule in C: %s\n", id,
                g_error[0] != '\0' ? g_error : "?");
        return 0;
    }
    if (!ghost_clean(stdout, id, "the replay of Rust's schedule",
                     asx_json_get(&g_fixture, root, "trace"))) {
        return 0;
    }
    got_sem = asx_json_get_string(&g_digest, run, "semantic_digest");
    if (got_sem == NULL || want_sem == NULL || strcmp(got_sem, want_sem) != 0) {
        fprintf(stdout, "FAIL %s: replaying Rust's schedule in C: semantic digest %s, Rust's %s\n",
                id, got_sem != NULL ? got_sem : "?", want_sem != NULL ? want_sem : "?");
        return 0;
    }
    g_forced_replayed++;
    return 1;
}

/* Scenarios skipped because they declare a requirement this build's profile
 * forbids (DEC-008), and the summary says so. */
static unsigned g_skipped_forbidden;

/* The scenario's "requires" against this build's profile. Returns 1 when
 * the profile forbids what the scenario declares: the run has nothing to
 * prove (the channel is refused by design), so it is skipped and counted,
 * never passed. Returns -1, printing ERROR, for a declaration the
 * interpreter does not know; 0 to run. A skip needs both halves: a
 * scenario that does not declare a requirement is never skipped, whatever
 * the profile refuses, and a profile that allows it runs the scenario. */
static int requirement_skip(const char *id, uint32_t scenario) {
    const char *name = NULL;
    asx_profile_id profile = asx_profile_active();
    int forbidden = asx_conformance_requirement_forbidden(&g_fixture, scenario, profile, &name);
    if (forbidden < 0) {
        fprintf(stdout,
                "ERROR %s: the scenario's \"requires\" is not a list of known requirements\n", id);
        return -1;
    }
    if (forbidden > 0) {
        fprintf(stdout, "SKIP %s: the %s profile forbids %s (DEC-008)\n", id,
                asx_profile_name(profile), name);
        g_skipped_forbidden++;
    }
    return forbidden;
}

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
    if (!fixture_authentic(root, id)) return CMP_ERROR;
    switch (requirement_skip(id, scenario)) {
    case -1: return CMP_ERROR;
    case 1: return CMP_SKIPPED;
    default: break;
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
    /* Not under the self-test: a corrupted expectation must be what fails. */
    if (!mutate && !ghost_clean(stdout, id, "the C run", want_trace)) ok = 0;
    if (!trace_binary_round_trip(id)) ok = 0;

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
    if (!certificate_matches(id, schedule, run)) ok = 0;
    if (!mutate) {
        /* Rust's own schedule, replayed in C (bd-9kll.9.3). It runs last:
         * it replaces the runtime and `g_digest`, not `g_run`. */
        int scheduling_only = ok;
        if (!forced_replay(id, root, scenario, schedule, want_dispatches)) {
            ok = 0;
        } else if (!scheduling_only) {
            fprintf(stdout,
                    "    under Rust's own schedule C's run is exact: the difference above is in "
                    "scheduling alone\n");
        }
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
    if (!ghost_clean(stderr, path, "the C run", ASX_JSON_NONE)) return 1;
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
    uint64_t u;  /* CK_U64; CK_BOOL as 0/1 */
    char s[160]; /* CK_STR: the value; CK_NA: why C has no counterpart */
    int as_set;  /* CK_STR from a list: compare the names sorted */
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

/* C's "unconstrained" encodings (deadline ASX_TIME_INFINITE, cost
 * UINT64_MAX) are Rust's None. */
static void ck_budget(const char *prefix, asx_budget b) {
    char key[64];
    (void)snprintf(key, sizeof(key), "%s.deadline_ns", prefix);
    if (b.deadline == ASX_TIME_INFINITE) {
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
#if ASX_HAS_BROWSER_TRACE
    asx_lab_config lab;
#endif
    asx_supervisor_config sup;
    asx_cancel_reason reason;
    char key[64];
    char order[160];
    size_t used = 0;
    uint32_t i;
    int ordinal;

    g_ck_n = 0;
    g_ck_overflow = 0;

    /* Evaluate the actual C policy against linked-Rust predicate captures. */
    for (i = 0; i < (uint32_t)(sizeof(k_region_states) / sizeof(k_region_states[0])); i++) {
        asx_region_state state = (asx_region_state)k_region_states[i].c;
        (void)snprintf(key, sizeof(key), "region_state.%s.can_accept_work",
                       k_region_states[i].rust);
        ck_bool(key, asx_region_can_accept_work(state));
        (void)snprintf(key, sizeof(key), "region_state.%s.can_accept_cleanup_work",
                       k_region_states[i].rust);
        ck_bool(key, asx_region_can_accept_cleanup_work(state));
    }

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

#if ASX_HAS_BROWSER_TRACE
    asx_lab_config_init(&lab);
    ck_u64("lab.default.seed", lab.seed);
    ck_bool("lab.default.panic_on_obligation_leak", rt.leak_response == ASX_LEAK_PANIC);
#else
    ck_na("lab.default.seed", "minimal browser excludes public Lab factories");
    ck_na("lab.default.panic_on_obligation_leak", "minimal browser excludes public Lab factories");
#endif
    ck_na("lab.default.max_steps",
          "asx_lab counts scenario steps (ASX_LAB_MAX_STEPS), not scheduler steps");
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
}

/* The JSON value as C text, for messages. */
static void ck_rust_text(uint32_t v, char *buf, size_t cap) {
    size_t n;
    if (!canonical(&g_fixture, v, g_a)) {
        (void)snprintf(buf, cap, "?");
        return;
    }
    if (cap == 0u) return;
    n = strlen(g_a);
    if (n >= cap) n = cap - 1u;
    memcpy(buf, g_a, n);
    buf[n] = '\0';
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
        fprintf(stderr, "asx-conformance: the C constants table overflowed\n");
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
            ok++;
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
    fprintf(stdout, "asx-conformance constants: rust=%s compared=%u ok=%u n/a=%u fail=%u\n",
            asx_json_get_string(&g_fixture, root, "asupersync_rev") != NULL
                ? asx_json_get_string(&g_fixture, root, "asupersync_rev")
                : "?",
            ok + fail, ok, na, fail);
    return fail == 0u && ok > 0u ? 0 : 1;
}

static int usage(void) {
    fprintf(stderr, "usage: asx-conformance run <scenario.json>\n"
                    "       asx-conformance compare <fixture.json>...\n"
                    "       asx-conformance compare-sweep [--trace] [--known <file>] "
                    "<fixture.json>...\n"
                    "       asx-conformance self-test <fixture.json>\n"
                    "       asx-conformance canon <cases.jsonl>\n"
                    "       asx-conformance constants <rust_kernel_constants.json>\n");
    return 2;
}

/* ---- Sweep mode (bd-9kll.4.9) -------------------------------------------
 *
 * Every other comparison here runs the scenario under lab dispatch, so the
 * scheduler a live user runs (asx_scheduler_run without it) is compared
 * with nothing. `compare-sweep` runs each fixture's scenario under the
 * default sweep scheduler and compares only what a scheduler that picks
 * differently must still agree on: the final snapshot and the per-task
 * observations (both canonical, so independent of dispatch order). Not the
 * trace, the dispatch order or the certificate.
 *
 * A divergence is either a real semantic difference or an order dependence
 * the scenario has by design. The second kind is listed, one line each with
 * its reason, in the known-divergence file (id, whitespace, reason); a
 * fixture that diverges and is not listed FAILs, and one that is listed
 * but now matches is STALE and fails too, so the list can neither hide a
 * new divergence nor rot. */

#define KNOWN_CAP 256u
#define KNOWN_ID_CAP 96u
#define KNOWN_REASON_CAP 192u

typedef struct {
    char id[KNOWN_ID_CAP];
    char reason[KNOWN_REASON_CAP];
    int used; /* a compared fixture carried this id */
} known_divergence;

static known_divergence g_known[KNOWN_CAP];
static uint32_t g_n_known;

/* Read `id reason...` lines ('#' comments and blank lines skipped). A line
 * with no reason is refused: a listing is a justification. Returns 1 on
 * success. */
static int load_known(const char *path) {
    FILE *f = fopen(path, "r");
    char line[KNOWN_ID_CAP + KNOWN_REASON_CAP + 8u];
    unsigned long lineno = 0;
    int ok = 1;

    if (f == NULL) {
        fprintf(stderr, "asx-conformance: cannot open %s\n", path);
        return 0;
    }
    while (fgets(line, (int)sizeof(line), f) != NULL) {
        char *p = line;
        const char *id;
        char *reason;
        size_t n;
        size_t id_len;
        lineno++;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;
        id = p;
        while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\n') p++;
        if (*p != '\0') *p++ = '\0';
        while (*p == ' ' || *p == '\t') p++;
        reason = p;
        n = strlen(reason);
        while (n > 0u && (reason[n - 1u] == '\n' || reason[n - 1u] == ' ')) reason[--n] = '\0';
        if (*id == '\0' || n == 0u) {
            fprintf(stderr, "asx-conformance: %s:%lu: expected `<scenario_id> <reason>`\n", path,
                    lineno);
            ok = 0;
            continue;
        }
        id_len = strlen(id);
        if (g_n_known >= KNOWN_CAP || id_len >= KNOWN_ID_CAP || n >= KNOWN_REASON_CAP) {
            fprintf(stderr, "asx-conformance: %s:%lu: entry too long or list full\n", path, lineno);
            ok = 0;
            continue;
        }
        memcpy(g_known[g_n_known].id, id, id_len + 1u);
        memcpy(g_known[g_n_known].reason, reason, n + 1u);
        g_known[g_n_known].used = 0;
        g_n_known++;
    }
    fclose(f);
    return ok;
}

static known_divergence *find_known(const char *id) {
    uint32_t i;
    for (i = 0; i < g_n_known; i++) {
        if (strcmp(g_known[i].id, id) == 0) return &g_known[i];
    }
    return NULL;
}

typedef enum {
    SWEEP_PASS,
    SWEEP_KNOWN,
    SWEEP_FAIL,
    SWEEP_STALE,
    SWEEP_ERROR,
    SWEEP_SKIPPED
} sweep_result;

/* The first differing member of each snapshot part and of the observations,
 * printed for a divergence nobody has explained. */
static void sweep_detail(uint32_t want_snapshot, uint32_t want_obs, uint32_t run, int snap_ok,
                         int obs_ok) {
    if (!snap_ok) {
        uint32_t got = asx_json_get(&g_run, run, "snapshot");
        static const char *const parts[] = {"tasks", "regions", "obligations"};
        size_t k;
        for (k = 0; k < sizeof(parts) / sizeof(parts[0]); k++) {
            char label[32];
            (void)snprintf(label, sizeof(label), "snapshot.%s", parts[k]);
            (void)diff_members(label, asx_json_get(&g_fixture, want_snapshot, parts[k]),
                               asx_json_get(&g_run, got, parts[k]));
        }
        {
            static const char *const scalars[] = {"now_ns", "quiescent", "timers_pending",
                                                  "channels"};
            for (k = 0; k < sizeof(scalars) / sizeof(scalars[0]); k++) {
                uint32_t w = asx_json_get(&g_fixture, want_snapshot, scalars[k]);
                uint32_t g = asx_json_get(&g_run, got, scalars[k]);
                if (w == ASX_JSON_NONE || g == ASX_JSON_NONE || !same_part(w, g)) {
                    fprintf(stdout, "    snapshot.%s differs\n", scalars[k]);
                    if (w != ASX_JSON_NONE && canonical(&g_fixture, w, g_a)) show("rust:", g_a);
                    if (g != ASX_JSON_NONE && canonical(&g_run, g, g_b)) show("c:   ", g_b);
                }
            }
        }
    }
    if (!obs_ok) {
        (void)diff_items("observations", want_obs, asx_json_get(&g_run, run, "observations"));
    }
}

/* --trace: the fixture's trace and the sweep run's, one Foata layer per line.
 * The two orders differ by design; what to read is which events one side has
 * and the other lacks, and their payloads. */
static int g_sweep_trace;

static void trace_dump(const char *label, const asx_json_doc *doc, uint32_t trace, char *buf) {
    uint32_t n;
    uint32_t i;
    if (trace == ASX_JSON_NONE) return;
    n = asx_json_count(doc, trace);
    fprintf(stdout, "    %s trace, %u layers\n", label, (unsigned)n);
    for (i = 0; i < n; i++) {
        if (canonical(doc, asx_json_item(doc, trace, i), buf)) {
            fprintf(stdout, "      %3u %s\n", (unsigned)i, buf);
        }
    }
}

/* One fixture under the sweep scheduler. */
static sweep_result sweep_one(const char *path) {
    uint32_t root;
    uint32_t scenario;
    uint32_t run = ASX_JSON_NONE;
    uint32_t want_snapshot;
    uint32_t want_obs;
    const char *id;
    known_divergence *known;
    char why[256];
    asx_status st;
    int snap_ok = 0;
    int obs_ok = 0;
    int diverged = 0;

    if (!load(path, &g_fixture, &root)) return SWEEP_ERROR;
    id = asx_json_get_string(&g_fixture, root, "scenario_id");
    scenario = asx_json_get(&g_fixture, root, "scenario");
    want_snapshot = asx_json_get(&g_fixture, root, "snapshot");
    want_obs = asx_json_get(&g_fixture, root, "observations");
    if (id == NULL || scenario == ASX_JSON_NONE || want_snapshot == ASX_JSON_NONE ||
        want_obs == ASX_JSON_NONE) {
        fprintf(stdout,
                "ERROR %s: not an asx.fixture.v2 file with scenario, snapshot and "
                "observations\n",
                path);
        return SWEEP_ERROR;
    }
    if (!fixture_authentic(root, id)) return SWEEP_ERROR;
    switch (requirement_skip(id, scenario)) {
    case -1: return SWEEP_ERROR;
    case 1: return SWEEP_SKIPPED;
    default: break;
    }
    known = find_known(id);

    why[0] = '\0';
    asx_json_doc_init(&g_run);
    st = asx_conformance_run_sweep(&g_fixture, scenario, &g_run, &run, g_error, sizeof(g_error));
    if (st != ASX_OK) {
        /* The scenario could not be played under the sweep at all. */
        (void)snprintf(why, sizeof(why), "sweep run failed: %.200s",
                       g_error[0] != '\0' ? g_error : "?");
        diverged = 1;
    } else {
        snap_ok = same_part(want_snapshot, asx_json_get(&g_run, run, "snapshot"));
        obs_ok = same_part(want_obs, asx_json_get(&g_run, run, "observations"));
        if (!snap_ok || !obs_ok) {
            (void)snprintf(why, sizeof(why), "%s%s%s", snap_ok ? "" : "snapshot",
                           (!snap_ok && !obs_ok) ? " and " : "", obs_ok ? "" : "observations");
            diverged = 1;
        }
    }

    if (!diverged) {
        if (known != NULL) {
            known->used = 1;
            fprintf(stdout,
                    "STALE %s: listed as order-dependent (%s) but matches Rust under the sweep; "
                    "remove it from the list\n",
                    id, known->reason);
            return SWEEP_STALE;
        }
        fprintf(stdout, "PASS %s\n", id);
        return SWEEP_PASS;
    }
    if (known != NULL) {
        known->used = 1;
        fprintf(stdout, "KNOWN %s: %s differ; %s\n", id, why, known->reason);
        if (g_sweep_trace && st == ASX_OK) {
            sweep_detail(want_snapshot, want_obs, run, snap_ok, obs_ok);
            trace_dump("rust", &g_fixture, asx_json_get(&g_fixture, root, "trace"), g_a);
            trace_dump("c", &g_run, asx_json_get(&g_run, run, "trace"), g_b);
        }
        return SWEEP_KNOWN;
    }
    fprintf(stdout, "FAIL %s: %s differ under the sweep\n", id, why);
    if (st == ASX_OK) sweep_detail(want_snapshot, want_obs, run, snap_ok, obs_ok);
    if (g_sweep_trace && st == ASX_OK) {
        trace_dump("rust", &g_fixture, asx_json_get(&g_fixture, root, "trace"), g_a);
        trace_dump("c", &g_run, asx_json_get(&g_run, run, "trace"), g_b);
    }
    return SWEEP_FAIL;
}

/* compare-sweep [--trace] [--known FILE] fixture.json... */
static int cmd_compare_sweep(int argc, char **argv) {
    unsigned counts[6] = {0u, 0u, 0u, 0u, 0u, 0u};
    unsigned unmatched = 0u;
    int i = 2;
    uint32_t k;

    if (i < argc && strcmp(argv[i], "--trace") == 0) {
        g_sweep_trace = 1;
        i++;
    }
    if (i + 1 < argc && strcmp(argv[i], "--known") == 0) {
        if (!load_known(argv[i + 1])) return 2;
        i += 2;
    }
    if (i >= argc) return usage();
    for (; i < argc; i++) counts[sweep_one(argv[i])]++;
    /* A listed id that no compared fixture carries is a typo or a removed
     * scenario: the list must not name things that do not exist. */
    for (k = 0; k < g_n_known; k++) {
        if (!g_known[k].used) {
            fprintf(stdout, "STALE %s: listed but no compared fixture has this id\n",
                    g_known[k].id);
            unmatched++;
        }
    }
    fprintf(stdout,
            "asx-conformance sweep: pass=%u known=%u fail=%u stale=%u error=%u unmatched=%u "
            "skipped_forbidden=%u\n",
            counts[SWEEP_PASS], counts[SWEEP_KNOWN], counts[SWEEP_FAIL], counts[SWEEP_STALE],
            counts[SWEEP_ERROR], unmatched, counts[SWEEP_SKIPPED]);
    return (counts[SWEEP_FAIL] == 0u && counts[SWEEP_STALE] == 0u && counts[SWEEP_ERROR] == 0u &&
            unmatched == 0u)
               ? 0
               : 1;
}

int main(int argc, char **argv) {
    int i;
    unsigned pass = 0;
    unsigned fail = 0;
    unsigned error = 0;
    if (argc < 3) return usage();
    /* ASX_ORACLES=all, or a comma-separated list of Rust oracle names
     * (docs/LAB_ORACLE_CATALOG.md), runs the kernel-state oracles at the end of
     * every scenario this process runs. */
    asx_conformance_set_oracles(getenv("ASX_ORACLES"));
    if (strcmp(argv[1], "run") == 0 && argc == 3) return cmd_run(argv[2]);
    if (strcmp(argv[1], "canon") == 0 && argc == 3) return cmd_canon(argv[2]);
    if (strcmp(argv[1], "constants") == 0 && argc == 3) return cmd_constants(argv[2]);
    if (strcmp(argv[1], "compare-sweep") == 0) return cmd_compare_sweep(argc, argv);
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
        case CMP_SKIPPED: break; /* counted by requirement_skip */
        }
    }
    fprintf(stdout,
            "asx-conformance: compared=%u pass=%u fail=%u error=%u forced_replayed=%u "
            "forced_unavailable=%u skipped_forbidden=%u\n",
            (unsigned)(argc - 2), pass, fail, error, g_forced_replayed, g_forced_unavailable,
            g_skipped_forbidden);
    return (fail == 0u && error == 0u) ? 0 : 1;
}
