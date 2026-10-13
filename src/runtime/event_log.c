/*
 * event_log.c — operation-level event recording with FNV-1a hash chain
 *
 * Implements the API declared in asx/runtime/event.h:
 *   - Fixed-capacity ring buffer for event records
 *   - Running FNV-1a hash chain over (kind, entity, sequence) tuples
 *   - JSON serialization of recorded events
 *   - Replay verification via event-by-event comparison
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx_status.h>
#include <asx/core/ghost.h>
#include <asx/runtime/event.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* FNV-1a constants                                                    */
/* ------------------------------------------------------------------ */

#define FNV_OFFSET 14695981039346656037ULL
#define FNV_PRIME 1099511628211ULL

/* ------------------------------------------------------------------ */
/* Global state                                                        */
/* ------------------------------------------------------------------ */

static asx_event_record g_event_log[ASX_EVENT_LOG_CAPACITY];
static uint32_t g_event_count;
static uint64_t g_hash_chain;

/* Planted memory bug (bd-9kll.2.22), compiled only in the gate build
 * tests/gates/test_conformance_monitors.sh: every logged event reads one
 * byte past the end of this table and drops the value. No result, trace or
 * digest changes, so a parity compare passes; AddressSanitizer reports a
 * global-buffer-overflow. The volatile index keeps the compiler from seeing
 * it, and the read goes through a pointer (see asx_event_emit). */
#ifdef ASX_MUTANT_SANITIZER_OOB_READ
static uint8_t g_mutant_oob_table[4] = {1u, 2u, 3u, 4u};
static volatile uint32_t g_mutant_oob_index = 4u;
static volatile uint8_t g_mutant_oob_sink;
#endif

/* ------------------------------------------------------------------ */
/* Internal: hash a single byte into the chain                         */
/* ------------------------------------------------------------------ */

static uint64_t fnv_byte(uint64_t h, unsigned char b) {
    h ^= (uint64_t)b;
    h *= FNV_PRIME;
    return h;
}

static uint64_t fnv_u32(uint64_t h, uint32_t v) {
    h = fnv_byte(h, (unsigned char)(v & 0xFFu));
    h = fnv_byte(h, (unsigned char)((v >> 8) & 0xFFu));
    h = fnv_byte(h, (unsigned char)((v >> 16) & 0xFFu));
    h = fnv_byte(h, (unsigned char)((v >> 24) & 0xFFu));
    return h;
}

static uint64_t fnv_u64(uint64_t h, uint64_t v) {
    h = fnv_u32(h, (uint32_t)(v & 0xFFFFFFFFu));
    h = fnv_u32(h, (uint32_t)((v >> 32) & 0xFFFFFFFFu));
    return h;
}

/* ------------------------------------------------------------------ */
/* API implementation                                                  */
/* ------------------------------------------------------------------ */

void asx_event_log_reset(void) {
    /* Records are written only at indices below the count, so only that
     * prefix (at most the capacity) is dirty. */
    uint32_t used = g_event_count < ASX_EVENT_LOG_CAPACITY ? g_event_count : ASX_EVENT_LOG_CAPACITY;

    memset(g_event_log, 0, (size_t)used * sizeof(g_event_log[0]));
    g_event_count = 0;
    g_hash_chain = FNV_OFFSET;
}

