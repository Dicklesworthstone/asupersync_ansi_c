/*
 * test_grammar_determinism.c — Regression tests for fuzz grammar determinism
 *
 * Verifies that the xoshiro256** PRNG and scenario generation produce
 * known-good outputs for reference seeds. If these tests fail, the grammar
 * has drifted and cross-language fuzz parity is broken.
 *
 * Bead: bd-rkql.6
 * SPDX-License-Identifier: MIT
 */

#include "../../test_log.h"
#include "../../test_harness.h"
#include <stdint.h>

/* ---- Inline PRNG (must match fuzz_differential.c exactly) ---- */

#define FUZZ_MAX_OPS      128u
#define FUZZ_MAX_REGIONS  8u
#define FUZZ_MAX_TASKS    64u
#define FUZZ_OP_KIND_COUNT 21

typedef struct { uint64_t s[4]; } fuzz_rng;

static uint64_t fuzz_rotl(uint64_t x, int k)
{
    return (x << k) | (x >> (64 - k));
}

static uint64_t fuzz_rng_next(fuzz_rng *rng)
{
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

static void fuzz_rng_seed(fuzz_rng *rng, uint64_t seed)
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

static uint32_t fuzz_rng_u32(fuzz_rng *rng, uint32_t bound)
{
    if (bound == 0u) return 0u;
    return (uint32_t)(fuzz_rng_next(rng) % (uint64_t)bound);
}

/* ---- Op types ---- */

typedef enum {
    FUZZ_OP_SPAWN_REGION=0, FUZZ_OP_CLOSE_REGION=1, FUZZ_OP_POISON_REGION=2,
    FUZZ_OP_SPAWN_TASK=3, FUZZ_OP_CANCEL_TASK=4,
    FUZZ_OP_RESERVE_OBLIGATION=5, FUZZ_OP_COMMIT_OBLIGATION=6, FUZZ_OP_ABORT_OBLIGATION=7,
    FUZZ_OP_CHANNEL_CREATE=8, FUZZ_OP_CHANNEL_RESERVE=9, FUZZ_OP_CHANNEL_SEND=10,
    FUZZ_OP_CHANNEL_ABORT=11, FUZZ_OP_CHANNEL_RECV=12, FUZZ_OP_CHANNEL_CLOSE_TX=13,
    FUZZ_OP_CHANNEL_CLOSE_RX=14, FUZZ_OP_TIMER_REGISTER=15, FUZZ_OP_TIMER_CANCEL=16,
    FUZZ_OP_ADVANCE_TIME=17, FUZZ_OP_SCHEDULER_RUN=18, FUZZ_OP_REGION_DRAIN=19,
    FUZZ_OP_QUIESCENCE_CHECK=20
} fuzz_op_kind;

static const uint32_t OP_WEIGHTS[FUZZ_OP_KIND_COUNT] = {
    12,8,3,15,10,8,7,5,6,5,5,3,5,3,3,6,4,5,12,5,4
};

typedef struct {
    fuzz_op_kind kind;
    uint32_t idx_a, idx_b, arg_u32;
    uint64_t arg_u64;
} fuzz_op;

typedef struct {
    uint64_t seed;
    uint32_t op_count;
    fuzz_op ops[FUZZ_MAX_OPS];
} fuzz_scenario;

static fuzz_op_kind fuzz_pick_op(fuzz_rng *rng)
{
    uint32_t total = 0u, r, acc;
    int i;
    for (i = 0; i < FUZZ_OP_KIND_COUNT; i++) total += OP_WEIGHTS[i];
    r = fuzz_rng_u32(rng, total);
    acc = 0u;
    for (i = 0; i < FUZZ_OP_KIND_COUNT; i++) {
        acc += OP_WEIGHTS[i];
        if (r < acc) return (fuzz_op_kind)i;
    }
    return FUZZ_OP_SPAWN_REGION;
}

static void fuzz_generate_scenario(fuzz_rng *rng, fuzz_scenario *sc, uint32_t max_ops)
{
    uint32_t n, i;
    sc->seed = fuzz_rng_next(rng);
    n = 4u + fuzz_rng_u32(rng, max_ops > 4u ? max_ops - 4u : 1u);
    if (n > FUZZ_MAX_OPS) n = FUZZ_MAX_OPS;
    sc->op_count = n;
    sc->ops[0].kind = FUZZ_OP_SPAWN_REGION;
    sc->ops[0].idx_a = 0u; sc->ops[0].idx_b = 0u;
    sc->ops[0].arg_u32 = 0u; sc->ops[0].arg_u64 = 0u;
    for (i = 1u; i < n; i++) {
        sc->ops[i].kind = fuzz_pick_op(rng);
        sc->ops[i].idx_a = fuzz_rng_u32(rng, FUZZ_MAX_REGIONS);
        sc->ops[i].idx_b = fuzz_rng_u32(rng, FUZZ_MAX_TASKS);
        sc->ops[i].arg_u32 = fuzz_rng_u32(rng, 32u);
        sc->ops[i].arg_u64 = fuzz_rng_next(rng) % 10000u;
    }
}

/* FNV-1a (same as harness) */
static uint64_t fnv1a_scenario(const fuzz_scenario *sc)
{
    uint64_t hash = 0xcbf29ce484222325ULL;
    uint32_t i;
    uint8_t *ptr;

    ptr = (uint8_t *)&sc->seed;
    for (i = 0u; i < 8u; i++) {
        hash ^= (uint64_t)ptr[i];
        hash *= 0x100000001b3ULL;
    }
    ptr = (uint8_t *)&sc->op_count;
    for (i = 0u; i < 4u; i++) {
        hash ^= (uint64_t)ptr[i];
        hash *= 0x100000001b3ULL;
    }
    for (i = 0u; i < sc->op_count; i++) {
        uint32_t k = (uint32_t)sc->ops[i].kind;
        ptr = (uint8_t *)&k;
        hash ^= (uint64_t)ptr[0]; hash *= 0x100000001b3ULL;
        hash ^= (uint64_t)ptr[1]; hash *= 0x100000001b3ULL;
        hash ^= (uint64_t)ptr[2]; hash *= 0x100000001b3ULL;
        hash ^= (uint64_t)ptr[3]; hash *= 0x100000001b3ULL;
    }
    return hash;
}

/* ================================================================
 * Tests — reference values from generate_grammar_vectors.c output
 * ================================================================ */

TEST(seed0_scenario_structure) {
    fuzz_rng rng;
    fuzz_scenario sc;

    fuzz_rng_seed(&rng, 0u);
    fuzz_generate_scenario(&rng, &sc, 64u);

    ASSERT_EQ(sc.seed, (uint64_t)6652065884787806945ULL);
    ASSERT_EQ(sc.op_count, (uint32_t)42u);
    ASSERT_EQ((int)sc.ops[0].kind, (int)FUZZ_OP_SPAWN_REGION);
    ASSERT_EQ((int)sc.ops[1].kind, (int)FUZZ_OP_TIMER_REGISTER);
    ASSERT_EQ(sc.ops[1].idx_a, (uint32_t)3u);
    ASSERT_EQ(sc.ops[1].idx_b, (uint32_t)50u);
    ASSERT_EQ(sc.ops[1].arg_u32, (uint32_t)22u);
    ASSERT_EQ(sc.ops[1].arg_u64, (uint64_t)7654u);
}

TEST(seed42_scenario_structure) {
    fuzz_rng rng;
    fuzz_scenario sc;

    fuzz_rng_seed(&rng, 42u);
    fuzz_generate_scenario(&rng, &sc, 64u);

    ASSERT_EQ(sc.seed, (uint64_t)3635898937931381181ULL);
    ASSERT_EQ(sc.op_count, (uint32_t)53u);
    ASSERT_EQ((int)sc.ops[1].kind, (int)FUZZ_OP_ABORT_OBLIGATION);
    ASSERT_EQ(sc.ops[1].idx_a, (uint32_t)2u);
    ASSERT_EQ(sc.ops[1].idx_b, (uint32_t)36u);
    ASSERT_EQ(sc.ops[2].idx_a, (uint32_t)6u);
    ASSERT_EQ((int)sc.ops[2].kind, (int)FUZZ_OP_CHANNEL_CLOSE_RX);
}

TEST(seed99_scenario_structure) {
    fuzz_rng rng;
    fuzz_scenario sc;

    fuzz_rng_seed(&rng, 99u);
    fuzz_generate_scenario(&rng, &sc, 64u);

    ASSERT_EQ(sc.seed, (uint64_t)7888305034174750639ULL);
    ASSERT_EQ(sc.op_count, (uint32_t)41u);
    ASSERT_EQ((int)sc.ops[1].kind, (int)FUZZ_OP_CANCEL_TASK);
    ASSERT_EQ(sc.ops[1].idx_a, (uint32_t)5u);
    ASSERT_EQ(sc.ops[1].arg_u32, (uint32_t)30u);
}

TEST(grammar_hash_seed0) {
    fuzz_rng rng;
    fuzz_scenario sc;

    fuzz_rng_seed(&rng, 0u);
    fuzz_generate_scenario(&rng, &sc, 64u);

    ASSERT_EQ(fnv1a_scenario(&sc), (uint64_t)0xaf54b3cd8c70264dULL);
}

TEST(grammar_hash_seed42) {
    fuzz_rng rng;
    fuzz_scenario sc;

    fuzz_rng_seed(&rng, 42u);
    fuzz_generate_scenario(&rng, &sc, 64u);

    ASSERT_EQ(fnv1a_scenario(&sc), (uint64_t)0xf452575e6f74e05bULL);
}

TEST(grammar_hash_seed99) {
    fuzz_rng rng;
    fuzz_scenario sc;

    fuzz_rng_seed(&rng, 99u);
    fuzz_generate_scenario(&rng, &sc, 64u);

    ASSERT_EQ(fnv1a_scenario(&sc), (uint64_t)0xda180f61c89281c3ULL);
}

TEST(determinism_100_seeds) {
    /* Run same generation twice for seeds 0-99, verify identical results */
    uint32_t seed_idx;
    for (seed_idx = 0u; seed_idx < 100u; seed_idx++) {
        fuzz_rng rng1, rng2;
        fuzz_scenario sc1, sc2;

        fuzz_rng_seed(&rng1, (uint64_t)seed_idx);
        fuzz_generate_scenario(&rng1, &sc1, 64u);

        fuzz_rng_seed(&rng2, (uint64_t)seed_idx);
        fuzz_generate_scenario(&rng2, &sc2, 64u);

        ASSERT_EQ(sc1.seed, sc2.seed);
        ASSERT_EQ(sc1.op_count, sc2.op_count);
        ASSERT_EQ(fnv1a_scenario(&sc1), fnv1a_scenario(&sc2));
    }
}

TEST(first_op_always_spawn_region) {
    uint32_t seed_idx;
    for (seed_idx = 0u; seed_idx < 100u; seed_idx++) {
        fuzz_rng rng;
        fuzz_scenario sc;

        fuzz_rng_seed(&rng, (uint64_t)seed_idx);
        fuzz_generate_scenario(&rng, &sc, 64u);

        ASSERT_EQ((int)sc.ops[0].kind, (int)FUZZ_OP_SPAWN_REGION);
        ASSERT_EQ(sc.ops[0].idx_a, (uint32_t)0u);
        ASSERT_EQ(sc.ops[0].idx_b, (uint32_t)0u);
        ASSERT_EQ(sc.ops[0].arg_u32, (uint32_t)0u);
        ASSERT_EQ(sc.ops[0].arg_u64, (uint64_t)0u);
    }
}

TEST(op_count_bounds) {
    uint32_t seed_idx;
    for (seed_idx = 0u; seed_idx < 100u; seed_idx++) {
        fuzz_rng rng;
        fuzz_scenario sc;

        fuzz_rng_seed(&rng, (uint64_t)seed_idx);
        fuzz_generate_scenario(&rng, &sc, 64u);

        ASSERT_TRUE(sc.op_count >= 4u);
        ASSERT_TRUE(sc.op_count <= 64u);
    }
}

TEST(idx_a_within_region_bound) {
    uint32_t seed_idx;
    for (seed_idx = 0u; seed_idx < 100u; seed_idx++) {
        fuzz_rng rng;
        fuzz_scenario sc;
        uint32_t i;

        fuzz_rng_seed(&rng, (uint64_t)seed_idx);
        fuzz_generate_scenario(&rng, &sc, 64u);

        for (i = 1u; i < sc.op_count; i++) {
            ASSERT_TRUE(sc.ops[i].idx_a < FUZZ_MAX_REGIONS);
        }
    }
}

TEST(idx_b_within_task_bound) {
    uint32_t seed_idx;
    for (seed_idx = 0u; seed_idx < 100u; seed_idx++) {
        fuzz_rng rng;
        fuzz_scenario sc;
        uint32_t i;

        fuzz_rng_seed(&rng, (uint64_t)seed_idx);
        fuzz_generate_scenario(&rng, &sc, 64u);

        for (i = 1u; i < sc.op_count; i++) {
            ASSERT_TRUE(sc.ops[i].idx_b < FUZZ_MAX_TASKS);
        }
    }
}

int main(void)
{
    RUN_TEST(seed0_scenario_structure);
    RUN_TEST(seed42_scenario_structure);
    RUN_TEST(seed99_scenario_structure);
    RUN_TEST(grammar_hash_seed0);
    RUN_TEST(grammar_hash_seed42);
    RUN_TEST(grammar_hash_seed99);
    RUN_TEST(determinism_100_seeds);
    RUN_TEST(first_op_always_spawn_region);
    RUN_TEST(op_count_bounds);
    RUN_TEST(idx_a_within_region_bound);
    RUN_TEST(idx_b_within_task_bound);
    TEST_REPORT();
    return test_failures;
}
