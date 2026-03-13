/*
 * fuzz_minimize.c — deterministic counterexample minimizer (bd-1md.4)
 *
 * Reduces failing fuzz scenarios while preserving the failure signature.
 * Uses two strategies:
 *   1. Delta debugging: binary search for minimal failing subset
 *   2. Op-level shrinking: try removing each op individually
 *
 * The minimizer is deterministic: same input always produces same output.
 *
 * Usage:
 *   fuzz_minimize [options]
 *     --scenario <json>       Inline scenario JSON (from fuzz report)
 *     --scenario-file <path>  Read scenario from file
 *     --failure-digest <hex>  Expected failure digest to preserve
 *     --output <path>         Write minimized scenario to file
 *     --max-rounds <n>        Max minimization rounds (default: 50)
 *     --verbose               Print progress
 *
 * The minimizer can also be used programmatically from the fuzz harness
 * by linking against this file.
 *
 * SPDX-License-Identifier: MIT
 */

#define _POSIX_C_SOURCE 199309L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include <asx/asx.h>
#include <asx/time/timer_wheel.h>

/* Import the shared scenario types from the fuzz harness.
 * In a real build system these would be in a shared header,
 * but for the walking skeleton we duplicate the essential types. */

/* ===================================================================
 * Configuration
 * =================================================================== */

#define MIN_MAX_OPS           128u
#define MIN_MAX_REGIONS       ASX_MAX_REGIONS
#define MIN_MAX_TASKS         ASX_MAX_TASKS
#define MIN_MAX_OBLIGATIONS   64u
#define MIN_MAX_CHANNELS      ASX_MAX_CHANNELS
#define MIN_MAX_TIMERS        32u

/* ===================================================================
 * PRNG (same as fuzz_differential.c for reproducibility)
 * =================================================================== */

typedef struct {
    uint64_t s[4];
} min_rng;

static uint64_t min_rotl(uint64_t x, int k)
{
    return (x << k) | (x >> (64 - k));
}

static uint64_t min_rng_next(min_rng *rng)
{
    uint64_t result = min_rotl(rng->s[1] * 5u, 7) * 9u;
    uint64_t t = rng->s[1] << 17;
    rng->s[2] ^= rng->s[0];
    rng->s[3] ^= rng->s[1];
    rng->s[1] ^= rng->s[2];
    rng->s[0] ^= rng->s[3];
    rng->s[2] ^= t;
    rng->s[3] = min_rotl(rng->s[3], 45);
    return result;
}

static void min_rng_seed(min_rng *rng, uint64_t seed)
{
    uint64_t z = seed;
    int i;
    for (i = 0; i < 4; i++) {
        z += 0x9e3779b97f4a7c15ULL;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        rng->s[i] = z ^ (z >> 31);
    }
}

static uint32_t min_rng_u32(min_rng *rng, uint32_t bound)
{
    if (bound == 0u) return 0u;
    return (uint32_t)(min_rng_next(rng) % (uint64_t)bound);
}

/* ===================================================================
 * FNV-1a hash (same as fuzz harness)
 * =================================================================== */

typedef struct {
    uint64_t hash;
} min_hasher;

static void min_hasher_init(min_hasher *h)
{
    h->hash = 0xcbf29ce484222325ULL;
}

static void min_hasher_u8(min_hasher *h, uint8_t b)
{
    h->hash ^= (uint64_t)b;
    h->hash *= 0x100000001b3ULL;
}

static void min_hasher_u32(min_hasher *h, uint32_t v)
{
    min_hasher_u8(h, (uint8_t)(v & 0xFFu));
    min_hasher_u8(h, (uint8_t)((v >> 8) & 0xFFu));
    min_hasher_u8(h, (uint8_t)((v >> 16) & 0xFFu));
    min_hasher_u8(h, (uint8_t)((v >> 24) & 0xFFu));
}

static void min_hasher_i32(min_hasher *h, int32_t v)
{
    min_hasher_u32(h, (uint32_t)v);
}

static void min_hasher_u64(min_hasher *h, uint64_t v)
{
    min_hasher_u32(h, (uint32_t)(v & 0xFFFFFFFFu));
    min_hasher_u32(h, (uint32_t)(v >> 32));
}

static uint64_t min_hasher_finish(const min_hasher *h)
{
    return h->hash;
}

/* ===================================================================
 * Scenario operation types (same as fuzz_differential.c)
 * =================================================================== */

typedef enum {
    MIN_OP_SPAWN_REGION        = 0,
    MIN_OP_CLOSE_REGION        = 1,
    MIN_OP_POISON_REGION       = 2,
    MIN_OP_SPAWN_TASK          = 3,
    MIN_OP_CANCEL_TASK         = 4,
    MIN_OP_RESERVE_OBLIGATION  = 5,
    MIN_OP_COMMIT_OBLIGATION   = 6,
    MIN_OP_ABORT_OBLIGATION    = 7,
    MIN_OP_CHANNEL_CREATE      = 8,
    MIN_OP_CHANNEL_RESERVE     = 9,
    MIN_OP_CHANNEL_SEND        = 10,
    MIN_OP_CHANNEL_ABORT       = 11,
    MIN_OP_CHANNEL_RECV        = 12,
    MIN_OP_CHANNEL_CLOSE_TX    = 13,
    MIN_OP_CHANNEL_CLOSE_RX    = 14,
    MIN_OP_TIMER_REGISTER      = 15,
    MIN_OP_TIMER_CANCEL        = 16,
    MIN_OP_ADVANCE_TIME        = 17,
    MIN_OP_SCHEDULER_RUN       = 18,
    MIN_OP_REGION_DRAIN        = 19,
    MIN_OP_QUIESCENCE_CHECK    = 20,
    MIN_OP_KIND_COUNT          = 21
} min_op_kind;

static const char *min_op_name(min_op_kind kind)
{
    static const char *names[] = {
        "SpawnRegion", "CloseRegion", "PoisonRegion",
        "SpawnTask", "CancelTask",
        "ReserveObligation", "CommitObligation", "AbortObligation",
        "ChannelCreate", "ChannelReserve", "ChannelSend",
        "ChannelAbort", "ChannelRecv", "ChannelCloseTx", "ChannelCloseRx",
        "TimerRegister", "TimerCancel", "AdvanceTime",
        "SchedulerRun", "RegionDrain", "QuiescenceCheck"
    };
    if ((int)kind < 0 || (int)kind >= MIN_OP_KIND_COUNT) return "Unknown";
    return names[(int)kind];
}