uint32_t asx_event_emit(asx_event_kind kind, uint64_t entity_id, uint64_t parent_id,
                        asx_status status) {
    uint32_t seq = g_event_count;

#ifdef ASX_MUTANT_SANITIZER_OOB_READ
    {
        /* Read through a pointer: -fsanitize=bounds checks subscripts of
         * declared arrays and would halt first with its own report, so the
         * gate's AddressSanitizer expectation would never see the
         * global-buffer-overflow. */
        const volatile uint8_t *table = g_mutant_oob_table;
        /* ASX_ANALYZER_WAIVER("planted mutant: built only by test_conformance_monitors.sh") */
        g_mutant_oob_sink = table[g_mutant_oob_index];
    }
#endif
#ifdef ASX_MUTANT_GHOST_ILLEGAL_TRANSITION
    /* Planted ghost-monitor bug (bd-9kll.2.21): logging a spawn also tells
     * the protocol monitor the task left Completed for Created, an illegal
     * transition. Nothing the runtime does, traces or digests changes, so a
     * parity compare passes; only a ghost-checked conformance run sees it. */
    if (kind == ASX_EVENT_TASK_SPAWN) {
        (void)asx_ghost_check_task_transition((asx_task_id)entity_id, ASX_TASK_COMPLETED,
                                              ASX_TASK_CREATED);
    }
#endif
#ifdef ASX_MUTANT_GHOST_UNEXPECTED_LEAK
    /* Planted ghost-monitor bug (bd-9kll.2.21): logging a reservation also
     * asks the linearity monitor about the obligation's region while the
     * obligation is still reserved, so it reports a leak of a token Rust
     * commits or aborts, with no obligation.leaked in Rust's trace. The
     * runner must FAIL it; only a leak the capture expects is accepted.
     * Nothing the runtime does, traces or digests changes. */
    if (kind == ASX_EVENT_OBLIGATION_CREATE) {
        (void)asx_ghost_check_obligation_leaks((asx_region_id)parent_id);
    }
#endif

    if (g_event_count < ASX_EVENT_LOG_CAPACITY) {
        asx_event_record *e = &g_event_log[g_event_count];
        e->kind = kind;
        e->entity_id = entity_id;
        e->parent_id = parent_id;
        e->sequence = seq;
        e->status = status;
    }

    /* Hash chain always advances even if ring is full */
    g_hash_chain = fnv_u32(g_hash_chain, (uint32_t)kind);
    g_hash_chain = fnv_u64(g_hash_chain, entity_id);
    g_hash_chain = fnv_u32(g_hash_chain, seq);

    /* Saturate at UINT32_MAX */
    if (g_event_count < UINT32_MAX) { g_event_count++; }

    return seq;
}

uint32_t asx_event_log_count(void) { return g_event_count; }

int asx_event_log_get(uint32_t index, asx_event_record *out) {
    if (out == NULL) return 0;
    if (index >= g_event_count) return 0;
    if (index >= ASX_EVENT_LOG_CAPACITY) return 0;
    *out = g_event_log[index];
    return 1;
}

uint64_t asx_event_hash_chain(void) { return g_hash_chain; }

/* ------------------------------------------------------------------ */
/* Event kind string table                                             */
/* ------------------------------------------------------------------ */

const char *asx_event_kind_str(asx_event_kind kind) {
    switch (kind) {
    case ASX_EVENT_REGION_OPEN: return "region_open";
    case ASX_EVENT_REGION_CLOSE: return "region_close";
    case ASX_EVENT_TASK_SPAWN: return "task_spawn";
    case ASX_EVENT_TASK_POLL: return "task_poll";
    case ASX_EVENT_TASK_COMPLETE: return "task_complete";
    case ASX_EVENT_OBLIGATION_CREATE: return "obligation_create";
    case ASX_EVENT_OBLIGATION_COMMIT: return "obligation_commit";
    case ASX_EVENT_OBLIGATION_ABORT: return "obligation_abort";
    case ASX_EVENT_BUDGET_EXHAUSTED: return "budget_exhausted";
    case ASX_EVENT_QUIESCENT: return "quiescent";
    case ASX_EVENT_DRAIN_BEGIN: return "drain_begin";
    case ASX_EVENT_DRAIN_END: return "drain_end";
    case ASX_EVENT_KIND_COUNT: break;
    }
    return "unknown";
}

/* ------------------------------------------------------------------ */
/* JSON serialization                                                  */
/* ------------------------------------------------------------------ */

