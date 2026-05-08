/*
 * test_memory_model_litmus.c — Memory-model litmus suite (bd-3vt.4)
 *
 * Verifies critical assumptions the asx runtime relies on across
 * compilers and optimization levels. Since the codebase is single-threaded
 * and synchronization-free, these tests focus on:
 *
 * 1. Type layout stability (sizes, alignment, endianness)
 * 2. Compiler optimization safety (observable side-effects preserved)
 * 3. Integer arithmetic assumptions (overflow, signedness, shifts)
 * 4. Struct packing and padding consistency
 * 5. Enum representation guarantees
 * 6. Function pointer call-through semantics
 *
 * These must pass on every compiler/target/optimization-level combination.
 *
 * SPDX-License-Identifier: MIT
 */

/* ASX_CHECKPOINT_WAIVER_FILE("Litmus test — memory model verification only") */

#include "../../test_harness.h"
#include <asx/asx.h>
#include <asx/platform/atomics.h>
#include <limits.h>
#include <string.h>

#define ASX_SEQLOCK_MAX_DATA 64u
#define ASX_EBR_EPOCH_COUNT 3u
#define ASX_EBR_MAX_READERS 16u
#define ASX_EBR_DEFER_CAPACITY 32u
#define ASX_EBR_INACTIVE UINT32_MAX
#define ASX_TASK_METADATA_NO_OWNER UINT16_MAX

typedef struct {
    asx_atomic_u32 sequence;
    uint8_t data[ASX_SEQLOCK_MAX_DATA];
    uint32_t data_size;
} asx_seqlock;

typedef struct {
    uint32_t state;
    uint16_t generation;
    int alive;
    uint32_t cancel_epoch;
    uint16_t lane;
    uint16_t owner_worker;
    uint64_t trace_sequence;
} asx_task_metadata;

typedef struct {
    uint32_t slot_index;
    uint32_t generation;
} asx_ebr_deferred_item;

typedef struct {
    asx_atomic_u32 global_epoch;
    asx_atomic_u32 reader_epoch[ASX_EBR_MAX_READERS];
    uint32_t reader_count;
    asx_ebr_deferred_item defer_ring[ASX_EBR_EPOCH_COUNT][ASX_EBR_DEFER_CAPACITY];
    uint32_t defer_count[ASX_EBR_EPOCH_COUNT];
    uint32_t total_deferred;
    uint32_t total_reclaimed;
    uint32_t epoch_advances;
} asx_ebr_state;

typedef void (*asx_ebr_reclaim_fn)(uint32_t slot_index, uint32_t generation, void *user_data);

typedef struct {
    asx_seqlock metadata;
    asx_ebr_state ebr;
    uint32_t slot_index;
    uint16_t current_generation;
    uint32_t stale_generation_rejects;
} asx_task_metadata_slot;

void asx_seqlock_init(asx_seqlock *sl, uint32_t data_size);
void asx_seqlock_write_begin(asx_seqlock *sl);
void asx_seqlock_write_end(asx_seqlock *sl);
void asx_seqlock_write(asx_seqlock *sl, const void *src, uint32_t size);
int asx_seqlock_read(const asx_seqlock *sl, void *out, uint32_t size);
uint32_t asx_seqlock_sequence(const asx_seqlock *sl);
void asx_task_metadata_init(asx_task_metadata *md, uint32_t state, uint16_t generation, int alive,
                            uint32_t cancel_epoch, uint16_t lane, uint16_t owner_worker,
                            uint64_t trace_sequence);
void asx_task_metadata_slot_init(asx_task_metadata_slot *slot, uint32_t slot_index,
                                 uint32_t reader_count);
int asx_task_metadata_slot_publish(asx_task_metadata_slot *slot, const asx_task_metadata *metadata);
uint32_t asx_task_metadata_slot_reader_enter(asx_task_metadata_slot *slot, uint32_t reader_id);
void asx_task_metadata_slot_reader_leave(asx_task_metadata_slot *slot, uint32_t reader_id);
int asx_task_metadata_slot_snapshot_in_epoch(asx_task_metadata_slot *slot, asx_task_metadata *out);
int asx_task_metadata_slot_retire(asx_task_metadata_slot *slot, uint16_t expected_generation);
int asx_task_metadata_slot_try_reclaim(asx_task_metadata_slot *slot, asx_ebr_reclaim_fn reclaim_fn,
                                       void *user_data);
uint32_t asx_ebr_pending_count(const asx_ebr_state *ebr);

/* -----------------------------------------------------------------------
 * LITMUS-1: Type sizes match ABI contract
 * Assumption: handle types are exactly 8 bytes, status is 4 bytes
 * ----------------------------------------------------------------------- */
TEST(litmus_type_sizes) {
    ASSERT_EQ((int)sizeof(asx_region_id), 8);
    ASSERT_EQ((int)sizeof(asx_task_id), 8);
    ASSERT_EQ((int)sizeof(asx_obligation_id), 8);
    ASSERT_EQ((int)sizeof(asx_timer_id), 8);
    ASSERT_EQ((int)sizeof(asx_channel_id), 8);
    ASSERT_EQ((int)sizeof(asx_status), 4);
    ASSERT_EQ((int)sizeof(asx_time), 8);
}