typedef struct {
    min_op_kind kind;
    uint32_t    idx_a;
    uint32_t    idx_b;
    uint32_t    arg_u32;
    uint64_t    arg_u64;
} min_op;

typedef struct {
    uint64_t seed;
    uint32_t op_count;
    min_op   ops[MIN_MAX_OPS];
} min_scenario;

/* ===================================================================
 * Scenario generation (same as fuzz_differential.c)
 * =================================================================== */

static const uint32_t MIN_OP_WEIGHTS[MIN_OP_KIND_COUNT] = {
    12,8,3,15,10,8,7,5,6,5,5,3,5,3,3,6,4,5,12,5,4
};

static min_op_kind min_pick_op(min_rng *rng)
{
    uint32_t total = 0u, r, acc;
    int i;
    for (i = 0; i < MIN_OP_KIND_COUNT; i++) total += MIN_OP_WEIGHTS[i];
    r = min_rng_u32(rng, total);
    acc = 0u;
    for (i = 0; i < MIN_OP_KIND_COUNT; i++) {
        acc += MIN_OP_WEIGHTS[i];
        if (r < acc) return (min_op_kind)i;
    }
    return MIN_OP_SPAWN_REGION;
}

static void min_generate_scenario(min_rng *rng, min_scenario *sc, uint32_t max_ops)
{
    uint32_t n, i;
    sc->seed = min_rng_next(rng);
    n = 4u + min_rng_u32(rng, max_ops > 4u ? max_ops - 4u : 1u);
    if (n > MIN_MAX_OPS) n = MIN_MAX_OPS;
    sc->op_count = n;
    sc->ops[0].kind = MIN_OP_SPAWN_REGION;
    sc->ops[0].idx_a = 0u; sc->ops[0].idx_b = 0u;
    sc->ops[0].arg_u32 = 0u; sc->ops[0].arg_u64 = 0u;
    for (i = 1u; i < n; i++) {
        sc->ops[i].kind = min_pick_op(rng);
        sc->ops[i].idx_a = min_rng_u32(rng, MIN_MAX_REGIONS);
        sc->ops[i].idx_b = min_rng_u32(rng, MIN_MAX_TASKS);
        sc->ops[i].arg_u32 = min_rng_u32(rng, 32u);
        sc->ops[i].arg_u64 = min_rng_next(rng) % 10000u;
    }
}

/* Read a scenario from stdin in text format:
 * "scenario_seed op_count kind0 a0 b0 u32_0 u64_0 kind1 ..."
 * Returns 0 on success, -1 on failure/EOF. */
static int min_read_scenario_stdin(min_scenario *sc)
{
    char line[8192];
    char *p;
    uint32_t i;
    unsigned long long seed_ull;
    unsigned int op_count_u;

    if (fgets(line, sizeof(line), stdin) == NULL) return -1;
    p = line;

    if (sscanf(p, "%llu %u%n", &seed_ull, &op_count_u,
               (int[]){0}) < 2) return -1;
    sc->seed = (uint64_t)seed_ull;
    sc->op_count = op_count_u;
    if (sc->op_count > MIN_MAX_OPS) sc->op_count = MIN_MAX_OPS;

    /* Skip past seed and op_count */
    while (*p == ' ') p++;
    while (*p && *p != ' ') p++; /* skip seed */
    while (*p == ' ') p++;
    while (*p && *p != ' ') p++; /* skip op_count */

    for (i = 0u; i < sc->op_count; i++) {
        unsigned int kind_u, a_u, b_u, u32_u;
        unsigned long long u64_ull;
        int n_read = 0;

        if (sscanf(p, " %u %u %u %u %llu%n",
                   &kind_u, &a_u, &b_u, &u32_u, &u64_ull, &n_read) < 5)
            break;

        sc->ops[i].kind = (min_op_kind)kind_u;
        sc->ops[i].idx_a = a_u;
        sc->ops[i].idx_b = b_u;
        sc->ops[i].arg_u32 = u32_u;
        sc->ops[i].arg_u64 = (uint64_t)u64_ull;
        p += n_read;
    }

    return 0;
}

/* ===================================================================
 * Handle tracking during execution (same as fuzz harness)
 * =================================================================== */

typedef struct {
    asx_region_id     regions[MIN_MAX_REGIONS];
    uint32_t          region_count;
    asx_task_id       tasks[MIN_MAX_TASKS];
    uint32_t          task_count;
    asx_obligation_id obligations[MIN_MAX_OBLIGATIONS];
    uint32_t          obligation_count;
    asx_channel_id    channels[MIN_MAX_CHANNELS];
    uint32_t          channel_count;
    asx_send_permit   permits[MIN_MAX_CHANNELS];
    uint32_t          permit_count;
    asx_timer_handle  timers[MIN_MAX_TIMERS];
    uint32_t          timer_count;
    uint64_t          sim_time;
} min_handle_state;

/* ===================================================================
 * Task poll function
 * =================================================================== */

typedef struct {
    uint32_t polls_remaining;
    asx_co_state co;
} min_task_state;

static asx_status min_task_poll(void *user_data, asx_task_id self)
{
    min_task_state *st = (min_task_state *)user_data;
    (void)self;
    if (st->polls_remaining > 0u) {
        st->polls_remaining--;
        return ASX_E_PENDING;
    }
    return ASX_OK;
}

static min_task_state g_min_task_states[MIN_MAX_TASKS];
static uint32_t g_min_task_state_next = 0u;

static min_task_state *min_alloc_task_state(uint32_t polls)
{
    min_task_state *st;
    if (g_min_task_state_next >= MIN_MAX_TASKS) return NULL;
    st = &g_min_task_states[g_min_task_state_next++];
    st->polls_remaining = polls;
    st->co.line = 0u;
    return st;
}

