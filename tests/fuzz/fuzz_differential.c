/*
 * fuzz_differential.c — C determinism and mutation fuzz harness
 *
 * Bead: bd-1md.3
 *
 * Generates random and mutated scenario operation sequences, executes them
 * against the C runtime, and verifies:
 *   1. Deterministic self-consistency (same seed → same results)
 *   2. No crashes or undefined behavior under mutation
 * Rust parity uses make fuzz-differential: one canonical DSL v2 projection
 * executed by twin_run and asx-conformance. This harness checks C only.
 *
 * Coverage strategy: cancellation, timer, channel, obligation, budget exhaustion,
 * region lifecycle, and quiescence boundary conditions.
 *
 * Usage:
 *   fuzz_differential [options]
 *     --seed <u64>          Initial PRNG seed (default: 0)
 *     --iterations <n>      Number of fuzz iterations (default: 1000)
 *     --max-ops <n>         Max ops per scenario (default: 64)
 *     --mutations <n>       Mutations per scenario (default: 4)
 *     --report <path>       JSONL report output path (default: stdout)
 *     --smoke               CI smoke mode (100 iterations, fast)
 *     --nightly             Nightly mode (100000 iterations)
 *     --verbose             Print each scenario to stderr
 *
 * SPDX-License-Identifier: MIT
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <asx/asx.h>
#include <asx/time/timer_wheel.h>

/* ===================================================================
 * Configuration
 * =================================================================== */

#define FUZZ_MAX_OPS 128u
#define FUZZ_MAX_REGIONS ASX_MAX_REGIONS
#define FUZZ_MAX_TASKS ASX_MAX_TASKS
#define FUZZ_MAX_OBLIGATIONS 64u
#define FUZZ_MAX_CHANNELS ASX_MAX_CHANNELS
#define FUZZ_MAX_TIMERS 32u
#define FUZZ_MAX_RESULTS FUZZ_MAX_OPS
#define FUZZ_DIGEST_LEN 32u

/* ===================================================================
 * PRNG (xoshiro256** — deterministic, fast, high quality)
 * =================================================================== */

typedef struct {
    uint64_t s[4];
} fuzz_rng;

static uint64_t fuzz_rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

static uint64_t fuzz_rng_next(fuzz_rng *rng) {
    uint64_t result = fuzz_rotl(rng->s[1] * 5u, 7) * 9u;
    uint64_t t = rng->s[1] << 17;
    rng->s[2] ^= rng->s[0];
    rng->s[3] ^= rng->s[1];
    rng->s[1] ^= rng->s[2];
    rng->s[0] ^= rng->s[3];
    rng->s[2] ^= t;
    rng->s[3] = fuzz_rotl(rng->s[3], 45);
    return result;
}

static void fuzz_rng_seed(fuzz_rng *rng, uint64_t seed) {
    /* splitmix64 to initialize state from a single seed */
    uint64_t z = seed;
    int i;
    for (i = 0; i < 4; i++) {
        z += 0x9e3779b97f4a7c15ULL;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        rng->s[i] = z ^ (z >> 31);
    }
}

static uint32_t fuzz_rng_u32(fuzz_rng *rng, uint32_t bound) {
    if (bound == 0u) return 0u;
    return (uint32_t)(fuzz_rng_next(rng) % (uint64_t)bound);
}

/* ===================================================================
 * FNV-1a hash for semantic digest
 * =================================================================== */

typedef struct {
    uint64_t hash;
} fuzz_hasher;

static void fuzz_hasher_init(fuzz_hasher *h) { h->hash = 0xcbf29ce484222325ULL; }

static void fuzz_hasher_u8(fuzz_hasher *h, uint8_t b) {
    h->hash ^= (uint64_t)b;
    h->hash *= 0x100000001b3ULL;
}

static void fuzz_hasher_u32(fuzz_hasher *h, uint32_t v) {
    fuzz_hasher_u8(h, (uint8_t)(v & 0xFFu));
    fuzz_hasher_u8(h, (uint8_t)((v >> 8) & 0xFFu));
    fuzz_hasher_u8(h, (uint8_t)((v >> 16) & 0xFFu));
    fuzz_hasher_u8(h, (uint8_t)((v >> 24) & 0xFFu));
}

static void fuzz_hasher_i32(fuzz_hasher *h, int32_t v) { fuzz_hasher_u32(h, (uint32_t)v); }

static void fuzz_hasher_u64(fuzz_hasher *h, uint64_t v) {
    fuzz_hasher_u32(h, (uint32_t)(v & 0xFFFFFFFFu));
    fuzz_hasher_u32(h, (uint32_t)(v >> 32));
}

static uint64_t fuzz_hasher_finish(const fuzz_hasher *h) { return h->hash; }

/* ===================================================================
 * Scenario operation types
 * =================================================================== */

typedef enum {
    FUZZ_OP_SPAWN_REGION = 0,
    FUZZ_OP_CLOSE_REGION = 1,
    FUZZ_OP_POISON_REGION = 2,
    FUZZ_OP_SPAWN_TASK = 3,
    FUZZ_OP_CANCEL_TASK = 4,
    FUZZ_OP_RESERVE_OBLIGATION = 5,
    FUZZ_OP_COMMIT_OBLIGATION = 6,
    FUZZ_OP_ABORT_OBLIGATION = 7,
    FUZZ_OP_CHANNEL_CREATE = 8,
    FUZZ_OP_CHANNEL_RESERVE = 9,
    FUZZ_OP_CHANNEL_SEND = 10,
    FUZZ_OP_CHANNEL_ABORT = 11,
    FUZZ_OP_CHANNEL_RECV = 12,
    FUZZ_OP_CHANNEL_CLOSE_TX = 13,
    FUZZ_OP_CHANNEL_CLOSE_RX = 14,
    FUZZ_OP_TIMER_REGISTER = 15,
    FUZZ_OP_TIMER_CANCEL = 16,
    FUZZ_OP_ADVANCE_TIME = 17,
    FUZZ_OP_SCHEDULER_RUN = 18,
    FUZZ_OP_REGION_DRAIN = 19,
    FUZZ_OP_QUIESCENCE_CHECK = 20,
    FUZZ_OP_KIND_COUNT = 21
} fuzz_op_kind;