/* -----------------------------------------------------------------------
 * LITMUS-2: Unsigned integer overflow wraps (C99 guarantees this)
 * Assumption: unsigned arithmetic wraps modulo 2^N
 * ----------------------------------------------------------------------- */
TEST(litmus_unsigned_wrap) {
    uint32_t a = UINT32_MAX;
    uint32_t b = a + 1;
    ASSERT_EQ((int)b, 0);

    uint16_t c = UINT16_MAX;
    uint16_t d = (uint16_t)(c + 1);
    ASSERT_EQ((int)d, 0);

    uint64_t e = UINT64_MAX;
    uint64_t f = e + 1;
    ASSERT_TRUE(f == 0);
}

/* -----------------------------------------------------------------------
 * LITMUS-3: Handle packing is endian-agnostic via shift/mask
 * Assumption: bit-field packing via shifts produces portable results
 * ----------------------------------------------------------------------- */
TEST(litmus_handle_pack_unpack) {
    /* Pack: [type_tag:16][state_mask:16][gen:16][slot:16] */
    uint16_t type_tag = 0x1234;
    uint16_t state_mask = 0x5678;
    uint16_t gen = 0x9ABC;
    uint16_t slot = 0xDEF0;

    uint64_t packed = ((uint64_t)type_tag << 48) | ((uint64_t)state_mask << 32) |
                      ((uint64_t)gen << 16) | (uint64_t)slot;

    /* Unpack */
    uint16_t out_type = (uint16_t)(packed >> 48);
    uint16_t out_state = (uint16_t)(packed >> 32);
    uint16_t out_gen = (uint16_t)(packed >> 16);
    uint16_t out_slot = (uint16_t)(packed);

    ASSERT_EQ((int)out_type, (int)type_tag);
    ASSERT_EQ((int)out_state, (int)state_mask);
    ASSERT_EQ((int)out_gen, (int)gen);
    ASSERT_EQ((int)out_slot, (int)slot);
}

/* -----------------------------------------------------------------------
 * LITMUS-4: Enum representation is int-compatible
 * Assumption: enums fit in int, values match explicit assignments
 * ----------------------------------------------------------------------- */
TEST(litmus_enum_representation) {
    /* Region states are contiguous 0..4 */
    ASSERT_EQ((int)ASX_REGION_OPEN, 0);
    ASSERT_EQ((int)ASX_REGION_CLOSED, 4);
    ASSERT_TRUE(sizeof(asx_region_state) <= sizeof(int));

    /* Task states are contiguous 0..5 */
    ASSERT_EQ((int)ASX_TASK_CREATED, 0);
    ASSERT_EQ((int)ASX_TASK_COMPLETED, 5);
    ASSERT_TRUE(sizeof(asx_task_state) <= sizeof(int));

    /* Status codes have specific values */
    ASSERT_EQ((int)ASX_OK, 0);
    ASSERT_EQ((int)ASX_E_PENDING, 1);
    ASSERT_EQ((int)ASX_E_INVALID_ARGUMENT, 100);
    ASSERT_EQ((int)ASX_E_INVALID_TRANSITION, 200);
}

/* -----------------------------------------------------------------------
 * LITMUS-5: Signed/unsigned cast preserves bit pattern
 * Assumption: casting between signed and unsigned of same width
 * preserves bit pattern (C99 guarantees for two's complement)
 * ----------------------------------------------------------------------- */
TEST(litmus_signed_unsigned_cast) {
    int32_t neg = -1;
    uint32_t as_uint = (uint32_t)neg;
    ASSERT_EQ(as_uint, UINT32_MAX);

    uint32_t big = UINT32_MAX;
    int32_t as_int = (int32_t)big;
    ASSERT_EQ(as_int, -1);
}

/* -----------------------------------------------------------------------
 * LITMUS-6: memset zero produces valid zero-initialized structs
 * Assumption: memset(0) yields all-bits-zero which is a valid
 * representation for integers, pointers, and enums
 * ----------------------------------------------------------------------- */
TEST(litmus_memset_zero_init) {
    asx_budget b;
    memset(&b, 0, sizeof(b));
    ASSERT_EQ((int)b.poll_quota, 0);
    ASSERT_TRUE(b.cost_quota == 0);
    ASSERT_TRUE(b.deadline == 0);
    ASSERT_EQ((int)b.priority, 0);

    asx_outcome o;
    memset(&o, 0, sizeof(o));
    ASSERT_EQ((int)o.severity, (int)ASX_OUTCOME_OK);

    asx_cancel_reason r;
    memset(&r, 0, sizeof(r));
    ASSERT_EQ((int)r.kind, (int)ASX_CANCEL_USER);
}

/* -----------------------------------------------------------------------
 * LITMUS-7: Function pointer identity
 * Assumption: function pointers are comparable and non-NULL when
 * pointing to real functions
 * ----------------------------------------------------------------------- */
static asx_status dummy_poll(void *ud, asx_task_id self) {
    (void)ud;
    (void)self;
    return ASX_OK;
}

