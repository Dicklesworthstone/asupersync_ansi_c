/*
 * generate_grammar_vectors.c — Generate grammar test vectors as JSON
 *
 * Standalone C program that generates fuzz scenarios from seeds 0-99
 * and outputs them as JSON for cross-language verification.
 *
 * Bead: bd-rkql.6
 */

#include <stdio.h>
#include <stdint.h>

#define FUZZ_MAX_OPS      128u
#define FUZZ_MAX_REGIONS  8u
#define FUZZ_MAX_TASKS    64u
#define FUZZ_OP_KIND_COUNT 21

/* PRNG: xoshiro256** (identical to fuzz_differential.c) */
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

/* Op types */
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

static const char *fuzz_op_names[] = {
    "SpawnRegion","CloseRegion","PoisonRegion","SpawnTask","CancelTask",
    "ReserveObligation","CommitObligation","AbortObligation",
    "ChannelCreate","ChannelReserve","ChannelSend","ChannelAbort",
    "ChannelRecv","ChannelCloseTx","ChannelCloseRx",
    "TimerRegister","TimerCancel","AdvanceTime",
    "SchedulerRun","RegionDrain","QuiescenceCheck"
};

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

    /* Hash seed (little-endian) */
    ptr = (uint8_t *)&sc->seed;
    for (i = 0u; i < 8u; i++) {
        hash ^= (uint64_t)ptr[i];
        hash *= 0x100000001b3ULL;
    }
    /* Hash op_count */
    ptr = (uint8_t *)&sc->op_count;
    for (i = 0u; i < 4u; i++) {
        hash ^= (uint64_t)ptr[i];
        hash *= 0x100000001b3ULL;
    }
    /* Hash each op's kind */
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

int main(void)
{
    fuzz_rng rng;
    uint32_t initial_seed;

    printf("[\n");
    for (initial_seed = 0u; initial_seed < 100u; initial_seed++) {
        fuzz_scenario sc;
        uint32_t i;

        fuzz_rng_seed(&rng, (uint64_t)initial_seed);
        fuzz_generate_scenario(&rng, &sc, 64u);

        if (initial_seed > 0u) printf(",\n");
        printf("  {\"initial_seed\":%u,\"scenario_seed\":%llu,\"op_count\":%u,\"grammar_hash\":\"%016llx\",\"first_op\":\"%s\",\"last_op\":\"%s\",\"ops\":[",
               initial_seed,
               (unsigned long long)sc.seed,
               sc.op_count,
               (unsigned long long)fnv1a_scenario(&sc),
               fuzz_op_names[(int)sc.ops[0].kind],
               fuzz_op_names[(int)sc.ops[sc.op_count - 1u].kind]);

        for (i = 0u; i < sc.op_count; i++) {
            if (i > 0u) printf(",");
            printf("{\"op\":\"%s\",\"a\":%u,\"b\":%u,\"u32\":%u,\"u64\":%llu}",
                   fuzz_op_names[(int)sc.ops[i].kind],
                   sc.ops[i].idx_a, sc.ops[i].idx_b,
                   sc.ops[i].arg_u32,
                   (unsigned long long)sc.ops[i].arg_u64);
        }
        printf("]}");
    }
    printf("\n]\n");
    return 0;
}