static const char *fuzz_op_name(fuzz_op_kind kind) {
    static const char *names[] = {"SpawnRegion",      "CloseRegion",     "PoisonRegion",
                                  "SpawnTask",        "CancelTask",      "ReserveObligation",
                                  "CommitObligation", "AbortObligation", "ChannelCreate",
                                  "ChannelReserve",   "ChannelSend",     "ChannelAbort",
                                  "ChannelRecv",      "ChannelCloseTx",  "ChannelCloseRx",
                                  "TimerRegister",    "TimerCancel",     "AdvanceTime",
                                  "SchedulerRun",     "RegionDrain",     "QuiescenceCheck"};
    if ((int)kind < 0 || (int)kind >= FUZZ_OP_KIND_COUNT) return "Unknown";
    return names[(int)kind];
}

typedef struct {
    fuzz_op_kind kind;
    uint32_t idx_a;   /* primary handle index (region/task/etc) */
    uint32_t idx_b;   /* secondary handle index */
    uint32_t arg_u32; /* capacity, cancel_kind, poll_quota, etc */
    uint64_t arg_u64; /* time, deadline, value */
} fuzz_op;

/* ===================================================================
 * Scenario and execution result
 * =================================================================== */

typedef struct {
    uint64_t seed;
    uint32_t op_count;
    fuzz_op ops[FUZZ_MAX_OPS];
} fuzz_scenario;

typedef struct {
    asx_status result;
} fuzz_op_result;

typedef struct {
    uint32_t op_count;
    fuzz_op_result results[FUZZ_MAX_RESULTS];
    uint32_t event_count;
    uint64_t digest;
    int crashed; /* set via signal handler if needed */
} fuzz_execution;

/* ===================================================================
 * Handle tracking during execution
 * =================================================================== */

typedef struct {
    asx_region_id regions[FUZZ_MAX_REGIONS];
    uint32_t region_count;

    asx_task_id tasks[FUZZ_MAX_TASKS];
    uint32_t task_count;

    asx_obligation_id obligations[FUZZ_MAX_OBLIGATIONS];
    uint32_t obligation_count;

    asx_channel_id channels[FUZZ_MAX_CHANNELS];
    uint32_t channel_count;

    asx_send_permit permits[FUZZ_MAX_CHANNELS];
    uint32_t permit_count;

    asx_timer_handle timers[FUZZ_MAX_TIMERS];
    uint32_t timer_count;

    uint64_t sim_time;
} fuzz_handle_state;

/* ===================================================================
 * Simple task poll function for fuzzing
 *
 * Each task runs for a configurable number of polls then completes.
 * This is embedded in the task's user_data as a counter.
 * =================================================================== */

typedef struct {
    uint32_t polls_remaining;
    asx_co_state co;
} fuzz_task_state;

static asx_status fuzz_task_poll(void *user_data, asx_task_id self) {
    fuzz_task_state *st = (fuzz_task_state *)user_data;
    (void)self;

    if (st->polls_remaining > 0u) {
        st->polls_remaining--;
        return ASX_E_PENDING;
    }
    return ASX_OK;
}

/* Static pool for task states (avoids malloc in tight loops) */
static fuzz_task_state g_task_states[FUZZ_MAX_TASKS];
static uint32_t g_task_state_next = 0u;

static fuzz_task_state *fuzz_alloc_task_state(uint32_t polls) {
    fuzz_task_state *st;
    if (g_task_state_next >= FUZZ_MAX_TASKS) return NULL;
    st = &g_task_states[g_task_state_next++];
    st->polls_remaining = polls;
    st->co.line = 0u;
    return st;
}

static void fuzz_reset_task_states(void) { g_task_state_next = 0u; }

/* ===================================================================
 * Scenario generation
 * =================================================================== */

/* Weight table for op kind selection.
 * Biased toward lifecycle and cancellation for coverage. */
static const uint32_t OP_WEIGHTS[FUZZ_OP_KIND_COUNT] = {
    /* SpawnRegion */ 12,
    /* CloseRegion */ 8,
    /* PoisonRegion */ 3,
    /* SpawnTask */ 15,
    /* CancelTask */ 10,
    /* ReserveObl */ 8,
    /* CommitObl */ 7,
    /* AbortObl */ 5,
    /* ChCreate */ 6,
    /* ChReserve */ 5,
    /* ChSend */ 5,
    /* ChAbort */ 3,
    /* ChRecv */ 5,
    /* ChCloseTx */ 3,
    /* ChCloseRx */ 3,
    /* TimerReg */ 6,
    /* TimerCancel */ 4,
    /* AdvanceTime */ 5,
    /* SchedRun */ 12,
    /* RegionDrain */ 5,
    /* QuiesCheck */ 4};

static fuzz_op_kind fuzz_pick_op(fuzz_rng *rng) {
    uint32_t total = 0u;
    uint32_t r, acc;
    int i;

    for (i = 0; i < FUZZ_OP_KIND_COUNT; i++) { total += OP_WEIGHTS[i]; }
    r = fuzz_rng_u32(rng, total);
    acc = 0u;
    for (i = 0; i < FUZZ_OP_KIND_COUNT; i++) {
        acc += OP_WEIGHTS[i];
        if (r < acc) return (fuzz_op_kind)i;
    }
    return FUZZ_OP_SPAWN_REGION;
}