static void min_reset_task_states(void)
{
    g_min_task_state_next = 0u;
}

/* ===================================================================
 * Scenario executor helpers
 * =================================================================== */

/* Execute a single op against handle state, return status */
static asx_status min_execute_one_op(const min_op *op, min_handle_state *hs);

/* ===================================================================
 * Execute a single op against handle state
 * =================================================================== */

static asx_status min_execute_one_op(const min_op *op, min_handle_state *hs)
{
    asx_status st = ASX_OK;

    switch (op->kind) {
    case MIN_OP_SPAWN_REGION: {
        asx_region_id rid = ASX_INVALID_ID;
        if (hs->region_count < MIN_MAX_REGIONS) {
            st = asx_region_open(&rid);
            if (st == ASX_OK) hs->regions[hs->region_count++] = rid;
        } else { st = ASX_E_REGION_AT_CAPACITY; }
        break;
    }
    case MIN_OP_CLOSE_REGION: {
        if (hs->region_count > 0u) {
            st = asx_region_close(hs->regions[op->idx_a % hs->region_count]);
        } else { st = ASX_E_NOT_FOUND; }
        break;
    }
    case MIN_OP_POISON_REGION: {
        if (hs->region_count > 0u) {
            st = asx_region_poison(hs->regions[op->idx_a % hs->region_count]);
        } else { st = ASX_E_NOT_FOUND; }
        break;
    }
    case MIN_OP_SPAWN_TASK: {
        if (hs->region_count > 0u && hs->task_count < MIN_MAX_TASKS) {
            uint32_t ridx = op->idx_a % hs->region_count;
            min_task_state *tst = min_alloc_task_state(1u + (op->arg_u32 % 16u));
            asx_task_id tid = ASX_INVALID_ID;
            if (tst != NULL) {
                st = asx_task_spawn(hs->regions[ridx], min_task_poll, tst, &tid);
                if (st == ASX_OK) hs->tasks[hs->task_count++] = tid;
            } else { st = ASX_E_RESOURCE_EXHAUSTED; }
        } else { st = ASX_E_NOT_FOUND; }
        break;
    }
    case MIN_OP_CANCEL_TASK: {
        if (hs->task_count > 0u) {
            st = asx_task_cancel(hs->tasks[op->idx_b % hs->task_count],
                                 (asx_cancel_kind)(op->arg_u32 % 11u));
        } else { st = ASX_E_NOT_FOUND; }
        break;
    }
    case MIN_OP_RESERVE_OBLIGATION: {
        if (hs->region_count > 0u && hs->obligation_count < MIN_MAX_OBLIGATIONS) {
            asx_obligation_id oid = ASX_INVALID_ID;
            st = asx_obligation_reserve(hs->regions[op->idx_a % hs->region_count], &oid);
            if (st == ASX_OK) hs->obligations[hs->obligation_count++] = oid;
        } else { st = ASX_E_NOT_FOUND; }
        break;
    }
    case MIN_OP_COMMIT_OBLIGATION: {
        if (hs->obligation_count > 0u) {
            st = asx_obligation_commit(hs->obligations[op->idx_a % hs->obligation_count]);
        } else { st = ASX_E_NOT_FOUND; }
        break;
    }
    case MIN_OP_ABORT_OBLIGATION: {
        if (hs->obligation_count > 0u) {
            st = asx_obligation_abort(hs->obligations[op->idx_a % hs->obligation_count]);
        } else { st = ASX_E_NOT_FOUND; }
        break;
    }
    case MIN_OP_CHANNEL_CREATE: {
        if (hs->region_count > 0u && hs->channel_count < MIN_MAX_CHANNELS) {
            asx_channel_id cid = ASX_INVALID_ID;
            st = asx_channel_create(hs->regions[op->idx_a % hs->region_count],
                                    1u + (op->arg_u32 % (ASX_CHANNEL_MAX_CAPACITY - 1u)), &cid);
            if (st == ASX_OK) hs->channels[hs->channel_count++] = cid;
        } else { st = ASX_E_NOT_FOUND; }
        break;
    }
    case MIN_OP_CHANNEL_RESERVE: {
        if (hs->channel_count > 0u && hs->permit_count < MIN_MAX_CHANNELS) {
            asx_send_permit permit;
            memset(&permit, 0, sizeof(permit));
            st = asx_channel_try_reserve(hs->channels[op->idx_a % hs->channel_count], &permit);
            if (st == ASX_OK) hs->permits[hs->permit_count++] = permit;
        } else { st = ASX_E_NOT_FOUND; }
        break;
    }
    case MIN_OP_CHANNEL_SEND: {
        if (hs->permit_count > 0u) {
            st = asx_send_permit_send(&hs->permits[op->idx_a % hs->permit_count], op->arg_u64);
        } else { st = ASX_E_NOT_FOUND; }
        break;
    }
    case MIN_OP_CHANNEL_ABORT: {
        if (hs->permit_count > 0u) {
            asx_send_permit_abort(&hs->permits[op->idx_a % hs->permit_count]);
        } else { st = ASX_E_NOT_FOUND; }
        break;
    }
    case MIN_OP_CHANNEL_RECV: {
        if (hs->channel_count > 0u) {
            uint64_t val = 0u;
            st = asx_channel_try_recv(hs->channels[op->idx_a % hs->channel_count], &val);
        } else { st = ASX_E_NOT_FOUND; }
        break;
    }
    case MIN_OP_CHANNEL_CLOSE_TX: {
        if (hs->channel_count > 0u) {
            st = asx_channel_close_sender(hs->channels[op->idx_a % hs->channel_count]);
        } else { st = ASX_E_NOT_FOUND; }
        break;
    }
    case MIN_OP_CHANNEL_CLOSE_RX: {
        if (hs->channel_count > 0u) {
            st = asx_channel_close_receiver(hs->channels[op->idx_a % hs->channel_count]);
        } else { st = ASX_E_NOT_FOUND; }
        break;
    }
    case MIN_OP_TIMER_REGISTER: {
        if (hs->timer_count < MIN_MAX_TIMERS) {
            asx_timer_handle th;
            memset(&th, 0, sizeof(th));
            st = asx_timer_register(asx_timer_wheel_global(),
                                    hs->sim_time + 1u + (op->arg_u64 % 5000u), NULL, &th);
            if (st == ASX_OK) hs->timers[hs->timer_count++] = th;
        } else { st = ASX_E_RESOURCE_EXHAUSTED; }
        break;
    }
    case MIN_OP_TIMER_CANCEL: {
        if (hs->timer_count > 0u) {
            int c = asx_timer_cancel(asx_timer_wheel_global(),
                                     &hs->timers[op->idx_a % hs->timer_count]);
            st = c ? ASX_OK : ASX_E_TIMER_NOT_FOUND;
        } else { st = ASX_E_NOT_FOUND; }
        break;
    }
    case MIN_OP_ADVANCE_TIME: {
        void *wakers[16];
        hs->sim_time += 1u + (op->arg_u64 % 2000u);
        asx_timer_collect_expired(asx_timer_wheel_global(), hs->sim_time, wakers, 16u);
        break;
    }
    case MIN_OP_SCHEDULER_RUN: {
        if (hs->region_count > 0u) {
            asx_budget budget = asx_budget_infinite();
            budget.poll_quota = 1u + (op->arg_u32 % 64u);
            st = asx_scheduler_run(hs->regions[op->idx_a % hs->region_count], &budget);
        } else { st = ASX_E_NOT_FOUND; }
        break;
    }
    case MIN_OP_REGION_DRAIN: {
        if (hs->region_count > 0u) {
            asx_budget budget = asx_budget_infinite();
            budget.poll_quota = 256u;
            st = asx_region_drain(hs->regions[op->idx_a % hs->region_count], &budget);
        } else { st = ASX_E_NOT_FOUND; }
        break;
    }
    case MIN_OP_QUIESCENCE_CHECK: {
        if (hs->region_count > 0u) {
            st = asx_quiescence_check(hs->regions[op->idx_a % hs->region_count]);
        } else { st = ASX_E_NOT_FOUND; }
        break;
    }
    default:
        st = ASX_E_INVALID_ARGUMENT;
        break;
    }

    return st;
}