TEST(litmus_function_pointer_identity) {
    asx_status (*p1)(void *, asx_task_id) = dummy_poll;
    asx_status (*p2)(void *, asx_task_id) = dummy_poll;

    ASSERT_TRUE(p1 != NULL);
    ASSERT_TRUE(p1 == p2);

    asx_status (*null_fn)(void *, asx_task_id) = NULL;
    ASSERT_TRUE(null_fn == NULL);
}

/* -----------------------------------------------------------------------
 * LITMUS-8: Array indexing with enum values
 * Assumption: using enum values directly as array indices is safe
 * when values are contiguous and bounded
 * ----------------------------------------------------------------------- */
TEST(litmus_enum_as_array_index) {
    static const char *region_names[] = {"Open", "Closing", "Draining", "Finalizing", "Closed"};

    int i;
    for (i = (int)ASX_REGION_OPEN; i <= (int)ASX_REGION_CLOSED; i++) {
        ASSERT_TRUE(region_names[i] != NULL);
    }

    /* Verify contiguity */
    ASSERT_EQ((int)ASX_REGION_CLOSED - (int)ASX_REGION_OPEN, 4);
    ASSERT_EQ((int)ASX_TASK_COMPLETED - (int)ASX_TASK_CREATED, 5);
    ASSERT_EQ((int)ASX_OBLIGATION_LEAKED - (int)ASX_OBLIGATION_RESERVED, 3);
}

/* -----------------------------------------------------------------------
 * LITMUS-9: Struct size-field pattern for forward compatibility
 * Assumption: sizeof(struct) is stable per compilation unit
 * ----------------------------------------------------------------------- */
TEST(litmus_size_field_pattern) {
    asx_runtime_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.size = (uint32_t)sizeof(cfg);

    ASSERT_TRUE(cfg.size > 0);
    ASSERT_EQ(cfg.size, (uint32_t)sizeof(asx_runtime_config));

    /* Size is stable across multiple evaluations */
    ASSERT_EQ((uint32_t)sizeof(asx_runtime_config), (uint32_t)sizeof(asx_runtime_config));
}

/* -----------------------------------------------------------------------
 * LITMUS-10: Transition table lookup produces consistent results
 * Assumption: calling transition_check with same inputs always
 * returns the same result (no hidden state, no optimization clobber)
 * ----------------------------------------------------------------------- */
TEST(litmus_transition_determinism) {
    int i;
    /* Call the same transition check 100 times — must be identical */
    for (i = 0; i < 100; i++) {
        asx_status st1 = asx_region_transition_check(ASX_REGION_OPEN, ASX_REGION_CLOSING);
        asx_status st2 = asx_region_transition_check(ASX_REGION_CLOSED, ASX_REGION_OPEN);
        ASSERT_EQ(st1, ASX_OK);
        ASSERT_EQ(st2, ASX_E_INVALID_TRANSITION);
    }
}

/* -----------------------------------------------------------------------
 * LITMUS-11: NULL pointer is detectable
 * Assumption: NULL == 0 and is always false in boolean context
 * ----------------------------------------------------------------------- */
TEST(litmus_null_pointer) {
    void *p = NULL;
    ASSERT_TRUE(p == NULL);
    ASSERT_TRUE(!p);
    ASSERT_TRUE(p == 0);
}

/* -----------------------------------------------------------------------
 * LITMUS-12: Bitwise operations on uint64_t
 * Assumption: bitwise OR, AND, XOR, and shifts work correctly
 * on 64-bit types across all targets
 * ----------------------------------------------------------------------- */
TEST(litmus_bitwise_uint64) {
    uint64_t a = 0xDEADBEEFCAFEBABEull;
    uint64_t b = 0x1234567890ABCDEFull;

    /* OR */
    uint64_t or_result = a | b;
    ASSERT_TRUE((or_result & a) == a);
    ASSERT_TRUE((or_result & b) == b);

    /* AND */
    uint64_t and_result = a & b;
    ASSERT_TRUE((and_result | a) == a);

    /* XOR self is zero */
    ASSERT_TRUE((a ^ a) == 0);

    /* Shift roundtrip */
    uint64_t val = 0x42ull;
    ASSERT_TRUE((val << 32 >> 32) == val);
}

/* -----------------------------------------------------------------------
 * LITMUS-13: CHAR_BIT is 8
 * Assumption: the codebase assumes 8-bit bytes throughout
 * ----------------------------------------------------------------------- */
TEST(litmus_char_bit_is_8) {
    ASSERT_EQ(CHAR_BIT, 8);
    ASSERT_EQ((int)sizeof(uint8_t), 1);
    ASSERT_EQ((int)sizeof(uint16_t), 2);
    ASSERT_EQ((int)sizeof(uint32_t), 4);
    ASSERT_EQ((int)sizeof(uint64_t), 8);
}

/* -----------------------------------------------------------------------
 * LITMUS-14: Outcome join is stable across optimization levels
 * Assumption: compiler optimization doesn't change observable behavior
 * of the outcome join operation
 * ----------------------------------------------------------------------- */