static void fuzz_generate_scenario(fuzz_rng *rng, fuzz_scenario *sc, uint32_t max_ops) {
    uint32_t n, i;

    sc->seed = fuzz_rng_next(rng);
    n = 4u + fuzz_rng_u32(rng, max_ops > 4u ? max_ops - 4u : 1u);
    if (n > FUZZ_MAX_OPS) n = FUZZ_MAX_OPS;
    sc->op_count = n;

    /* Always start with SpawnRegion to have something to work with */
    sc->ops[0].kind = FUZZ_OP_SPAWN_REGION;
    sc->ops[0].idx_a = 0u;
    sc->ops[0].idx_b = 0u;
    sc->ops[0].arg_u32 = 0u;
    sc->ops[0].arg_u64 = 0u;

    for (i = 1u; i < n; i++) {
        fuzz_op *op = &sc->ops[i];
        op->kind = fuzz_pick_op(rng);
        op->idx_a = fuzz_rng_u32(rng, FUZZ_MAX_REGIONS);
        op->idx_b = fuzz_rng_u32(rng, FUZZ_MAX_TASKS);
        op->arg_u32 = fuzz_rng_u32(rng, 32u);
        op->arg_u64 = fuzz_rng_next(rng) % 10000u;
    }
}

/* ===================================================================
 * Mutation engine
 *
 * Applies one of several mutation operators to a scenario.
 * All mutations are deterministic given the mutation seed.
 * =================================================================== */

typedef enum {
    FUZZ_MUT_REMOVE_OP = 0,
    FUZZ_MUT_DUPLICATE_OP = 1,
    FUZZ_MUT_SWAP_OPS = 2,
    FUZZ_MUT_CHANGE_KIND = 3,
    FUZZ_MUT_TWEAK_ARG = 4,
    FUZZ_MUT_INSERT_OP = 5,
    FUZZ_MUT_CHANGE_IDX = 6,
    FUZZ_MUT_COUNT = 7
} fuzz_mutation_kind;

static const char *fuzz_mutation_name(fuzz_mutation_kind kind) {
    static const char *names[] = {"remove_op", "duplicate_op", "swap_ops",  "change_kind",
                                  "tweak_arg", "insert_op",    "change_idx"};
    if ((int)kind < 0 || (int)kind >= FUZZ_MUT_COUNT) return "unknown";
    return names[(int)kind];
}

typedef struct {
    fuzz_mutation_kind kind;
    uint32_t target_idx;
    uint32_t secondary;
} fuzz_mutation_record;

static fuzz_mutation_record fuzz_mutate(fuzz_rng *rng, fuzz_scenario *sc) {
    fuzz_mutation_record rec;
    fuzz_mutation_kind mut;
    uint32_t idx, idx2;

    memset(&rec, 0, sizeof(rec));

    if (sc->op_count < 2u) {
        /* Can't mutate a trivial scenario meaningfully */
        rec.kind = FUZZ_MUT_TWEAK_ARG;
        rec.target_idx = 0u;
        if (sc->op_count > 0u) { sc->ops[0].arg_u32 = fuzz_rng_u32(rng, 64u); }
        return rec;
    }

    mut = (fuzz_mutation_kind)fuzz_rng_u32(rng, FUZZ_MUT_COUNT);
    rec.kind = mut;

    switch (mut) {
    case FUZZ_MUT_REMOVE_OP:
        /* Remove a random op (not the first SpawnRegion) */
        idx = 1u + fuzz_rng_u32(rng, sc->op_count - 1u);
        rec.target_idx = idx;
        memmove(&sc->ops[idx], &sc->ops[idx + 1u],
                (size_t)(sc->op_count - idx - 1u) * sizeof(fuzz_op));
        sc->op_count--;
        break;

    case FUZZ_MUT_DUPLICATE_OP:
        if (sc->op_count < FUZZ_MAX_OPS) {
            idx = fuzz_rng_u32(rng, sc->op_count);
            rec.target_idx = idx;
            memmove(&sc->ops[sc->op_count + 1u], &sc->ops[sc->op_count], 0u); /* just need space */
            /* Insert copy at end */
            sc->ops[sc->op_count] = sc->ops[idx];
            sc->op_count++;
        }
        break;

    case FUZZ_MUT_SWAP_OPS:
        idx = 1u + fuzz_rng_u32(rng, sc->op_count - 1u);
        idx2 = 1u + fuzz_rng_u32(rng, sc->op_count - 1u);
        rec.target_idx = idx;
        rec.secondary = idx2;
        if (idx != idx2) {
            fuzz_op tmp = sc->ops[idx];
            sc->ops[idx] = sc->ops[idx2];
            sc->ops[idx2] = tmp;
        }
        break;

    case FUZZ_MUT_CHANGE_KIND:
        idx = 1u + fuzz_rng_u32(rng, sc->op_count - 1u);
        rec.target_idx = idx;
        sc->ops[idx].kind = fuzz_pick_op(rng);
        break;

    case FUZZ_MUT_TWEAK_ARG:
        idx = fuzz_rng_u32(rng, sc->op_count);
        rec.target_idx = idx;
        switch (fuzz_rng_u32(rng, 3u)) {
        case 0u: sc->ops[idx].arg_u32 = fuzz_rng_u32(rng, 128u); break;
        case 1u: sc->ops[idx].arg_u64 = fuzz_rng_next(rng) % 100000u; break;
        default: sc->ops[idx].arg_u32 ^= (1u << fuzz_rng_u32(rng, 32u)); break;
        }
        break;

    case FUZZ_MUT_INSERT_OP:
        if (sc->op_count < FUZZ_MAX_OPS) {
            idx = 1u + fuzz_rng_u32(rng, sc->op_count);
            if (idx > sc->op_count) idx = sc->op_count;
            rec.target_idx = idx;
            memmove(&sc->ops[idx + 1u], &sc->ops[idx],
                    (size_t)(sc->op_count - idx) * sizeof(fuzz_op));
            sc->ops[idx].kind = fuzz_pick_op(rng);
            sc->ops[idx].idx_a = fuzz_rng_u32(rng, FUZZ_MAX_REGIONS);
            sc->ops[idx].idx_b = fuzz_rng_u32(rng, FUZZ_MAX_TASKS);
            sc->ops[idx].arg_u32 = fuzz_rng_u32(rng, 32u);
            sc->ops[idx].arg_u64 = fuzz_rng_next(rng) % 10000u;
            sc->op_count++;
        }
        break;

    case FUZZ_MUT_CHANGE_IDX:
        idx = fuzz_rng_u32(rng, sc->op_count);
        rec.target_idx = idx;
        if (fuzz_rng_u32(rng, 2u) == 0u) {
            sc->ops[idx].idx_a = fuzz_rng_u32(rng, FUZZ_MAX_REGIONS);
        } else {
            sc->ops[idx].idx_b = fuzz_rng_u32(rng, FUZZ_MAX_TASKS);
        }
        break;

    default: break;
    }

    return rec;
}