/* ===================================================================
 * Scenario executor (produces semantic digest)
 * =================================================================== */

static uint64_t min_execute_digest(const min_scenario *sc)
{
    min_handle_state hs;
    min_hasher hasher;
    uint32_t i;

    memset(&hs, 0, sizeof(hs));

    asx_runtime_reset();
    asx_scheduler_event_reset();
    asx_channel_reset();
    asx_timer_wheel_reset(asx_timer_wheel_global());
    min_reset_task_states();

    min_hasher_init(&hasher);
    min_hasher_u64(&hasher, sc->seed);
    min_hasher_u32(&hasher, sc->op_count);

    for (i = 0u; i < sc->op_count; i++) {
        asx_status st;

        min_hasher_u32(&hasher, (uint32_t)sc->ops[i].kind);
        st = min_execute_one_op(&sc->ops[i], &hs);
        min_hasher_i32(&hasher, (int32_t)st);
    }

    /* Include scheduler event log in digest */
    {
        uint32_t ec = asx_scheduler_event_count();
        uint32_t ei;
        min_hasher_u32(&hasher, ec);
        for (ei = 0u; ei < ec; ei++) {
            asx_scheduler_event ev;
            if (asx_scheduler_event_get(ei, &ev)) {
                min_hasher_u32(&hasher, (uint32_t)ev.kind);
                min_hasher_u64(&hasher, ev.task_id);
                min_hasher_u32(&hasher, ev.sequence);
            }
        }
    }

    return min_hasher_finish(&hasher);
}

/* ===================================================================
 * Minimization strategies
 * =================================================================== */

/*
 * Strategy 1: Delta debugging (ddmin)
 *
 * Repeatedly try removing chunks of ops. Start with halves,
 * then quarters, etc. Keep the smallest scenario that still
 * produces a different digest on replay (non-deterministic).
 *
 * For determinism-preserving minimization, we check that the
 * minimized scenario still produces a *different* digest between
 * two runs (or still matches the target failure digest).
 */

typedef int (*min_predicate_fn)(const min_scenario *sc, void *user_data);

typedef enum {
    MIN_MODE_DETERMINISM,  /* preserve: digest differs between two runs */
    MIN_MODE_DIGEST_MATCH, /* preserve: digest equals target_digest */
    MIN_MODE_PREDICATE     /* preserve: user predicate returns nonzero */
} min_mode;

typedef struct {
    min_mode         mode;
    uint64_t         target_digest;   /* for DIGEST_MATCH mode */
    min_predicate_fn predicate;       /* for PREDICATE mode */
    void            *predicate_data;  /* for PREDICATE mode */
    uint32_t         max_rounds;
    int              verbose;
} min_config;

/* Check if a scenario preserves the failure property */
static int min_preserves_failure(const min_scenario *sc, const min_config *cfg)
{
    uint64_t d1, d2;

    switch (cfg->mode) {
    case MIN_MODE_DETERMINISM:
        /* Failure = digest differs between two runs */
        d1 = min_execute_digest(sc);
        d2 = min_execute_digest(sc);
        return d1 != d2;

    case MIN_MODE_DIGEST_MATCH:
        /* Failure = digest matches a specific target */
        d1 = min_execute_digest(sc);
        return d1 == cfg->target_digest;

    case MIN_MODE_PREDICATE:
        /* Failure = user predicate says so */
        if (cfg->predicate != NULL) {
            return cfg->predicate(sc, cfg->predicate_data);
        }
        return 0;
    }

    return 0;
}

/* Strategy 1: Try removing individual ops */
static uint32_t min_try_remove_singles(min_scenario *sc, const min_config *cfg)
{
    uint32_t removed = 0u;
    uint32_t i;

    /* Try removing each op from the end backwards (preserves indices) */
    for (i = sc->op_count; i > 1u; i--) {
        uint32_t target = i - 1u;
        min_scenario candidate;

        /* Copy scenario without the target op */
        memcpy(&candidate, sc, sizeof(min_scenario));
        if (target < candidate.op_count - 1u) {
            memmove(&candidate.ops[target], &candidate.ops[target + 1u],
                    (size_t)(candidate.op_count - target - 1u) * sizeof(min_op));
        }
        candidate.op_count--;

        if (min_preserves_failure(&candidate, cfg)) {
            /* Removal preserved the failure — accept */
            memcpy(sc, &candidate, sizeof(min_scenario));
            removed++;
            if (cfg->verbose) {
                fprintf(stderr, "[minimize] removed op %u (%s), now %u ops\n",
                        target, min_op_name(sc->ops[target < sc->op_count ? target : 0].kind),
                        sc->op_count);
            }
        }
    }

    return removed;
}