TEST(litmus_outcome_join_stability) {
    asx_outcome a, b, result;
    int i;

    for (i = 0; i < 100; i++) {
        a.severity = ASX_OUTCOME_ERR;
        b.severity = ASX_OUTCOME_PANICKED;
        result = asx_outcome_join(&a, &b);
        ASSERT_EQ((int)result.severity, (int)ASX_OUTCOME_PANICKED);

        result = asx_outcome_join(&b, &a);
        ASSERT_EQ((int)result.severity, (int)ASX_OUTCOME_PANICKED);
    }
}

/* -----------------------------------------------------------------------
 * LITMUS-15: Cancel severity lookup is pure
 * Assumption: asx_cancel_severity() is a pure function with no side
 * effects — same input always gives same output
 * ----------------------------------------------------------------------- */
TEST(litmus_cancel_severity_purity) {
    int k, i;
    for (k = 0; k <= 10; k++) {
        int first = asx_cancel_severity((asx_cancel_kind)k);
        for (i = 0; i < 50; i++) {
            int again = asx_cancel_severity((asx_cancel_kind)k);
            ASSERT_EQ(first, again);
        }
    }
}

/* -----------------------------------------------------------------------
 * LITMUS-16: Atomic backend selection is explicit
 * Assumption: production atomics have one visible backend per build mode
 * ----------------------------------------------------------------------- */
TEST(litmus_atomic_backend_selection) {
#if ASX_LOCKFREE_SINGLE_THREAD
    ASSERT_EQ((int)asx_atomic_u32_backend(), (int)ASX_ATOMIC_BACKEND_SINGLE_THREAD);
    ASSERT_TRUE(asx_atomic_u32_is_single_threaded());
#else
    ASSERT_TRUE(asx_atomic_u32_backend() != ASX_ATOMIC_BACKEND_SINGLE_THREAD);
    ASSERT_FALSE(asx_atomic_u32_is_single_threaded());
#endif
}

/* -----------------------------------------------------------------------
 * LITMUS-17: Atomic load/store/init preserve values
 * Assumption: acquire loads observe values published by release stores
 * in the same deterministic sequence
 * ----------------------------------------------------------------------- */
TEST(litmus_atomic_load_store_init) {
    asx_atomic_u32 a;
    asx_atomic_u32_init(&a, 7u);
    ASSERT_EQ(asx_atomic_u32_load(&a), (uint32_t)7);

    asx_atomic_u32_store(&a, 42u);
    ASSERT_EQ(asx_atomic_u32_load(&a), (uint32_t)42);
}

/* -----------------------------------------------------------------------
 * LITMUS-18: Atomic compare-exchange has stable success/failure semantics
 * Assumption: failed compare-exchange reports the observed value and does
 * not mutate storage
 * ----------------------------------------------------------------------- */
TEST(litmus_atomic_compare_exchange_contract) {
    asx_atomic_u32 a;
    uint32_t expected;

    asx_atomic_u32_init(&a, 10u);
    expected = 10u;
    ASSERT_TRUE(asx_atomic_u32_compare_exchange(&a, &expected, 11u));
    ASSERT_EQ(expected, (uint32_t)10);
    ASSERT_EQ(asx_atomic_u32_load(&a), (uint32_t)11);

    expected = 10u;
    ASSERT_FALSE(asx_atomic_u32_compare_exchange(&a, &expected, 12u));
    ASSERT_EQ(expected, (uint32_t)11);
    ASSERT_EQ(asx_atomic_u32_load(&a), (uint32_t)11);

    ASSERT_FALSE(asx_atomic_u32_compare_exchange(&a, NULL, 13u));
    ASSERT_EQ(asx_atomic_u32_load(&a), (uint32_t)11);
}

/* -----------------------------------------------------------------------
 * LITMUS-19: Legacy CAS wrapper and exchange/fetch_add are linear
 * Assumption: each atomic read-modify-write returns the old value exactly
 * once in deterministic single-thread test execution
 * ----------------------------------------------------------------------- */
TEST(litmus_atomic_rmw_linearity) {
    asx_atomic_u32 a;
    uint32_t old;
    uint32_t i;

    asx_atomic_u32_init(&a, 0u);
    ASSERT_TRUE(asx_atomic_u32_cas(&a, 0u, 1u));
    ASSERT_FALSE(asx_atomic_u32_cas(&a, 0u, 2u));
    ASSERT_EQ(asx_atomic_u32_load(&a), (uint32_t)1);

    old = asx_atomic_u32_exchange(&a, 100u);
    ASSERT_EQ(old, (uint32_t)1);
    ASSERT_EQ(asx_atomic_u32_load(&a), (uint32_t)100);

    for (i = 0u; i < 16u; i++) {
        old = asx_atomic_u32_fetch_add(&a, 1u);
        ASSERT_EQ(old, (uint32_t)(100u + i));
    }
    ASSERT_EQ(asx_atomic_u32_load(&a), (uint32_t)116);
}

/* -----------------------------------------------------------------------
 * LITMUS-20: Atomic fences are callable no-op/ordering boundaries
 * Assumption: acquire/release fence calls compile and preserve surrounding
 * deterministic atomic observations
 * ----------------------------------------------------------------------- */