asx_status asx_event_log_to_json(asx_codec_buffer *out) {
    asx_status s;
    uint32_t i;
    uint32_t limit;

    if (out == NULL) return ASX_E_INVALID_ARGUMENT;

    s = asx_codec_buffer_append_char(out, '[');
    if (s != ASX_OK) return s;

    limit = g_event_count;
    if (limit > ASX_EVENT_LOG_CAPACITY) { limit = ASX_EVENT_LOG_CAPACITY; }

    for (i = 0; i < limit; i++) {
        /* ASX_CHECKPOINT_WAIVER("bounded aggregation") */
        const asx_event_record *e = &g_event_log[i];
        int first = 1;

        if (i > 0) {
            s = asx_codec_buffer_append_char(out, ',');
            if (s != ASX_OK) return s;
        }

        s = asx_codec_buffer_append_char(out, '{');
        if (s != ASX_OK) return s;

        s = asx_codec_buffer_append_string_field(out, &first, "kind", asx_event_kind_str(e->kind));
        if (s != ASX_OK) return s;

        s = asx_codec_buffer_append_u64_field(out, &first, "entity_id", e->entity_id);
        if (s != ASX_OK) return s;

        s = asx_codec_buffer_append_u64_field(out, &first, "parent_id", e->parent_id);
        if (s != ASX_OK) return s;

        s = asx_codec_buffer_append_u64_field(out, &first, "sequence", (uint64_t)e->sequence);
        if (s != ASX_OK) return s;

        s = asx_codec_buffer_append_u64_field(out, &first, "status", (uint64_t)(uint32_t)e->status);
        if (s != ASX_OK) return s;

        s = asx_codec_buffer_append_char(out, '}');
        if (s != ASX_OK) return s;
    }

    s = asx_codec_buffer_append_char(out, ']');
    return s;
}

/* ------------------------------------------------------------------ */
/* Replay verification                                                 */
/* ------------------------------------------------------------------ */

asx_status asx_event_replay_verify(const asx_event_record *expected, uint32_t expected_events,
                                   uint32_t expected_total, uint64_t expected_chain,
                                   asx_replay_divergence *divergence) {
    uint32_t i;
    uint32_t window;
    uint32_t compare_limit;

    if (divergence == NULL) return ASX_E_INVALID_ARGUMENT;
    memset(divergence, 0, sizeof(*divergence));
    if (expected == NULL && expected_events > 0) return ASX_E_INVALID_ARGUMENT;
    if (expected_events > expected_total) return ASX_E_INVALID_ARGUMENT;

    /* The log stores the first ASX_EVENT_LOG_CAPACITY events; the count
     * and the hash chain cover every event emitted. */
    window = g_event_count < ASX_EVENT_LOG_CAPACITY ? g_event_count : ASX_EVENT_LOG_CAPACITY;
    divergence->expected_count = expected_total;
    divergence->actual_count = g_event_count;
    divergence->expected_chain = expected_chain;
    divergence->actual_chain = g_hash_chain;
    divergence->count_mismatch = expected_total != g_event_count;
    divergence->chain_mismatch = expected_chain != g_hash_chain;

    compare_limit = expected_events < window ? expected_events : window;
    divergence->compared = compare_limit;

    for (i = 0; i < compare_limit; i++) {
        /* ASX_CHECKPOINT_WAIVER("bounded aggregation") */
        const asx_event_record *exp = &expected[i];
        const asx_event_record *act = &g_event_log[i];

        if (exp->kind != act->kind || exp->entity_id != act->entity_id ||
            exp->parent_id != act->parent_id || exp->sequence != act->sequence ||
            exp->status != act->status) {
            divergence->diverged = 1;
            divergence->first_divergence_index = i;
            divergence->expected = *exp;
            divergence->actual = *act;
            return ASX_E_EQUIVALENCE_MISMATCH;
        }
    }

    /* Every compared event matched. A different total or chain means the
     * runs differ at or after the first event not compared. */
    if (divergence->count_mismatch || divergence->chain_mismatch) {
        divergence->diverged = 1;
        divergence->first_divergence_index = compare_limit;
        return ASX_E_EQUIVALENCE_MISMATCH;
    }

    return ASX_OK;
}