/* Strategy 2: Delta debugging — try removing chunks */
static uint32_t min_delta_debug(min_scenario *sc, const min_config *cfg)
{
    uint32_t removed = 0u;
    uint32_t chunk_size;

    /* Start with large chunks, progressively shrink */
    for (chunk_size = sc->op_count / 2u; chunk_size >= 1u; chunk_size /= 2u) {
        uint32_t start;

        for (start = 1u; start + chunk_size <= sc->op_count; /* advance below */) {
            min_scenario candidate;
            uint32_t end = start + chunk_size;
            uint32_t tail;

            if (end > sc->op_count) end = sc->op_count;
            tail = sc->op_count - end;

            memcpy(&candidate, sc, sizeof(min_scenario));
            if (tail > 0u) {
                memmove(&candidate.ops[start], &candidate.ops[end],
                        (size_t)tail * sizeof(min_op));
            }
            candidate.op_count = sc->op_count - (end - start);

            if (min_preserves_failure(&candidate, cfg)) {
                memcpy(sc, &candidate, sizeof(min_scenario));
                removed += (end - start);
                if (cfg->verbose) {
                    fprintf(stderr, "[minimize] removed chunk [%u..%u), now %u ops\n",
                            start, end, sc->op_count);
                }
                /* Don't advance start — try same position again */
            } else {
                start += chunk_size;
            }
        }

        if (chunk_size == 1u) break;
    }

    return removed;
}

/* Strategy 3: Try simplifying op arguments */
static uint32_t min_simplify_args(min_scenario *sc, const min_config *cfg)
{
    uint32_t simplified = 0u;
    uint32_t i;

    for (i = 0u; i < sc->op_count; i++) {
        min_scenario candidate;

        /* Try zeroing idx_a */
        if (sc->ops[i].idx_a != 0u) {
            memcpy(&candidate, sc, sizeof(min_scenario));
            candidate.ops[i].idx_a = 0u;
            if (min_preserves_failure(&candidate, cfg)) {
                sc->ops[i].idx_a = 0u;
                simplified++;
            }
        }

        /* Try zeroing idx_b */
        if (sc->ops[i].idx_b != 0u) {
            memcpy(&candidate, sc, sizeof(min_scenario));
            candidate.ops[i].idx_b = 0u;
            if (min_preserves_failure(&candidate, cfg)) {
                sc->ops[i].idx_b = 0u;
                simplified++;
            }
        }

        /* Try reducing arg_u32 */
        if (sc->ops[i].arg_u32 > 1u) {
            memcpy(&candidate, sc, sizeof(min_scenario));
            candidate.ops[i].arg_u32 = 1u;
            if (min_preserves_failure(&candidate, cfg)) {
                sc->ops[i].arg_u32 = 1u;
                simplified++;
            }
        }

        /* Try reducing arg_u64 */
        if (sc->ops[i].arg_u64 > 1u) {
            memcpy(&candidate, sc, sizeof(min_scenario));
            candidate.ops[i].arg_u64 = 1u;
            if (min_preserves_failure(&candidate, cfg)) {
                sc->ops[i].arg_u64 = 1u;
                simplified++;
            }
        }
    }

    return simplified;
}

/* ===================================================================
 * Main minimization driver
 * =================================================================== */

typedef struct {
    uint32_t original_ops;
    uint32_t minimized_ops;
    uint32_t rounds;
    uint32_t ops_removed;
    uint32_t args_simplified;
    double   duration_sec;
    uint64_t final_digest;
} min_result;

