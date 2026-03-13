/*
 * vignette_replay.c — API ergonomics vignette: trace and replay
 *
 * Exercises: trace event capture, digest computation, binary
 * export/import, replay verification, and snapshot capture.
 * Demonstrates deterministic execution identity.
 *
 * bd-56t.5 — API ergonomics validation gate
 * SPDX-License-Identifier: MIT
 */
/* ASX_CHECKPOINT_WAIVER_FILE("vignette: no kernel loops") */

#include <asx/asx.h>
#include <stdio.h>
#include <string.h>

/* Deterministic task: always does the same thing for same input. */
typedef struct {
    asx_co_state co;
    int value;
} det_state;

static asx_status poll_deterministic(void *ud, asx_task_id self) {
    det_state *s = (det_state *)ud;
    (void)self;

    ASX_CO_BEGIN(&s->co);
    s->value = 1;
    ASX_CO_YIELD(&s->co);
    s->value = 2;
    ASX_CO_YIELD(&s->co);
    s->value = 3;
    ASX_CO_END(&s->co);
}

/* Helper: run a standard scenario and return the trace digest. */
static int run_scenario(uint64_t *out_digest) {
    asx_status st;
    asx_region_id region;
    asx_task_id task;
    asx_budget budget;
    void *state_ptr = NULL;
    det_state *ds;

    asx_runtime_reset();
    asx_trace_reset();

    st = asx_region_open(&region);
    if (st != ASX_OK) return 1;

    st = asx_task_spawn_captured(region, poll_deterministic, (uint32_t)sizeof(det_state), NULL,
                                 &task, &state_ptr);
    if (st != ASX_OK) return 1;

    ds = (det_state *)state_ptr;
    ds->co = (asx_co_state)ASX_CO_STATE_INIT;
    ds->value = 0;

    budget = asx_budget_from_polls(100);
    st = asx_region_drain(region, &budget);
    if (st != ASX_OK) return 1;

    /*
     * ERGO: asx_trace_digest() returns the hash directly — no out-param
     * pattern. This is nice for a value that can't fail. Good use of
     * direct return for infallible operations.
     */
    *out_digest = asx_trace_digest();
    return 0;
}

/* -------------------------------------------------------------------
 * Scenario 1: Trace capture and digest
 * ------------------------------------------------------------------- */
static int scenario_trace_digest(void) {
    uint64_t digest1, digest2;

    printf("--- scenario: trace digest ---\n");

    if (run_scenario(&digest1) != 0) {
        printf("  FAIL: first run failed\n");
        return 1;
    }
    printf("  first run digest:  0x%016llx\n", (unsigned long long)digest1);

    /*
     * ERGO: Running the same scenario twice should produce the same
     * digest. This is the core determinism guarantee. The API makes
     * it easy to verify: just call asx_trace_digest() after each run.
     */
    if (run_scenario(&digest2) != 0) {
        printf("  FAIL: second run failed\n");
        return 1;
    }
    printf("  second run digest: 0x%016llx\n", (unsigned long long)digest2);

    if (digest1 != digest2) {
        printf("  FAIL: digest mismatch for identical deterministic scenario\n");
        return 1;
    }

    printf("  PASS: trace digest API usage\n");
    return 0;
}

/* -------------------------------------------------------------------
 * Scenario 2: Trace event inspection
 * ------------------------------------------------------------------- */
static int scenario_event_inspection(void) {
    uint64_t digest;
    uint32_t count;
    asx_trace_event ev;

    printf("--- scenario: event inspection ---\n");

    if (run_scenario(&digest) != 0) {
        printf("  FAIL: scenario run failed\n");
        return 1;
    }

    /*
     * ERGO: asx_trace_event_count() + asx_trace_event_get(index, &ev)
     * is a clean iteration pattern. The index-based API avoids exposing
     * internal ring buffer details. Each event has kind, entity_id,
     * aux, and sequence — enough for diagnostics and conformance.
     */
    count = asx_trace_event_count();
    printf("  trace contains %u events\n", count);

    if (count == 0) {
        printf("  FAIL: expected at least one trace event\n");
        return 1;
    }

    /* Print first few events for illustration. */
    for (uint32_t i = 0; i < count && i < 5; i++) {
        if (asx_trace_event_get(i, &ev)) {
            /*
             * ERGO: asx_trace_event_kind_str() returns a human-readable
             * name. Good for diagnostics. Naming is consistent with
             * other _str() functions across the API.
             */
            printf("  [%u] %s entity=0x%llx aux=%llu\n", ev.sequence,
                   asx_trace_event_kind_str(ev.kind), (unsigned long long)ev.entity_id,
                   (unsigned long long)ev.aux);
        }
    }

    printf("  PASS: event inspection\n");
    return 0;
}

/* -------------------------------------------------------------------
 * Scenario 3: Binary trace export and import
 * ------------------------------------------------------------------- */