TEST(litmus_atomic_fences) {
    asx_atomic_u32 a;

    asx_atomic_u32_init(&a, 3u);
    asx_atomic_fence_release();
    asx_atomic_u32_store(&a, 4u);
    asx_atomic_fence_acquire();
    ASSERT_EQ(asx_atomic_u32_load(&a), (uint32_t)4);
}

/* -----------------------------------------------------------------------
 * LITMUS-21: Seqlock sequence publication remains even/monotone
 * Assumption: every completed metadata publication advances the seqlock
 * sequence by exactly two and leaves readers observing an even sequence.
 * ----------------------------------------------------------------------- */
TEST(litmus_seqlock_sequence_monotone) {
    asx_seqlock sl;
    asx_task_metadata md;
    asx_task_metadata snap;
    uint32_t i;

    asx_seqlock_init(&sl, sizeof(asx_task_metadata));
    ASSERT_EQ(asx_seqlock_sequence(&sl), (uint32_t)0);

    for (i = 0u; i < 8u; i++) {
        asx_task_metadata_init(&md, i, (uint16_t)(i + 1u), 1, i * 3u, (uint16_t)(i % 3u),
                               (uint16_t)(i % 4u), (uint64_t)i * 100u);
        asx_seqlock_write(&sl, &md, sizeof(md));
        ASSERT_EQ(asx_seqlock_sequence(&sl), (uint32_t)((i + 1u) * 2u));
        ASSERT_EQ(asx_seqlock_sequence(&sl) & 1u, (uint32_t)0);
        ASSERT_TRUE(asx_seqlock_read(&sl, &snap, sizeof(snap)));
        ASSERT_EQ(snap.trace_sequence, (uint64_t)i * 100u);
    }
}

static uint32_t litmus_reclaim_count = 0u;
static uint32_t litmus_reclaim_slot = UINT32_MAX;
static uint32_t litmus_reclaim_generation = UINT32_MAX;

static void litmus_reclaim_fn(uint32_t slot_index, uint32_t generation, void *user_data) {
    (void)user_data;
    litmus_reclaim_count++;
    litmus_reclaim_slot = slot_index;
    litmus_reclaim_generation = generation;
}

typedef struct {
    asx_atomic_u32 published;
    asx_task_metadata metadata;
} litmus_publication_cell;

typedef enum {
    LITMUS_QUEUE_EMPTY = 0,
    LITMUS_QUEUE_RESERVED = 1,
    LITMUS_QUEUE_COMMITTED = 2
} litmus_queue_slot_state;

static void litmus_runtime_reset(void) {
    asx_runtime_reset();
    asx_parallel_reset();
    asx_trace_reset();
}

static asx_parallel_config litmus_parallel_config(uint32_t worker_count,
                                                  asx_fairness_policy fairness) {
    asx_parallel_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.worker_count = worker_count;
    cfg.fairness = fairness;
    cfg.lane_weights[ASX_LANE_READY] = 1u;
    cfg.lane_weights[ASX_LANE_CANCEL] = 1u;
    cfg.lane_weights[ASX_LANE_TIMED] = 1u;
    cfg.starvation_limit = 3u;
    asx_parallel_admission_policy_init(&cfg.admission_policy);
    return cfg;
}

static asx_status litmus_pending_n(void *user_data, asx_task_id self) {
    int *remaining = (int *)user_data;
    (void)self;
    if (remaining != NULL && *remaining > 0) {
        (*remaining)--;
        return ASX_E_PENDING;
    }
    return ASX_OK;
}

static asx_status litmus_pending_forever(void *user_data, asx_task_id self) {
    (void)user_data;
    (void)self;
    return ASX_E_PENDING;
}

static uint32_t litmus_cancel_model_should_yield(uint32_t cancel_count, uint32_t ready_count,
                                                 uint32_t timed_count, uint32_t cancel_streak,
                                                 uint32_t cancel_streak_limit) {
    if (cancel_count == 0u) return 0u;
    if (cancel_streak_limit == 0u) return 0u;
    if (cancel_streak < cancel_streak_limit) return 0u;
    return (ready_count + timed_count) > 0u ? 1u : 0u;
}

/* -----------------------------------------------------------------------
 * LITMUS-22: EBR grace period blocks reclaim while a reader is active
 * Assumption: retirement is fail-closed until readers leave the epoch
 * containing the retired generation.
 * ----------------------------------------------------------------------- */