/* ===================================================================
 * Scenario executor
 *
 * Runs a scenario through the C runtime, collecting per-op status
 * codes and computing a semantic digest.
 * =================================================================== */

static void fuzz_execute(const fuzz_scenario *sc, fuzz_execution *exec) {
    fuzz_handle_state hs;
    fuzz_hasher hasher;
    uint32_t i;

    memset(&hs, 0, sizeof(hs));
    memset(exec, 0, sizeof(*exec));
    exec->op_count = sc->op_count;

    /* Reset all runtime state for clean execution */
    asx_runtime_reset();
    asx_channel_reset();
    asx_timer_wheel_reset(asx_timer_wheel_global());
    fuzz_reset_task_states();

    fuzz_hasher_init(&hasher);
    fuzz_hasher_u64(&hasher, sc->seed);
    fuzz_hasher_u32(&hasher, sc->op_count);

    for (i = 0u; i < sc->op_count; i++) {
        const fuzz_op *op = &sc->ops[i];
        asx_status st = ASX_OK;

        fuzz_hasher_u32(&hasher, (uint32_t)op->kind);

        switch (op->kind) {
        case FUZZ_OP_SPAWN_REGION: {
            asx_region_id rid = ASX_INVALID_ID;
            if (hs.region_count < FUZZ_MAX_REGIONS) {
                st = asx_region_open(&rid);
                if (st == ASX_OK) { hs.regions[hs.region_count++] = rid; }
            } else {
                st = ASX_E_REGION_AT_CAPACITY;
            }
            break;
        }

        case FUZZ_OP_CLOSE_REGION: {
            if (hs.region_count > 0u) {
                uint32_t idx = op->idx_a % hs.region_count;
                st = asx_region_close(hs.regions[idx]);
            } else {
                st = ASX_E_NOT_FOUND;
            }
            break;
        }

        case FUZZ_OP_POISON_REGION: {
            if (hs.region_count > 0u) {
                uint32_t idx = op->idx_a % hs.region_count;
                st = asx_region_poison(hs.regions[idx]);
            } else {
                st = ASX_E_NOT_FOUND;
            }
            break;
        }

        case FUZZ_OP_SPAWN_TASK: {
            if (hs.region_count > 0u && hs.task_count < FUZZ_MAX_TASKS) {
                uint32_t ridx = op->idx_a % hs.region_count;
                uint32_t polls = 1u + (op->arg_u32 % 16u);
                fuzz_task_state *tst = fuzz_alloc_task_state(polls);
                asx_task_id tid = ASX_INVALID_ID;
                if (tst != NULL) {
                    st = asx_task_spawn(hs.regions[ridx], fuzz_task_poll, tst, &tid);
                    if (st == ASX_OK) { hs.tasks[hs.task_count++] = tid; }
                } else {
                    st = ASX_E_RESOURCE_EXHAUSTED;
                }
            } else {
                st = ASX_E_NOT_FOUND;
            }
            break;
        }

        case FUZZ_OP_CANCEL_TASK: {
            if (hs.task_count > 0u) {
                uint32_t tidx = op->idx_b % hs.task_count;
                asx_cancel_kind kind = (asx_cancel_kind)(op->arg_u32 % 11u);
                st = asx_task_cancel(hs.tasks[tidx], kind);
            } else {
                st = ASX_E_NOT_FOUND;
            }
            break;
        }

        case FUZZ_OP_RESERVE_OBLIGATION: {
            if (hs.region_count > 0u && hs.obligation_count < FUZZ_MAX_OBLIGATIONS) {
                uint32_t ridx = op->idx_a % hs.region_count;
                asx_obligation_id oid = ASX_INVALID_ID;
                st = asx_obligation_reserve(hs.regions[ridx], &oid);
                if (st == ASX_OK) { hs.obligations[hs.obligation_count++] = oid; }
            } else {
                st = ASX_E_NOT_FOUND;
            }
            break;
        }

        case FUZZ_OP_COMMIT_OBLIGATION: {
            if (hs.obligation_count > 0u) {
                uint32_t oidx = op->idx_a % hs.obligation_count;
                st = asx_obligation_commit(hs.obligations[oidx]);
            } else {
                st = ASX_E_NOT_FOUND;
            }
            break;
        }

        case FUZZ_OP_ABORT_OBLIGATION: {
            if (hs.obligation_count > 0u) {
                uint32_t oidx = op->idx_a % hs.obligation_count;
                st = asx_obligation_abort(hs.obligations[oidx]);
            } else {
                st = ASX_E_NOT_FOUND;
            }
            break;
        }

        case FUZZ_OP_CHANNEL_CREATE: {
            if (hs.region_count > 0u && hs.channel_count < FUZZ_MAX_CHANNELS) {
                uint32_t ridx = op->idx_a % hs.region_count;
                uint32_t cap = 1u + (op->arg_u32 % (ASX_CHANNEL_MAX_CAPACITY - 1u));
                asx_channel_id cid = ASX_INVALID_ID;
                st = asx_channel_create(hs.regions[ridx], cap, &cid);
                if (st == ASX_OK) { hs.channels[hs.channel_count++] = cid; }
            } else {
                st = ASX_E_NOT_FOUND;
            }
            break;
        }

        case FUZZ_OP_CHANNEL_RESERVE: {
            if (hs.channel_count > 0u && hs.permit_count < FUZZ_MAX_CHANNELS) {
                uint32_t cidx = op->idx_a % hs.channel_count;
                asx_send_permit permit;
                memset(&permit, 0, sizeof(permit));
                st = asx_channel_try_reserve(hs.channels[cidx], &permit);
                if (st == ASX_OK) { hs.permits[hs.permit_count++] = permit; }
            } else {
                st = ASX_E_NOT_FOUND;
            }
            break;
        }

        case FUZZ_OP_CHANNEL_SEND: {
            if (hs.permit_count > 0u) {
                uint32_t pidx = op->idx_a % hs.permit_count;
                st = asx_send_permit_send(&hs.permits[pidx], op->arg_u64);
            } else {
                st = ASX_E_NOT_FOUND;
            }
            break;
        }

        case FUZZ_OP_CHANNEL_ABORT: {
            if (hs.permit_count > 0u) {
                uint32_t pidx = op->idx_a % hs.permit_count;
                asx_send_permit_abort(&hs.permits[pidx]);
                st = ASX_OK;
            } else {
                st = ASX_E_NOT_FOUND;
            }
            break;
        }

        case FUZZ_OP_CHANNEL_RECV: {
            if (hs.channel_count > 0u) {
                uint32_t cidx = op->idx_a % hs.channel_count;
                uint64_t val = 0u;
                st = asx_channel_try_recv(hs.channels[cidx], &val);
            } else {
                st = ASX_E_NOT_FOUND;
            }
            break;
        }

        case FUZZ_OP_CHANNEL_CLOSE_TX: {
            if (hs.channel_count > 0u) {
                uint32_t cidx = op->idx_a % hs.channel_count;
                st = asx_channel_close_sender(hs.channels[cidx]);
            } else {
                st = ASX_E_NOT_FOUND;
            }
            break;
        }

        case FUZZ_OP_CHANNEL_CLOSE_RX: {
            if (hs.channel_count > 0u) {
                uint32_t cidx = op->idx_a % hs.channel_count;
                st = asx_channel_close_receiver(hs.channels[cidx]);
            } else {
                st = ASX_E_NOT_FOUND;
            }
            break;
        }

        case FUZZ_OP_TIMER_REGISTER: {
            if (hs.timer_count < FUZZ_MAX_TIMERS) {
                asx_timer_handle th;
                asx_time deadline = hs.sim_time + 1u + (op->arg_u64 % 5000u);
                memset(&th, 0, sizeof(th));
                st = asx_timer_register(asx_timer_wheel_global(), deadline, NULL, &th);
                if (st == ASX_OK) { hs.timers[hs.timer_count++] = th; }
            } else {
                st = ASX_E_RESOURCE_EXHAUSTED;
            }
            break;
        }

        case FUZZ_OP_TIMER_CANCEL: {
            if (hs.timer_count > 0u) {
                uint32_t tidx = op->idx_a % hs.timer_count;
                int cancelled = asx_timer_cancel(asx_timer_wheel_global(), &hs.timers[tidx]);
                st = cancelled ? ASX_OK : ASX_E_TIMER_NOT_FOUND;
            } else {
                st = ASX_E_NOT_FOUND;
            }
            break;
        }

        case FUZZ_OP_ADVANCE_TIME: {
            uint64_t advance = 1u + (op->arg_u64 % 2000u);
            void *wakers[16];
            hs.sim_time += advance;
            asx_timer_collect_expired(asx_timer_wheel_global(), hs.sim_time, wakers, 16u);
            st = ASX_OK;
            break;
        }

        case FUZZ_OP_SCHEDULER_RUN: {
            if (hs.region_count > 0u) {
                uint32_t ridx = op->idx_a % hs.region_count;
                uint32_t quota = 1u + (op->arg_u32 % 64u);
                asx_budget budget;
                budget = asx_budget_infinite();
                budget.poll_quota = quota;
                st = asx_scheduler_run(hs.regions[ridx], &budget);
            } else {
                st = ASX_E_NOT_FOUND;
            }
            break;
        }

        case FUZZ_OP_REGION_DRAIN: {
            if (hs.region_count > 0u) {
                uint32_t ridx = op->idx_a % hs.region_count;
                asx_budget budget;
                budget = asx_budget_infinite();
                budget.poll_quota = 256u;
                st = asx_region_drain(hs.regions[ridx], &budget);
            } else {
                st = ASX_E_NOT_FOUND;
            }
            break;
        }

        case FUZZ_OP_QUIESCENCE_CHECK: {
            if (hs.region_count > 0u) {
                uint32_t ridx = op->idx_a % hs.region_count;
                st = asx_quiescence_check(hs.regions[ridx]);
            } else {
                st = ASX_E_NOT_FOUND;
            }
            break;
        }

        default: st = ASX_E_INVALID_ARGUMENT; break;
        }

        exec->results[i].result = st;
        fuzz_hasher_i32(&hasher, (int32_t)st);
    }

    /* Hash scheduler event log for extra determinism verification */
    exec->event_count = asx_scheduler_event_count();
    fuzz_hasher_u32(&hasher, exec->event_count);
    {
        uint32_t ei;
        for (ei = 0u; ei < exec->event_count; ei++) {
            asx_scheduler_event ev;
            if (asx_scheduler_event_get(ei, &ev)) {
                fuzz_hasher_u32(&hasher, (uint32_t)ev.kind);
                fuzz_hasher_u64(&hasher, ev.task_id);
                fuzz_hasher_u32(&hasher, ev.sequence);
            }
        }
    }

    exec->digest = fuzz_hasher_finish(&hasher);
}