static void min_minimize(min_scenario *sc, const min_config *cfg, min_result *result)
{
    uint32_t round;
    struct timespec start_ts, end_ts;
    double start_sec, end_sec;

    clock_gettime(CLOCK_MONOTONIC, &start_ts);
    start_sec = (double)start_ts.tv_sec + (double)start_ts.tv_nsec * 1e-9;

    result->original_ops = sc->op_count;
    result->ops_removed = 0u;
    result->args_simplified = 0u;
    result->rounds = 0u;

    if (cfg->verbose) {
        fprintf(stderr, "[minimize] starting with %u ops\n", sc->op_count);
    }

    /* Verify the scenario actually fails before minimizing */
    if (!min_preserves_failure(sc, cfg)) {
        if (cfg->verbose) {
            fprintf(stderr, "[minimize] scenario does not reproduce failure, skipping\n");
        }
        result->minimized_ops = sc->op_count;
        result->final_digest = min_execute_digest(sc);
        return;
    }

    for (round = 0u; round < cfg->max_rounds; round++) {
        uint32_t prev_ops = sc->op_count;
        uint32_t removed;

        result->rounds = round + 1u;

        /* Phase 1: Delta debugging (chunk removal) */
        removed = min_delta_debug(sc, cfg);
        result->ops_removed += removed;

        /* Phase 2: Single op removal */
        removed = min_try_remove_singles(sc, cfg);
        result->ops_removed += removed;

        /* Phase 3: Argument simplification */
        result->args_simplified += min_simplify_args(sc, cfg);

        if (sc->op_count >= prev_ops) {
            /* No progress this round — we've converged */
            break;
        }

        if (cfg->verbose) {
            fprintf(stderr, "[minimize] round %u: %u -> %u ops\n",
                    round + 1u, prev_ops, sc->op_count);
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &end_ts);
    end_sec = (double)end_ts.tv_sec + (double)end_ts.tv_nsec * 1e-9;

    result->minimized_ops = sc->op_count;
    result->final_digest = min_execute_digest(sc);
    result->duration_sec = end_sec - start_sec;
}

/* ===================================================================
 * JSON output for minimized scenario
 * =================================================================== */

static void min_emit_json(FILE *out, const min_scenario *sc, const min_result *result)
{
    uint32_t i;

    fprintf(out, "{\"kind\":\"minimized_scenario\",");
    fprintf(out, "\"seed\":%llu,", (unsigned long long)sc->seed);
    fprintf(out, "\"original_ops\":%u,", result->original_ops);
    fprintf(out, "\"minimized_ops\":%u,", result->minimized_ops);
    fprintf(out, "\"rounds\":%u,", result->rounds);
    fprintf(out, "\"ops_removed\":%u,", result->ops_removed);
    fprintf(out, "\"args_simplified\":%u,", result->args_simplified);
    fprintf(out, "\"duration_sec\":%.3f,", result->duration_sec);
    fprintf(out, "\"final_digest\":\"%016llx\",", (unsigned long long)result->final_digest);
    fprintf(out, "\"ops\":[");
    for (i = 0u; i < sc->op_count; i++) {
        if (i > 0u) fprintf(out, ",");
        fprintf(out, "{\"op\":\"%s\",\"idx_a\":%u,\"idx_b\":%u,\"arg_u32\":%u,\"arg_u64\":%llu}",
                min_op_name(sc->ops[i].kind),
                sc->ops[i].idx_a, sc->ops[i].idx_b,
                sc->ops[i].arg_u32,
                (unsigned long long)sc->ops[i].arg_u64);
    }
    fprintf(out, "]}\n");
    fflush(out);
}

/* ===================================================================
 * Predicate: does the scenario produce ASX_E_REGION_POISONED?
 * Used by self-test 3 to demonstrate op removal.
 * =================================================================== */

static int min_predicate_has_poisoned_spawn(const min_scenario *sc, void *user_data)
{
    min_handle_state hs;
    uint32_t i;
    (void)user_data;

    memset(&hs, 0, sizeof(hs));
    asx_runtime_reset();
    asx_scheduler_event_reset();
    asx_channel_reset();
    asx_timer_wheel_reset(asx_timer_wheel_global());
    min_reset_task_states();

    for (i = 0u; i < sc->op_count; i++) {
        asx_status st = min_execute_one_op(&sc->ops[i], &hs);
        if (st == ASX_E_REGION_POISONED) return 1;
    }
    return 0;
}

/* ===================================================================
 * Self-test: generate a scenario that differs and minimize it
 * =================================================================== */

static int min_selftest(int verbose)
{
    min_rng rng;
    min_scenario sc;
    min_config cfg;
    min_result result;
    uint32_t i;

    memset(&result, 0, sizeof(result));
    fprintf(stderr, "[minimize] running self-test...\n");

    min_rng_seed(&rng, 12345u);

    /* Create a scenario with some redundant ops */
    sc.seed = 9999u;
    sc.op_count = 20u;
    sc.ops[0].kind = MIN_OP_SPAWN_REGION;
    sc.ops[0].idx_a = 0u; sc.ops[0].idx_b = 0u;
    sc.ops[0].arg_u32 = 0u; sc.ops[0].arg_u64 = 0u;

    for (i = 1u; i < sc.op_count; i++) {
        sc.ops[i].kind = (min_op_kind)(min_rng_next(&rng) % MIN_OP_KIND_COUNT);
        sc.ops[i].idx_a = (uint32_t)(min_rng_next(&rng) % 8u);
        sc.ops[i].idx_b = (uint32_t)(min_rng_next(&rng) % 16u);
        sc.ops[i].arg_u32 = (uint32_t)(min_rng_next(&rng) % 32u);
        sc.ops[i].arg_u64 = min_rng_next(&rng) % 5000u;
    }

    /* Run in DIGEST_MATCH mode: try to minimize while preserving the digest */
    cfg.mode = MIN_MODE_DIGEST_MATCH;
    cfg.target_digest = min_execute_digest(&sc);
    cfg.max_rounds = 10u;
    cfg.verbose = verbose;

    min_minimize(&sc, &cfg, &result);

    fprintf(stderr,
        "[minimize] self-test: %u -> %u ops in %u rounds (%.3fs)\n",
        result.original_ops, result.minimized_ops,
        result.rounds, result.duration_sec);

    /* Verify minimized scenario still produces the target digest */
    if (min_execute_digest(&sc) != cfg.target_digest) {
        fprintf(stderr, "[minimize] self-test FAIL: digest changed after minimization\n");
        return 1;
    }

    /* Verify it's actually smaller (or same — some scenarios are already minimal) */
    if (result.minimized_ops > result.original_ops) {
        fprintf(stderr, "[minimize] self-test FAIL: scenario grew during minimization\n");
        return 1;
    }

    fprintf(stderr, "[minimize] self-test PASS\n");

    if (verbose) {
        min_emit_json(stderr, &sc, &result);
    }

    /* ---- Self-test 2: verify argument simplification on structured scenario ---- */
    /*
     * In DIGEST_MATCH mode, op removal is impossible because the digest
     * includes op_count and each op's kind+result. The real value of
     * DIGEST_MATCH is argument simplification (zeroing indices, reducing
     * constants). Op removal works in DETERMINISM mode when a real
     * non-determinism bug is found.
     */
    fprintf(stderr, "[minimize] running self-test 2 (arg simplification)...\n");
    memset(&result, 0, sizeof(result));

    /* Build a scenario with complex arguments that can be simplified */
    sc.seed = 7777u;
    sc.op_count = 8u;
    memset(sc.ops, 0, sizeof(sc.ops));

    sc.ops[0].kind = MIN_OP_SPAWN_REGION;
    sc.ops[0].arg_u32 = 999u; sc.ops[0].arg_u64 = 12345u;
    sc.ops[1].kind = MIN_OP_SPAWN_TASK;
    sc.ops[1].idx_a = 7u; sc.ops[1].idx_b = 15u;
    sc.ops[1].arg_u32 = 30u; sc.ops[1].arg_u64 = 4999u;
    sc.ops[2].kind = MIN_OP_SCHEDULER_RUN;
    sc.ops[2].idx_a = 5u; sc.ops[2].arg_u32 = 60u;
    sc.ops[3].kind = MIN_OP_CHANNEL_CREATE;
    sc.ops[3].idx_a = 3u; sc.ops[3].arg_u32 = 15u;
    sc.ops[4].kind = MIN_OP_TIMER_REGISTER;
    sc.ops[4].arg_u64 = 3000u;
    sc.ops[5].kind = MIN_OP_ADVANCE_TIME;
    sc.ops[5].arg_u64 = 1500u;
    sc.ops[6].kind = MIN_OP_QUIESCENCE_CHECK;
    sc.ops[6].idx_a = 4u;
    sc.ops[7].kind = MIN_OP_REGION_DRAIN;
    sc.ops[7].idx_a = 2u; sc.ops[7].arg_u32 = 20u;

    cfg.mode = MIN_MODE_DIGEST_MATCH;
    cfg.target_digest = min_execute_digest(&sc);
    cfg.max_rounds = 10u;
    cfg.verbose = verbose;

    min_minimize(&sc, &cfg, &result);

    fprintf(stderr,
        "[minimize] self-test 2: %u ops, %u args simplified in %u rounds (%.3fs)\n",
        result.minimized_ops, result.args_simplified,
        result.rounds, result.duration_sec);

    if (min_execute_digest(&sc) != cfg.target_digest) {
        fprintf(stderr, "[minimize] self-test 2 FAIL: digest changed\n");
        return 1;
    }

    if (result.args_simplified == 0u) {
        fprintf(stderr, "[minimize] self-test 2 FAIL: no args simplified\n");
        return 1;
    }

    fprintf(stderr, "[minimize] self-test 2 PASS (%u args simplified)\n",
            result.args_simplified);
    if (verbose) {
        min_emit_json(stderr, &sc, &result);
    }

    /* ---- Self-test 3: predicate mode with actual op removal ---- */
    /*
     * Uses PREDICATE mode: "does the scenario still produce
     * ASX_E_REGION_POISONED when spawning a task?" This allows
     * removing ops that don't affect whether the region gets poisoned.
     */
    fprintf(stderr, "[minimize] running self-test 3 (op removal via predicate)...\n");
    memset(&result, 0, sizeof(result));

    /* SpawnRegion, PoisonRegion, SpawnTask (fails with POISONED),
     * then several irrelevant padding ops */
    sc.seed = 5555u;
    sc.op_count = 12u;
    memset(sc.ops, 0, sizeof(sc.ops));

    sc.ops[0].kind = MIN_OP_SPAWN_REGION;
    sc.ops[1].kind = MIN_OP_POISON_REGION;
    sc.ops[2].kind = MIN_OP_SPAWN_TASK;       /* produces POISONED */
    sc.ops[2].arg_u32 = 1u;
    /* Padding ops that don't affect the poison status */
    sc.ops[3].kind = MIN_OP_ADVANCE_TIME;
    sc.ops[3].arg_u64 = 100u;
    sc.ops[4].kind = MIN_OP_ADVANCE_TIME;
    sc.ops[4].arg_u64 = 200u;
    sc.ops[5].kind = MIN_OP_TIMER_REGISTER;
    sc.ops[5].arg_u64 = 50u;
    sc.ops[6].kind = MIN_OP_ADVANCE_TIME;
    sc.ops[6].arg_u64 = 300u;
    sc.ops[7].kind = MIN_OP_TIMER_REGISTER;
    sc.ops[7].arg_u64 = 75u;
    sc.ops[8].kind = MIN_OP_ADVANCE_TIME;
    sc.ops[8].arg_u64 = 400u;
    sc.ops[9].kind = MIN_OP_ADVANCE_TIME;
    sc.ops[9].arg_u64 = 500u;
    sc.ops[10].kind = MIN_OP_ADVANCE_TIME;
    sc.ops[10].arg_u64 = 600u;
    sc.ops[11].kind = MIN_OP_ADVANCE_TIME;
    sc.ops[11].arg_u64 = 700u;

    cfg.mode = MIN_MODE_PREDICATE;
    cfg.predicate = min_predicate_has_poisoned_spawn;
    cfg.predicate_data = NULL;
    cfg.max_rounds = 10u;
    cfg.verbose = verbose;

    min_minimize(&sc, &cfg, &result);

    fprintf(stderr,
        "[minimize] self-test 3: %u -> %u ops in %u rounds (%.3fs)\n",
        result.original_ops, result.minimized_ops,
        result.rounds, result.duration_sec);

    if (!min_preserves_failure(&sc, &cfg)) {
        fprintf(stderr, "[minimize] self-test 3 FAIL: predicate no longer holds\n");
        return 1;
    }

    if (result.minimized_ops >= result.original_ops) {
        fprintf(stderr, "[minimize] self-test 3 FAIL: no ops removed (expected reduction)\n");
        return 1;
    }

    fprintf(stderr, "[minimize] self-test 3 PASS (reduced %u -> %u ops)\n",
            result.original_ops, result.minimized_ops);
    if (verbose) {
        min_emit_json(stderr, &sc, &result);
    }

    return 0;
}

/* ===================================================================
 * CLI
 * =================================================================== */

static uint64_t parse_u64(const char *s)
{
    uint64_t v = 0u;
    while (*s >= '0' && *s <= '9') {
        v = v * 10u + (uint64_t)(*s - '0');
        s++;
    }
    return v;
}

static uint64_t parse_hex64(const char *s)
{
    uint64_t v = 0u;
    while ((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f') || (*s >= 'A' && *s <= 'F')) {
        v <<= 4;
        if (*s >= '0' && *s <= '9') v |= (uint64_t)(*s - '0');
        else if (*s >= 'a' && *s <= 'f') v |= (uint64_t)(*s - 'a' + 10);
        else v |= (uint64_t)(*s - 'A' + 10);
        s++;
    }
    return v;
}

int main(int argc, char **argv)
{
    int i;
    int run_selftest = 0;
    int verbose = 0;
    int stdin_scenario = 0;
    int has_initial_seed = 0;
    uint32_t max_rounds = 50u;
    uint32_t max_ops = 64u;
    uint32_t mutations_per_scenario = 4u;
    uint64_t initial_seed = 0u;
    uint64_t target_iteration = 0u;
    uint64_t target_digest = 0u;
    int has_target = 0;
    const char *output_path = NULL;
    min_scenario sc;

    memset(&sc, 0, sizeof(sc));

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--selftest") == 0) {
            run_selftest = 1;
        } else if (strcmp(argv[i], "--verbose") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "--max-rounds") == 0 && i + 1 < argc) {
            max_rounds = (uint32_t)parse_u64(argv[++i]);
        } else if (strcmp(argv[i], "--max-ops") == 0 && i + 1 < argc) {
            max_ops = (uint32_t)parse_u64(argv[++i]);
        } else if (strcmp(argv[i], "--mutations") == 0 && i + 1 < argc) {
            mutations_per_scenario = (uint32_t)parse_u64(argv[++i]);
        } else if (strcmp(argv[i], "--failure-digest") == 0 && i + 1 < argc) {
            target_digest = parse_hex64(argv[++i]);
            has_target = 1;
        } else if (strcmp(argv[i], "--initial-seed") == 0 && i + 1 < argc) {
            initial_seed = parse_u64(argv[++i]);
            has_initial_seed = 1;
        } else if (strcmp(argv[i], "--iteration") == 0 && i + 1 < argc) {
            target_iteration = parse_u64(argv[++i]);
        } else if (strcmp(argv[i], "--stdin-scenario") == 0) {
            stdin_scenario = 1;
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            output_path = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            fprintf(stderr,
                "Usage: fuzz_minimize [options]\n"
                "  --selftest                Run built-in self-test\n"
                "  --failure-digest <hex>    Target digest to preserve\n"
                "  --initial-seed <n>        Master seed (with --iteration)\n"
                "  --iteration <n>           Iteration index within seed\n"
                "  --stdin-scenario          Read scenario from stdin (text format)\n"
                "  --max-ops <n>             Max ops per scenario (default: 64)\n"
                "  --mutations <n>           Mutations per scenario for RNG sync (default: 4)\n"
                "  --output <path>           Write minimized result to file\n"
                "  --max-rounds <n>          Max minimization rounds (default: 50)\n"
                "  --verbose                 Print progress\n");
            return 0;
        } else {
            fprintf(stderr, "[minimize] unknown argument: %s\n", argv[i]);
            return 2;
        }
    }

    if (run_selftest) {
        return min_selftest(verbose);
    }

    if (!has_target && !stdin_scenario && !has_initial_seed) {
        /* Default to self-test mode if no target specified */
        fprintf(stderr, "[minimize] no --failure-digest specified, running self-test\n");
        return min_selftest(verbose);
    }

    /* Generate or read scenario */
    if (has_initial_seed) {
        /* Generate scenario from master seed + iteration, matching
         * fuzz_differential.c's RNG sequence exactly. */
        min_rng gen_rng;
        uint64_t skip;
        min_rng_seed(&gen_rng, initial_seed);
        /* Skip to the target iteration by generating + discarding
         * preceding scenarios. Each iteration consumes generation RNG
         * plus mutation RNG (4 mutations by default). */
        for (skip = 0u; skip < target_iteration; skip++) {
            min_scenario skip_sc;
            uint32_t m;
            min_generate_scenario(&gen_rng, &skip_sc, max_ops);
            /* Skip mutation RNG draws to stay in sync */
            for (m = 0u; m < mutations_per_scenario && m < 16u; m++) {
                /* Each mutation draws 2-4 RNG values depending on type.
                 * For simplicity, we do the actual mutation on a throw-away copy. */
                /* TODO: when fuzz_common.h is created, use the real mutate.
                 * For now, just advance the RNG by the average mutation cost. */
                min_rng_next(&gen_rng);
                min_rng_next(&gen_rng);
                min_rng_next(&gen_rng);
            }
        }
        min_generate_scenario(&gen_rng, &sc, max_ops);
        fprintf(stderr, "[minimize] generated scenario from seed=%llu iter=%llu: "
                "%u ops, scenario_seed=%llu\n",
                (unsigned long long)initial_seed,
                (unsigned long long)target_iteration,
                sc.op_count,
                (unsigned long long)sc.seed);
    } else if (stdin_scenario) {
        if (min_read_scenario_stdin(&sc) != 0) {
            fprintf(stderr, "[minimize] error: failed to read scenario from stdin\n");
            return 1;
        }
        fprintf(stderr, "[minimize] read scenario from stdin: "
                "%u ops, scenario_seed=%llu\n",
                sc.op_count, (unsigned long long)sc.seed);
    } else {
        fprintf(stderr, "[minimize] error: no scenario source specified\n");
        fprintf(stderr, "[minimize] use --initial-seed, --stdin-scenario, or --selftest\n");
        return 1;
    }

    /* Set up minimization */
    {
        min_config cfg;
        min_result result;
        FILE *out_file;

        memset(&result, 0, sizeof(result));
        cfg.mode = MIN_MODE_DIGEST_MATCH;
        cfg.target_digest = has_target ? target_digest : min_execute_digest(&sc);
        cfg.max_rounds = max_rounds;
        cfg.verbose = verbose;
        cfg.predicate = NULL;
        cfg.predicate_data = NULL;

        fprintf(stderr, "[minimize] target digest: %016llx\n",
                (unsigned long long)cfg.target_digest);
        fprintf(stderr, "[minimize] starting with %u ops\n", sc.op_count);

        min_minimize(&sc, &cfg, &result);

        fprintf(stderr,
            "[minimize] result: %u -> %u ops in %u rounds (%.3fs)\n",
            result.original_ops, result.minimized_ops,
            result.rounds, result.duration_sec);

        /* Output JSONL to stdout */
        min_emit_json(stdout, &sc, &result);

        /* Optionally save to file */
        if (output_path != NULL) {
            out_file = fopen(output_path, "w");
            if (out_file != NULL) {
                min_emit_json(out_file, &sc, &result);
                fclose(out_file);
                fprintf(stderr, "[minimize] saved to: %s\n", output_path);
            } else {
                fprintf(stderr, "[minimize] error: cannot write to %s\n", output_path);
            }
        }
    }

    return 0;
}