static int scenario_binary_roundtrip(void) {
    uint64_t digest;
    asx_status st;
    uint8_t buf[8192];
    uint32_t buf_len = 0;
    asx_replay_result rr;

    printf("--- scenario: binary trace roundtrip ---\n");

    if (run_scenario(&digest) != 0) {
        printf("  FAIL: scenario run failed\n");
        return 1;
    }

    /*
     * ERGO: asx_trace_export_binary(buf, capacity, &out_len) follows
     * the standard C buffer pattern. The capacity parameter prevents
     * overflows and ASX_E_BUFFER_TOO_SMALL is returned if insufficient.
     * This is idiomatic and safe.
     */
    st = asx_trace_export_binary(buf, (uint32_t)sizeof(buf), &buf_len);
    if (st != ASX_OK) {
        printf("  FAIL: export_binary returned %s\n", asx_status_str(st));
        return 1;
    }
    printf("  exported %u bytes of binary trace\n", buf_len);

    /* Re-run the same scenario. */
    if (run_scenario(&digest) != 0) {
        printf("  FAIL: re-run failed\n");
        return 1;
    }

    /*
     * ERGO: asx_trace_import_binary loads the exported trace as a
     * replay reference. Then asx_replay_verify() compares the current
     * trace against it. This two-step pattern gives control over when
     * the comparison happens.
     *
     * ALTERNATIVE: asx_trace_continuity_check(buf, len) combines both
     * steps into one call — good for the common case where you just
     * want to know "did it match?"
     */
    st = asx_trace_continuity_check(buf, buf_len);
    if (st != ASX_OK) {
        printf("  FAIL: continuity_check returned %s\n", asx_status_str(st));
        return 1;
    }

    /* Also exercise the two-step replay API path. */
    st = asx_trace_import_binary(buf, buf_len);
    if (st != ASX_OK) {
        printf("  FAIL: import_binary returned %s\n", asx_status_str(st));
        return 1;
    }
    rr = asx_replay_verify();
    printf("  replay_verify result=%s index=%u\n", asx_replay_result_kind_str(rr.result),
           rr.divergence_index);
    if (rr.result != ASX_REPLAY_MATCH) {
        printf("  FAIL: replay_verify expected match\n");
        return 1;
    }

    printf("  PASS: binary trace roundtrip API usage\n");
    return 0;
}

/* -------------------------------------------------------------------
 * Scenario 4: Snapshot capture
 * ------------------------------------------------------------------- */
static int scenario_snapshot(void) {
    uint64_t digest;
    asx_snapshot_buffer snap1, snap2;
    uint64_t snap_digest1, snap_digest2;

    printf("--- scenario: snapshot capture ---\n");

    /*
     * ERGO: asx_snapshot_capture(&buf) fills a buffer with the current
     * runtime state (JSON object with regions, tasks, obligations).
     * asx_snapshot_digest(&buf) returns a FNV-1a hash of that buffer.
     * Both are deterministic for the same runtime state.
     */
    if (run_scenario(&digest) != 0) {
        printf("  FAIL: scenario run failed\n");
        return 1;
    }

    if (asx_snapshot_capture(&snap1) != ASX_OK) {
        printf("  FAIL: snapshot_capture failed\n");
        return 1;
    }

    if (snap1.len == 0) {
        printf("  FAIL: snapshot should not be empty\n");
        return 1;
    }
    printf("  snapshot captured: %u bytes\n", snap1.len);

    snap_digest1 = asx_snapshot_digest(&snap1);
    if (snap_digest1 == 0) {
        printf("  FAIL: snapshot digest should not be zero\n");
        return 1;
    }
    printf("  snapshot digest: 0x%016llx\n", (unsigned long long)snap_digest1);

    /*
     * ERGO: Replaying the same deterministic scenario should produce
     * the same snapshot digest. This confirms snapshot determinism
     * in addition to trace determinism (scenario 1).
     */
    if (run_scenario(&digest) != 0) {
        printf("  FAIL: second scenario run failed\n");
        return 1;
    }

    if (asx_snapshot_capture(&snap2) != ASX_OK) {
        printf("  FAIL: second snapshot_capture failed\n");
        return 1;
    }

    snap_digest2 = asx_snapshot_digest(&snap2);
    if (snap_digest1 != snap_digest2) {
        printf("  FAIL: snapshot digest mismatch across identical runs\n");
        return 1;
    }
    printf("  PASS: snapshot capture and digest determinism\n");
    return 0;
}

/* -------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------- */
int main(void) {
    int failures = 0;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== vignette: replay ===\n\n");

    failures += scenario_trace_digest();
    failures += scenario_event_inspection();
    failures += scenario_binary_roundtrip();
    failures += scenario_snapshot();

    printf("\n=== replay: %d failures ===\n", failures);
    return failures;
}