/* ===================================================================
 * Time helpers
 * =================================================================== */

static double fuzz_clock_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ===================================================================
 * JSONL reporting
 * =================================================================== */

static void fuzz_dump_scenario_ops(FILE *out, const fuzz_scenario *sc, const fuzz_execution *exec) {
    uint32_t i;
    fprintf(out, ",\"ops\":[");
    for (i = 0u; i < sc->op_count; i++) {
        if (i > 0u) fprintf(out, ",");
        fprintf(out,
                "{\"op\":\"%s\",\"idx_a\":%u,\"idx_b\":%u,"
                "\"arg_u32\":%u,\"arg_u64\":%llu,\"result\":%d}",
                fuzz_op_name(sc->ops[i].kind), sc->ops[i].idx_a, sc->ops[i].idx_b,
                sc->ops[i].arg_u32, (unsigned long long)sc->ops[i].arg_u64,
                i < exec->op_count ? (int)exec->results[i].result : -1);
    }
    fprintf(out, "]");
}

static void fuzz_report_mismatch(FILE *out, const char *kind, uint64_t iteration, uint64_t seed,
                                 uint64_t digest_a, uint64_t digest_b, const fuzz_scenario *sc,
                                 const fuzz_execution *exec, const fuzz_mutation_record *mutations,
                                 uint32_t mutation_count) {
    uint32_t m;
    fprintf(out,
            "{\"kind\":\"%s\","
            "\"iteration\":%llu,"
            "\"seed\":%llu,"
            "\"digest_a\":\"%016llx\","
            "\"digest_b\":\"%016llx\","
            "\"mutations\":[",
            kind, (unsigned long long)iteration, (unsigned long long)seed,
            (unsigned long long)digest_a, (unsigned long long)digest_b);
    for (m = 0u; m < mutation_count; m++) {
        if (m > 0u) fprintf(out, ",");
        fprintf(out, "{\"kind\":\"%s\",\"target\":%u,\"secondary\":%u}",
                fuzz_mutation_name(mutations[m].kind), mutations[m].target_idx,
                mutations[m].secondary);
    }
    fprintf(out, "]");
    if (sc != NULL && exec != NULL) { fuzz_dump_scenario_ops(out, sc, exec); }
    fprintf(out, "}\n");
    fflush(out);
}