TEST(litmus_ebr_grace_period_blocks_reclaim) {
    asx_task_metadata_slot slot;
    asx_task_metadata md;
    asx_task_metadata snap;
    uint32_t epoch;

    litmus_reclaim_count = 0u;
    litmus_reclaim_slot = UINT32_MAX;
    litmus_reclaim_generation = UINT32_MAX;

    asx_task_metadata_slot_init(&slot, 33u, 2u);
    asx_task_metadata_init(&md, 1u, 9u, 1, 0u, 2u, 1u, 700u);
    ASSERT_TRUE(asx_task_metadata_slot_publish(&slot, &md));

    epoch = asx_task_metadata_slot_reader_enter(&slot, 0u);
    ASSERT_EQ(epoch, (uint32_t)0);
    ASSERT_TRUE(asx_task_metadata_slot_snapshot_in_epoch(&slot, &snap));
    ASSERT_EQ(snap.generation, (uint16_t)9);

    ASSERT_TRUE(asx_task_metadata_slot_retire(&slot, 9u));
    ASSERT_TRUE(asx_task_metadata_slot_try_reclaim(&slot, litmus_reclaim_fn, NULL));
    ASSERT_TRUE(asx_task_metadata_slot_try_reclaim(&slot, litmus_reclaim_fn, NULL));
    ASSERT_TRUE(!asx_task_metadata_slot_try_reclaim(&slot, litmus_reclaim_fn, NULL));
    ASSERT_EQ(litmus_reclaim_count, (uint32_t)0);

    asx_task_metadata_slot_reader_leave(&slot, 0u);
    ASSERT_TRUE(asx_task_metadata_slot_try_reclaim(&slot, litmus_reclaim_fn, NULL));
    ASSERT_EQ(litmus_reclaim_count, (uint32_t)1);
    ASSERT_EQ(litmus_reclaim_slot, (uint32_t)33);
    ASSERT_EQ(litmus_reclaim_generation, (uint32_t)9);
}

/* -----------------------------------------------------------------------
 * LITMUS-23: Release/acquire publication preserves metadata payload
 * Assumption: the portable atomic layer can publish a metadata block with
 * a release store and observe it through an acquire load before use.
 * ----------------------------------------------------------------------- */
TEST(litmus_atomic_publication_release_acquire) {
    litmus_publication_cell cell;
    asx_task_metadata observed;

    memset(&cell, 0, sizeof(cell));
    memset(&observed, 0, sizeof(observed));
    asx_atomic_u32_init(&cell.published, 0u);

    asx_task_metadata_init(&cell.metadata, 3u, 17u, 1, 11u, ASX_LANE_CANCEL, 5u, 9001u);
    asx_atomic_fence_release();
    asx_atomic_u32_store(&cell.published, 1u);

    ASSERT_EQ(asx_atomic_u32_load(&cell.published), 1u);
    asx_atomic_fence_acquire();
    observed = cell.metadata;

    ASSERT_EQ(observed.state, 3u);
    ASSERT_EQ(observed.generation, (uint16_t)17u);
    ASSERT_EQ(observed.alive, 1);
    ASSERT_EQ(observed.cancel_epoch, 11u);
    ASSERT_EQ(observed.lane, (uint16_t)ASX_LANE_CANCEL);
    ASSERT_EQ(observed.owner_worker, (uint16_t)5u);
    ASSERT_EQ(observed.trace_sequence, (uint64_t)9001u);
}

/* -----------------------------------------------------------------------
 * LITMUS-24: Queue slot ownership is single-owner and terminal
 * Assumption: lock-free two-phase queue slots can be reserved by exactly
 * one writer and then committed exactly once.
 * ----------------------------------------------------------------------- */
TEST(litmus_queue_slot_single_owner_cas) {
    asx_atomic_u32 state;
    uint32_t expected;

    asx_atomic_u32_init(&state, (uint32_t)LITMUS_QUEUE_EMPTY);

    expected = (uint32_t)LITMUS_QUEUE_EMPTY;
    ASSERT_TRUE(
        asx_atomic_u32_compare_exchange(&state, &expected, (uint32_t)LITMUS_QUEUE_RESERVED));
    ASSERT_EQ(expected, (uint32_t)LITMUS_QUEUE_EMPTY);

    expected = (uint32_t)LITMUS_QUEUE_EMPTY;
    ASSERT_FALSE(
        asx_atomic_u32_compare_exchange(&state, &expected, (uint32_t)LITMUS_QUEUE_RESERVED));
    ASSERT_EQ(expected, (uint32_t)LITMUS_QUEUE_RESERVED);
    ASSERT_EQ(asx_atomic_u32_load(&state), (uint32_t)LITMUS_QUEUE_RESERVED);

    expected = (uint32_t)LITMUS_QUEUE_RESERVED;
    ASSERT_TRUE(
        asx_atomic_u32_compare_exchange(&state, &expected, (uint32_t)LITMUS_QUEUE_COMMITTED));
    ASSERT_EQ(asx_atomic_u32_load(&state), (uint32_t)LITMUS_QUEUE_COMMITTED);

    expected = (uint32_t)LITMUS_QUEUE_RESERVED;
    ASSERT_FALSE(
        asx_atomic_u32_compare_exchange(&state, &expected, (uint32_t)LITMUS_QUEUE_COMMITTED));
    ASSERT_EQ(expected, (uint32_t)LITMUS_QUEUE_COMMITTED);
}

/* -----------------------------------------------------------------------
 * LITMUS-25: Seqlock readers reject in-progress publications
 * Assumption: readers never consume a torn metadata snapshot while the
 * sequence is odd, and accept the same payload after write completion.
 * ----------------------------------------------------------------------- */
