/*
 * generate_grammar_vectors_jsonl.c — JSONL output for cross-language comparison
 *
 * Outputs one JSON line per seed (0..N-1), each containing the scenario
 * structure for diff-based comparison against the Rust fuzz target.
 *
 * Usage: generate_grammar_vectors_jsonl [num_seeds]
 * Default: 100 seeds
 *
 * Bead: bd-rkql.6
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define FUZZ_MAX_OPS 128u
#define FUZZ_MAX_REGIONS 8u
#define FUZZ_MAX_TASKS 64u
#define FUZZ_OP_KIND_COUNT 21

/* PRNG: xoshiro256** (identical to fuzz_differential.c) */
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

/* Op types */
static const char *fuzz_op_names[] = {"SpawnRegion",      "CloseRegion",     "PoisonRegion",
                                      "SpawnTask",        "CancelTask",      "ReserveObligation",
                                      "CommitObligation", "AbortObligation", "ChannelCreate",
                                      "ChannelReserve",   "ChannelSend",     "ChannelAbort",
                                      "ChannelRecv",      "ChannelCloseTx",  "ChannelCloseRx",
                                      "TimerRegister",    "TimerCancel",     "AdvanceTime",
                                      "SchedulerRun",     "RegionDrain",     "QuiescenceCheck"};

static const uint32_t OP_WEIGHTS[FUZZ_OP_KIND_COUNT] = {12, 8, 3, 15, 10, 8, 7, 5,  6, 5, 5,
                                                        3,  5, 3, 3,  6,  4, 5, 12, 5, 4};

typedef struct {
    uint32_t kind;
    uint32_t idx_a, idx_b, arg_u32;
    uint64_t arg_u64;
} fuzz_op;

typedef struct {
    uint64_t seed;
    uint32_t op_count;
    fuzz_op ops[FUZZ_MAX_OPS];
} fuzz_scenario;

static uint32_t fuzz_pick_op(fuzz_rng *rng) {
    uint32_t total = 0u, r, acc;
    int i;
    for (i = 0; i < FUZZ_OP_KIND_COUNT; i++) total += OP_WEIGHTS[i];
    r = fuzz_rng_u32(rng, total);
    acc = 0u;
    for (i = 0; i < FUZZ_OP_KIND_COUNT; i++) {
        acc += OP_WEIGHTS[i];
        if (r < acc) return (uint32_t)i;
    }
    return 0u;
}

static void fuzz_generate_scenario(fuzz_rng *rng, fuzz_scenario *sc, uint32_t max_ops) {
    uint32_t n, i;
    sc->seed = fuzz_rng_next(rng);
    n = 4u + fuzz_rng_u32(rng, max_ops > 4u ? max_ops - 4u : 1u);
    if (n > FUZZ_MAX_OPS) n = FUZZ_MAX_OPS;
    sc->op_count = n;
    sc->ops[0].kind = 0u; /* SpawnRegion */
    sc->ops[0].idx_a = 0u;
    sc->ops[0].idx_b = 0u;
    sc->ops[0].arg_u32 = 0u;
    sc->ops[0].arg_u64 = 0u;
    for (i = 1u; i < n; i++) {
        sc->ops[i].kind = fuzz_pick_op(rng);
        sc->ops[i].idx_a = fuzz_rng_u32(rng, FUZZ_MAX_REGIONS);
        sc->ops[i].idx_b = fuzz_rng_u32(rng, FUZZ_MAX_TASKS);
        sc->ops[i].arg_u32 = fuzz_rng_u32(rng, 32u);
        sc->ops[i].arg_u64 = fuzz_rng_next(rng) % 10000u;
    }
}

int main(int argc, char **argv) {
    fuzz_rng rng;
    uint32_t num_seeds = 100u;
    uint32_t initial_seed;

    if (argc > 1) { num_seeds = (uint32_t)atoi(argv[1]); }

    for (initial_seed = 0u; initial_seed < num_seeds; initial_seed++) {
        fuzz_scenario sc;
        uint32_t i;

        fuzz_rng_seed(&rng, (uint64_t)initial_seed);
        fuzz_generate_scenario(&rng, &sc, 64u);

        printf("{\"seed\":%llu,\"op_count\":%u,\"ops\":[", (unsigned long long)sc.seed,
               sc.op_count);

        for (i = 0u; i < sc.op_count; i++) {
            if (i > 0u) printf(",");
            printf("{\"op\":\"%s\",\"idx_a\":%u,\"idx_b\":%u,\"arg_u32\":%u,\"arg_u64\":%llu}",
                   fuzz_op_names[sc.ops[i].kind], sc.ops[i].idx_a, sc.ops[i].idx_b,
                   sc.ops[i].arg_u32, (unsigned long long)sc.ops[i].arg_u64);
        }
        printf("]}\n");
    }
    return 0;
}