static void fuzz_report_summary(FILE *out, uint64_t initial_seed, uint64_t iterations,
                                uint64_t determinism_failures, uint64_t crash_count,
                                double duration_sec) {
    fprintf(out,
            "{\"kind\":\"summary\","
            "\"initial_seed\":%llu,"
            "\"iterations\":%llu,"
            "\"determinism_failures\":%llu,"
            "\"crashes\":%llu,"
            "\"duration_sec\":%.3f,"
            "\"iterations_per_sec\":%.1f}\n",
            (unsigned long long)initial_seed, (unsigned long long)iterations,
            (unsigned long long)determinism_failures, (unsigned long long)crash_count, duration_sec,
            iterations > 0 ? (double)iterations / duration_sec : 0.0);
    fflush(out);
}

/* `mkdir -p` without a shell: create each missing component of `path`.
 * Returns 0 when the directory exists afterwards. */
static int fuzz_mkdir_p(const char *path) {
    char buf[512];
    size_t i;
    size_t len = strlen(path);
    if (len == 0u || len >= sizeof(buf)) return -1;
    memcpy(buf, path, len + 1u);
    for (i = 1; i <= len; i++) {
        if (buf[i] == '/' || buf[i] == '\0') {
            char saved = buf[i];
            buf[i] = '\0';
            if (mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
            buf[i] = saved;
        }
    }
    return 0;
}

/* ===================================================================
 * Auto-minimize: save scenario and invoke minimizer
 * =================================================================== */

static void fuzz_save_and_minimize(const fuzz_scenario *sc, const fuzz_execution *exec,
                                   uint64_t c_digest, const char *minimize_binary,
                                   const char *findings_dir, int verbose) {
    char path[512];
    FILE *f;
    uint32_t i;

    /* Save scenario as text file for minimizer stdin */
    snprintf(path, sizeof(path), "%s/seed_%llu.scenario", findings_dir,
             (unsigned long long)sc->seed);
    f = fopen(path, "w");
    if (f == NULL) {
        if (verbose) { fprintf(stderr, "[fuzz] warning: cannot write scenario to %s\n", path); }
        return;
    }

    /* Write in text format: "seed op_count kind0 a0 b0 u32_0 u64_0 ..." */
    fprintf(f, "%llu %u", (unsigned long long)sc->seed, sc->op_count);
    for (i = 0u; i < sc->op_count; i++) {
        fprintf(f, " %u %u %u %u %llu", (uint32_t)sc->ops[i].kind, sc->ops[i].idx_a,
                sc->ops[i].idx_b, sc->ops[i].arg_u32, (unsigned long long)sc->ops[i].arg_u64);
    }
    fprintf(f, "\n");
    fclose(f);

    /* Invoke minimizer if available: fork+exec with the finding on stdin, no
     * shell (paths and digests never pass through a command line). */
    if (minimize_binary != NULL) {
        char out_path[512];
        char digest_arg[32];
        pid_t pid;
        int status = 0;

        snprintf(out_path, sizeof(out_path), "%s/seed_%llu.minimized.json", findings_dir,
                 (unsigned long long)sc->seed);
        snprintf(digest_arg, sizeof(digest_arg), "%016llx", (unsigned long long)c_digest);

        if (verbose) {
            fprintf(stderr, "[fuzz] minimizing seed=%llu...\n", (unsigned long long)sc->seed);
        }

        pid = fork();
        if (pid == 0) {
            int fd = open(path, O_RDONLY);
            if (fd < 0 || dup2(fd, 0) < 0) _exit(126);
            close(fd);
            if (verbose) {
                execl(minimize_binary, minimize_binary, "--stdin-scenario", "--failure-digest",
                      digest_arg, "--output", out_path, "--verbose", (char *)NULL);
            } else {
                execl(minimize_binary, minimize_binary, "--stdin-scenario", "--failure-digest",
                      digest_arg, "--output", out_path, (char *)NULL);
            }
            _exit(127);
        }
        if (pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
            WEXITSTATUS(status) == 0) {
            if (verbose) { fprintf(stderr, "[fuzz] minimized saved to %s\n", out_path); }
        } else if (verbose) {
            fprintf(stderr, "[fuzz] minimizer failed for seed=%llu\n",
                    (unsigned long long)sc->seed);
        }
    }

    (void)exec;
}

/* ===================================================================
 * Main fuzz loop
 * =================================================================== */

typedef struct {
    uint64_t initial_seed;
    uint64_t iterations;
    uint32_t max_ops;
    uint32_t mutations_per_scenario;
    const char *report_path;
    const char *minimize_binary;
    const char *findings_dir;
    int verbose;
} fuzz_config;

static void fuzz_config_defaults(fuzz_config *cfg) {
    cfg->initial_seed = 0u;
    cfg->iterations = 1000u;
    cfg->max_ops = 64u;
    cfg->mutations_per_scenario = 4u;
    cfg->report_path = NULL;
    cfg->minimize_binary = NULL;
    cfg->findings_dir = "fuzz_findings/minimized";
    cfg->verbose = 0;
}

static int fuzz_run(const fuzz_config *cfg) {
    fuzz_rng rng;
    FILE *report;
    uint64_t iter;
    uint64_t determinism_failures = 0u;
    uint64_t crash_count = 0u;
    double start_time;
    double end_time;
    int exit_code = 0;

    fuzz_rng_seed(&rng, cfg->initial_seed);

    report = stdout;
    if (cfg->report_path != NULL) {
        report = fopen(cfg->report_path, "w");
        if (report == NULL) {
            fprintf(stderr, "[fuzz] error: cannot open report file: %s\n", cfg->report_path);
            return 1;
        }
    }

    fprintf(stderr,
            "[fuzz] C determinism and mutation fuzz harness (bd-1md.3)\n"
            "[fuzz] seed=%llu iterations=%llu max_ops=%u mutations=%u\n",
            (unsigned long long)cfg->initial_seed, (unsigned long long)cfg->iterations,
            cfg->max_ops, cfg->mutations_per_scenario);

    /* ---- Create findings directory if --minimize specified ---- */
    if (cfg->minimize_binary != NULL && fuzz_mkdir_p(cfg->findings_dir) != 0) {
        fprintf(stderr, "[fuzz] warning: cannot create findings dir: %s\n", cfg->findings_dir);
    }

    start_time = fuzz_clock_sec();

    for (iter = 0u; iter < cfg->iterations; iter++) {
        fuzz_scenario base_scenario;
        fuzz_scenario mutated;
        fuzz_execution exec_a;
        fuzz_execution exec_b;
        fuzz_mutation_record mutations[16];
        uint32_t mut_count = 0u;
        uint32_t m;

        /* Generate a random base scenario */
        fuzz_generate_scenario(&rng, &base_scenario, cfg->max_ops);

        /* ---- Phase 1: Determinism self-check ---- */
        /* Run the base scenario twice and verify identical digest */
        fuzz_execute(&base_scenario, &exec_a);
        fuzz_execute(&base_scenario, &exec_b);

        if (exec_a.digest != exec_b.digest) {
            determinism_failures++;
            fuzz_report_mismatch(report, "determinism_failure", iter, base_scenario.seed,
                                 exec_a.digest, exec_b.digest, &base_scenario, &exec_a, NULL, 0u);
            if (cfg->verbose) {
                fprintf(stderr,
                        "[fuzz] DETERMINISM FAILURE iter=%llu seed=%llu "
                        "digest_a=%016llx digest_b=%016llx\n",
                        (unsigned long long)iter, (unsigned long long)base_scenario.seed,
                        (unsigned long long)exec_a.digest, (unsigned long long)exec_b.digest);
            }
            if (cfg->minimize_binary != NULL) {
                fuzz_save_and_minimize(&base_scenario, &exec_a, exec_a.digest, cfg->minimize_binary,
                                       cfg->findings_dir, cfg->verbose);
            }
        }

        /* ---- Phase 2: Mutation + crash detection ---- */
        /* Apply mutations and execute, checking for crashes */
        memcpy(&mutated, &base_scenario, sizeof(fuzz_scenario));

        for (m = 0u; m < cfg->mutations_per_scenario && m < 16u; m++) {
            mutations[m] = fuzz_mutate(&rng, &mutated);
            mut_count++;
        }

        /* Execute mutated scenario (crash detection via result) */
        fuzz_execute(&mutated, &exec_b);

        /* Verify mutated scenario is also deterministic */
        {
            fuzz_execution exec_verify;
            fuzz_execute(&mutated, &exec_verify);
            if (exec_b.digest != exec_verify.digest) {
                determinism_failures++;
                fuzz_report_mismatch(report, "mutant_determinism_failure", iter, mutated.seed,
                                     exec_b.digest, exec_verify.digest, &mutated, &exec_b,
                                     mutations, mut_count);
                if (cfg->verbose) {
                    fprintf(stderr, "[fuzz] MUTANT DETERMINISM FAILURE iter=%llu\n",
                            (unsigned long long)iter);
                }
            }
        }

        /* Progress reporting */
        if (cfg->verbose && (iter % 100u == 0u)) {
            double elapsed = fuzz_clock_sec() - start_time;
            fprintf(stderr, "[fuzz] progress: %llu/%llu (%.1f/s) det_fail=%llu\n",
                    (unsigned long long)iter, (unsigned long long)cfg->iterations,
                    iter > 0u ? (double)iter / elapsed : 0.0,
                    (unsigned long long)determinism_failures);
        }
    }

    end_time = fuzz_clock_sec();

    fuzz_report_summary(report, cfg->initial_seed, cfg->iterations, determinism_failures,
                        crash_count, end_time - start_time);

    fprintf(stderr,
            "[fuzz] complete: %llu iterations in %.3fs (%.1f/s)\n"
            "[fuzz] determinism_failures=%llu crashes=%llu\n",
            (unsigned long long)cfg->iterations, end_time - start_time,
            (double)cfg->iterations / (end_time - start_time),
            (unsigned long long)determinism_failures, (unsigned long long)crash_count);
    fprintf(stderr, "[fuzz] Rust parity: use make fuzz-differential (live DSL v2 oracle)\n");

    if (determinism_failures > 0u || crash_count > 0u) {
        fprintf(stderr, "[fuzz] FAIL: issues detected\n");
        exit_code = 1;
    } else {
        fprintf(stderr, "[fuzz] PASS: no C determinism failures or crashes\n");
    }

    if (report != stdout) { fclose(report); }

    return exit_code;
}

/* ===================================================================
 * CLI argument parsing
 * =================================================================== */

static uint64_t parse_u64(const char *s) {
    uint64_t v = 0u;
    while (*s >= '0' && *s <= '9') {
        v = v * 10u + (uint64_t)(*s - '0');
        s++;
    }
    return v;
}

int main(int argc, char **argv) {
    fuzz_config cfg;
    int i;

    fuzz_config_defaults(&cfg);

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            cfg.initial_seed = parse_u64(argv[++i]);
        } else if (strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) {
            cfg.iterations = parse_u64(argv[++i]);
        } else if (strcmp(argv[i], "--max-ops") == 0 && i + 1 < argc) {
            cfg.max_ops = (uint32_t)parse_u64(argv[++i]);
        } else if (strcmp(argv[i], "--mutations") == 0 && i + 1 < argc) {
            cfg.mutations_per_scenario = (uint32_t)parse_u64(argv[++i]);
        } else if (strcmp(argv[i], "--minimize") == 0 && i + 1 < argc) {
            cfg.minimize_binary = argv[++i];
        } else if (strcmp(argv[i], "--findings-dir") == 0 && i + 1 < argc) {
            cfg.findings_dir = argv[++i];
        } else if (strcmp(argv[i], "--report") == 0 && i + 1 < argc) {
            cfg.report_path = argv[++i];
        } else if (strcmp(argv[i], "--smoke") == 0) {
            cfg.iterations = 100u;
            cfg.max_ops = 32u;
        } else if (strcmp(argv[i], "--nightly") == 0) {
            cfg.iterations = 100000u;
            cfg.max_ops = 96u;
            cfg.mutations_per_scenario = 8u;
        } else if (strcmp(argv[i], "--verbose") == 0) {
            cfg.verbose = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            fprintf(
                stderr,
                "Usage: fuzz_differential [options]\n"
                "  --seed <u64>          Initial PRNG seed (default: 0)\n"
                "  --iterations <n>      Number of fuzz iterations (default: 1000)\n"
                "  --max-ops <n>         Max ops per scenario (default: 64)\n"
                "  --mutations <n>       Mutations per scenario (default: 4)\n"
                "  --minimize <path>     Path to fuzz_minimize binary (auto-minimize divergences)\n"
                "  --findings-dir <dir>  Directory for minimized scenarios (default: "
                "fuzz_findings)\n"
                "  --report <path>       JSONL report output path\n"
                "  --smoke               CI smoke mode (100 iterations)\n"
                "  --nightly             Nightly mode (100000 iterations)\n"
                "  --verbose             Verbose progress output\n");
            return 0;
        } else {
            fprintf(stderr, "[fuzz] unknown argument: %s\n", argv[i]);
            return 2;
        }
    }

    return fuzz_run(&cfg);
}