TEST(litmus_seqlock_rejects_mid_write_snapshot) {
    asx_seqlock sl;
    asx_task_metadata md;
    asx_task_metadata snap;

    asx_seqlock_init(&sl, sizeof(asx_task_metadata));
    asx_task_metadata_init(&md, 4u, 21u, 1, 12u, ASX_LANE_TIMED, 7u, 12345u);

    asx_seqlock_write_begin(&sl);
    memcpy(sl.data, &md, sizeof(md));
    ASSERT_EQ(asx_seqlock_sequence(&sl) & 1u, 1u);
    ASSERT_FALSE(asx_seqlock_read(&sl, &snap, sizeof(snap)));

    asx_seqlock_write_end(&sl);
    ASSERT_EQ(asx_seqlock_sequence(&sl) & 1u, 0u);
    ASSERT_TRUE(asx_seqlock_read(&sl, &snap, sizeof(snap)));
    ASSERT_EQ(snap.generation, (uint16_t)21u);
    ASSERT_EQ(snap.trace_sequence, (uint64_t)12345u);
}

/* -----------------------------------------------------------------------
 * LITMUS-26: EBR rejects stale-generation retirement
 * Assumption: a retired slot must match the generation observed through
 * seqlock metadata before it can enter the EBR defer ring.
 * ----------------------------------------------------------------------- */
TEST(litmus_ebr_rejects_stale_generation_retire) {
    asx_task_metadata_slot slot;
    asx_task_metadata md;

    asx_task_metadata_slot_init(&slot, 44u, 2u);
    asx_task_metadata_init(&md, 1u, 4u, 1, 0u, ASX_LANE_READY, 0u, 77u);
    ASSERT_TRUE(asx_task_metadata_slot_publish(&slot, &md));

    ASSERT_FALSE(asx_task_metadata_slot_retire(&slot, 3u));
    ASSERT_EQ(slot.stale_generation_rejects, 1u);
    ASSERT_EQ(asx_ebr_pending_count(&slot.ebr), 0u);

    ASSERT_TRUE(asx_task_metadata_slot_retire(&slot, 4u));
    ASSERT_EQ(asx_ebr_pending_count(&slot.ebr), 1u);
}

/* -----------------------------------------------------------------------
 * LITMUS-27: Trace commit tickets are gap-free and monotone
 * Assumption: commit order can be represented by a single fetch-add ticket
 * stream; every worker observes a unique replay-stable sequence number.
 * ----------------------------------------------------------------------- */
TEST(litmus_trace_commit_fetch_add_order) {
    asx_atomic_u32 commit_ticket;
    uint32_t seen[8];
    uint32_t i;

    asx_atomic_u32_init(&commit_ticket, 0u);
    for (i = 0u; i < 8u; i++) {
        seen[i] = asx_atomic_u32_fetch_add(&commit_ticket, 1u);
        ASSERT_EQ(seen[i], i);
    }

    ASSERT_EQ(asx_atomic_u32_load(&commit_ticket), 8u);
}

/* -----------------------------------------------------------------------
 * LITMUS-28: Bounded work-steal model preserves ownership accounting
 * Assumption: deterministic stealing transfers at most one lane slot from
 * an owner to a thief and fails closed when no transferable work exists.
 * ----------------------------------------------------------------------- */
TEST(litmus_bounded_work_steal_model) {
    uint32_t owner_depth;
    uint32_t thief_depth;
    uint32_t attempts;
    uint32_t succeeded;
    uint32_t failed;
    uint32_t initial_owner;

    for (initial_owner = 0u; initial_owner <= 2u; initial_owner++) {
        owner_depth = initial_owner;
        thief_depth = 0u;
        attempts = 0u;
        succeeded = 0u;
        failed = 0u;

        attempts++;
        if (owner_depth > 0u) {
            owner_depth--;
            thief_depth++;
            succeeded++;
        } else {
            failed++;
        }

        ASSERT_EQ(owner_depth + thief_depth, initial_owner);
        ASSERT_EQ(succeeded + failed, attempts);
        ASSERT_TRUE(succeeded <= 1u);
        ASSERT_TRUE(failed <= 1u);
    }
}

/* -----------------------------------------------------------------------
 * LITMUS-29: Bounded cancel-lane fairness model yields to live work
 * Assumption: a saturated cancel streak yields when ready or timed work is
 * available, but never invents a yield when cancel is the only live lane.
 * ----------------------------------------------------------------------- */
TEST(litmus_bounded_cancel_fairness_model) {
    uint32_t cancel_count;
    uint32_t ready_count;
    uint32_t timed_count;
    uint32_t streak;
    uint32_t limit;

    for (cancel_count = 0u; cancel_count <= 2u; cancel_count++) {
        for (ready_count = 0u; ready_count <= 2u; ready_count++) {
            for (timed_count = 0u; timed_count <= 2u; timed_count++) {
                for (streak = 0u; streak <= 2u; streak++) {
                    for (limit = 0u; limit <= 2u; limit++) {
                        uint32_t should_yield =
                            litmus_cancel_model_should_yield(cancel_count, ready_count, timed_count,
                                                             streak, limit);
                        if (should_yield) {
                            ASSERT_TRUE(cancel_count > 0u);
                            ASSERT_TRUE(limit > 0u);
                            ASSERT_TRUE(streak >= limit);
                            ASSERT_TRUE((ready_count + timed_count) > 0u);
                        } else if (cancel_count > 0u && limit > 0u && streak >= limit) {
                            ASSERT_EQ(ready_count + timed_count, 0u);
                        }
                    }
                }
            }
        }
    }
}

/* -----------------------------------------------------------------------
 * LITMUS-30: Parallel commit sequence covers worker commits
 * Assumption: multi-worker execution preserves a single replay-stable
 * commit stream while permitting deterministic steal accounting.
 * ----------------------------------------------------------------------- */
TEST(litmus_parallel_commit_sequence_worker_sum) {
    asx_region_id rid;
    asx_task_id t1, t2, t3;
    asx_budget budget;
    asx_parallel_config cfg;
    asx_scheduling_metrics metrics;
    uint32_t worker_idx;
    uint32_t commit_sum = 0u;
    int c1 = 2;
    int c2 = 1;
    int c3 = 0;

    litmus_runtime_reset();
    cfg = litmus_parallel_config(4u, ASX_FAIRNESS_ROUND_ROBIN);
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, litmus_pending_n, &c1, &t1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, litmus_pending_n, &c2, &t2), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, litmus_pending_n, &c3, &t3), ASX_OK);

    budget = asx_budget_from_polls(100u);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_OK);
    ASSERT_EQ(asx_parallel_get_metrics(&metrics), ASX_OK);
    ASSERT_TRUE(metrics.commit_sequence > 0u);
    ASSERT_TRUE(metrics.steal_attempts > 0u);

    for (worker_idx = 0u; worker_idx < asx_parallel_worker_count(); worker_idx++) {
        asx_worker_state ws;
        ASSERT_EQ(asx_worker_get_state(worker_idx, &ws), ASX_OK);
        commit_sum += ws.commits_total;
        ASSERT_FALSE(ws.active);
        ASSERT_EQ((int)ws.lifecycle, (int)ASX_WORKER_DRAINED);
        if (ws.commits_total > 0u) {
            ASSERT_TRUE(ws.last_commit_sequence < metrics.commit_sequence);
        }
    }
    ASSERT_EQ(commit_sum, metrics.commit_sequence);
}

/* -----------------------------------------------------------------------
 * LITMUS-31: Budget exhaustion leaves workers in draining state
 * Assumption: shutdown/drain state is fail-closed under bounded budgets;
 * workers do not report drained while live work remains pending.
 * ----------------------------------------------------------------------- */
TEST(litmus_parallel_budget_exhaustion_draining) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    asx_parallel_config cfg;
    asx_worker_state ws;

    litmus_runtime_reset();
    cfg = litmus_parallel_config(2u, ASX_FAIRNESS_ROUND_ROBIN);
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, litmus_pending_forever, NULL, &tid), ASX_OK);

    budget = asx_budget_from_polls(1u);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_E_POLL_BUDGET_EXHAUSTED);

    ASSERT_EQ(asx_worker_get_state(0u, &ws), ASX_OK);
    ASSERT_TRUE(ws.active);
    ASSERT_EQ((int)ws.lifecycle, (int)ASX_WORKER_DRAINING);
}

/* --- Main --- */

int main(void) {
    fprintf(stderr, "[formal] memory-model litmus suite (bd-3vt.4)\n"
                    "[formal] verifying C99 assumptions across compiler/target\n");

    RUN_TEST(litmus_type_sizes);
    RUN_TEST(litmus_unsigned_wrap);
    RUN_TEST(litmus_handle_pack_unpack);
    RUN_TEST(litmus_enum_representation);
    RUN_TEST(litmus_signed_unsigned_cast);
    RUN_TEST(litmus_memset_zero_init);
    RUN_TEST(litmus_function_pointer_identity);
    RUN_TEST(litmus_enum_as_array_index);
    RUN_TEST(litmus_size_field_pattern);
    RUN_TEST(litmus_transition_determinism);
    RUN_TEST(litmus_null_pointer);
    RUN_TEST(litmus_bitwise_uint64);
    RUN_TEST(litmus_char_bit_is_8);
    RUN_TEST(litmus_outcome_join_stability);
    RUN_TEST(litmus_cancel_severity_purity);
    RUN_TEST(litmus_atomic_backend_selection);
    RUN_TEST(litmus_atomic_load_store_init);
    RUN_TEST(litmus_atomic_compare_exchange_contract);
    RUN_TEST(litmus_atomic_rmw_linearity);
    RUN_TEST(litmus_atomic_fences);
    RUN_TEST(litmus_seqlock_sequence_monotone);
    RUN_TEST(litmus_ebr_grace_period_blocks_reclaim);
    RUN_TEST(litmus_atomic_publication_release_acquire);
    RUN_TEST(litmus_queue_slot_single_owner_cas);
    RUN_TEST(litmus_seqlock_rejects_mid_write_snapshot);
    RUN_TEST(litmus_ebr_rejects_stale_generation_retire);
    RUN_TEST(litmus_trace_commit_fetch_add_order);
    RUN_TEST(litmus_bounded_work_steal_model);
    RUN_TEST(litmus_bounded_cancel_fairness_model);
    RUN_TEST(litmus_parallel_commit_sequence_worker_sum);
    RUN_TEST(litmus_parallel_budget_exhaustion_draining);

    TEST_REPORT();
    return test_failures;
}
