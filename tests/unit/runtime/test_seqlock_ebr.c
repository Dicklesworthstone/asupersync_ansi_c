/*
 * test_seqlock_ebr.c — Seqlock metadata, EBR reclamation, and spinlock
 * parity evaluation (bd-3vt.7)
 *
 * Validates:
 * - Seqlock: init, consistent read, write protocol, retry on torn read
 * - EBR: epoch lifecycle, defer/reclaim, quiescence detection, advance
 * - Spinlock: init, lock/unlock, try_lock, deadlock guard
 * - Comparison: three-way benchmark, correctness parity
 * - Determinism: identical sequences produce identical state
 *
 * SPDX-License-Identifier: MIT
 */

/* ASX_CHECKPOINT_WAIVER_FILE() -- seqlock/EBR test, no checkpoint coverage needed */

#include "test_harness.h"
#include <asx/asx.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Types from spike (not in public headers)                           */
/* ------------------------------------------------------------------ */

#define ASX_SEQLOCK_MAX_DATA 64u
#define ASX_EBR_EPOCH_COUNT 3u
#define ASX_EBR_MAX_READERS 16u
#define ASX_EBR_DEFER_CAPACITY 32u
#define ASX_EBR_INACTIVE UINT32_MAX
#define ASX_TASK_METADATA_NO_OWNER UINT16_MAX

typedef struct {
    uint32_t value;
} asx_seqlock_atomic_u32;

typedef struct {
    asx_seqlock_atomic_u32 sequence;
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
    asx_seqlock_atomic_u32 global_epoch;
    asx_seqlock_atomic_u32 reader_epoch[ASX_EBR_MAX_READERS];
    uint32_t reader_count;
    asx_ebr_deferred_item defer_ring[ASX_EBR_EPOCH_COUNT][ASX_EBR_DEFER_CAPACITY];
    uint32_t defer_count[ASX_EBR_EPOCH_COUNT];
    uint32_t total_deferred;
    uint32_t total_reclaimed;
    uint32_t epoch_advances;
} asx_ebr_state;

typedef void (*asx_ebr_reclaim_fn)(uint32_t slot_index, uint32_t generation, void *user_data);

typedef struct {
    asx_seqlock_atomic_u32 locked;
    uint32_t acquisitions;
    uint32_t contentions;
} asx_spinlock;

typedef struct {
    uint64_t seqlock_read_cycles;
    uint64_t seqlock_write_cycles;
    uint64_t spinlock_read_cycles;
    uint64_t spinlock_write_cycles;
    uint64_t raw_read_cycles;
    uint64_t raw_write_cycles;
    uint32_t read_ops;
    uint32_t write_ops;
    uint32_t seqlock_retries;
} asx_concurrency_bench;

typedef struct {
    asx_seqlock metadata;
    asx_ebr_state ebr;
    uint32_t slot_index;
    uint16_t current_generation;
    uint32_t stale_generation_rejects;
} asx_task_metadata_slot;

/* Forward declarations for spike functions */
void asx_seqlock_init(asx_seqlock *sl, uint32_t data_size);
void asx_seqlock_write_begin(asx_seqlock *sl);
void asx_seqlock_write_end(asx_seqlock *sl);
int asx_seqlock_read(const asx_seqlock *sl, void *out, uint32_t size);
uint32_t asx_seqlock_sequence(const asx_seqlock *sl);
void asx_seqlock_write(asx_seqlock *sl, const void *src, uint32_t size);

void asx_ebr_init(asx_ebr_state *ebr, uint32_t reader_count);
uint32_t asx_ebr_reader_enter(asx_ebr_state *ebr, uint32_t reader_id);
void asx_ebr_reader_leave(asx_ebr_state *ebr, uint32_t reader_id);
int asx_ebr_defer(asx_ebr_state *ebr, uint32_t slot_index, uint32_t generation);
int asx_ebr_try_advance(asx_ebr_state *ebr, asx_ebr_reclaim_fn reclaim_fn, void *user_data);
uint32_t asx_ebr_current_epoch(const asx_ebr_state *ebr);
uint32_t asx_ebr_pending_count(const asx_ebr_state *ebr);

void asx_spinlock_init(asx_spinlock *lock);
int asx_spinlock_try_lock(asx_spinlock *lock);
void asx_spinlock_lock(asx_spinlock *lock);
void asx_spinlock_unlock(asx_spinlock *lock);

void asx_concurrency_bench_init(asx_concurrency_bench *bench);
void asx_concurrency_bench_run(asx_concurrency_bench *bench, uint32_t rounds);
void asx_task_metadata_init(asx_task_metadata *md, uint32_t state, uint16_t generation, int alive,
                            uint32_t cancel_epoch, uint16_t lane, uint16_t owner_worker,
                            uint64_t trace_sequence);
void asx_task_metadata_slot_init(asx_task_metadata_slot *slot, uint32_t slot_index,
                                 uint32_t reader_count);
int asx_task_metadata_slot_publish(asx_task_metadata_slot *slot, const asx_task_metadata *metadata);
uint32_t asx_task_metadata_slot_reader_enter(asx_task_metadata_slot *slot, uint32_t reader_id);
void asx_task_metadata_slot_reader_leave(asx_task_metadata_slot *slot, uint32_t reader_id);
int asx_task_metadata_slot_snapshot_in_epoch(asx_task_metadata_slot *slot, asx_task_metadata *out);
int asx_task_metadata_slot_snapshot(asx_task_metadata_slot *slot, uint32_t reader_id,
                                    asx_task_metadata *out);
int asx_task_metadata_is_current(const asx_task_metadata *metadata, uint16_t expected_generation);
int asx_task_metadata_slot_retire(asx_task_metadata_slot *slot, uint16_t expected_generation);
int asx_task_metadata_slot_try_reclaim(asx_task_metadata_slot *slot, asx_ebr_reclaim_fn reclaim_fn,
                                       void *user_data);

/* ------------------------------------------------------------------ */
/* Test reclaim callback state                                        */
/* ------------------------------------------------------------------ */

static uint32_t reclaim_log[64];
static uint32_t reclaim_gen[64];
static uint32_t reclaim_count = 0;

static void test_reclaim_fn(uint32_t slot_index, uint32_t generation, void *user_data) {
    (void)user_data;
    if (reclaim_count < 64) {
        reclaim_log[reclaim_count] = slot_index;
        reclaim_gen[reclaim_count] = generation;
        reclaim_count++;
    }
}

static void reset_reclaim_log(void) {
    memset(reclaim_log, 0, sizeof(reclaim_log));
    memset(reclaim_gen, 0, sizeof(reclaim_gen));
    reclaim_count = 0;
}

/* ================================================================== */
/* SEQLOCK TESTS                                                      */
/* ================================================================== */

TEST(seqlock_init_zeros) {
    asx_seqlock sl;
    asx_seqlock_init(&sl, sizeof(asx_task_metadata));
    ASSERT_EQ(asx_seqlock_sequence(&sl), (uint32_t)0);
    ASSERT_EQ(sl.data_size, (uint32_t)sizeof(asx_task_metadata));
}

TEST(seqlock_init_clamps_size) {
    asx_seqlock sl;
    asx_seqlock_init(&sl, 1000);
    ASSERT_EQ(sl.data_size, (uint32_t)ASX_SEQLOCK_MAX_DATA);
}

TEST(seqlock_init_null_safety) { asx_seqlock_init(NULL, 8); /* should not crash */ }

TEST(seqlock_write_advances_sequence) {
    asx_seqlock sl;
    asx_task_metadata md = {1, 42, 1, 100, 2, 7, 9001u};

    asx_seqlock_init(&sl, sizeof(asx_task_metadata));
    ASSERT_EQ(asx_seqlock_sequence(&sl), (uint32_t)0);

    asx_seqlock_write(&sl, &md, sizeof(md));
    /* write_begin: 0→1, write_end: 1→2 */
    ASSERT_EQ(asx_seqlock_sequence(&sl), (uint32_t)2);
}

TEST(seqlock_consistent_read) {
    asx_seqlock sl;
    asx_task_metadata md = {3, 10, 1, 500, 1, 3, 123456u};
    asx_task_metadata snap;
    int consistent;

    asx_seqlock_init(&sl, sizeof(asx_task_metadata));
    asx_seqlock_write(&sl, &md, sizeof(md));

    memset(&snap, 0, sizeof(snap));
    consistent = asx_seqlock_read(&sl, &snap, sizeof(snap));

    ASSERT_TRUE(consistent);
    ASSERT_EQ(snap.state, (uint32_t)3);
    ASSERT_EQ(snap.generation, (uint16_t)10);
    ASSERT_EQ(snap.alive, 1);
    ASSERT_EQ(snap.cancel_epoch, (uint32_t)500);
    ASSERT_EQ(snap.lane, (uint16_t)1);
    ASSERT_EQ(snap.owner_worker, (uint16_t)3);
    ASSERT_EQ(snap.trace_sequence, (uint64_t)123456u);
}

TEST(seqlock_read_fails_during_write) {
    asx_seqlock sl;
    asx_task_metadata snap;
    int ok;

    asx_seqlock_init(&sl, sizeof(asx_task_metadata));

    /* Begin write but don't end — sequence is now odd */
    asx_seqlock_write_begin(&sl);
    ASSERT_EQ(asx_seqlock_sequence(&sl) & 1u, (uint32_t)1);

    /* Reader should fail (sequence is odd, simulates concurrent write) */
    memset(&snap, 0, sizeof(snap));
    ok = asx_seqlock_read(&sl, &snap, sizeof(snap));
    ASSERT_TRUE(!ok); /* could not get consistent read */
}

TEST(seqlock_multiple_writes) {
    asx_seqlock sl;
    asx_task_metadata md;
    asx_task_metadata snap;
    uint32_t i;

    asx_seqlock_init(&sl, sizeof(asx_task_metadata));

    for (i = 0; i < 20; i++) {
        md.state = i;
        md.generation = (uint16_t)(i + 1);
        md.alive = 1;
        md.cancel_epoch = i * 10;
        md.lane = (uint16_t)(i % 3u);
        md.owner_worker = (uint16_t)(i % 16u);
        md.trace_sequence = (uint64_t)i * 11u;
        asx_seqlock_write(&sl, &md, sizeof(md));
    }

    /* Sequence should be 2 * 20 = 40 (each write does +2) */
    ASSERT_EQ(asx_seqlock_sequence(&sl), (uint32_t)40);

    /* Last write should be readable */
    asx_seqlock_read(&sl, &snap, sizeof(snap));
    ASSERT_EQ(snap.state, (uint32_t)19);
    ASSERT_EQ(snap.generation, (uint16_t)20);
    ASSERT_EQ(snap.cancel_epoch, (uint32_t)190);
    ASSERT_EQ(snap.lane, (uint16_t)1);
    ASSERT_EQ(snap.owner_worker, (uint16_t)3);
    ASSERT_EQ(snap.trace_sequence, (uint64_t)209u);
}

TEST(seqlock_read_null_safety) {
    asx_seqlock sl;
    asx_task_metadata snap;

    asx_seqlock_init(&sl, sizeof(asx_task_metadata));
    ASSERT_TRUE(!asx_seqlock_read(NULL, &snap, sizeof(snap)));
    ASSERT_TRUE(!asx_seqlock_read(&sl, NULL, sizeof(snap)));
}

TEST(seqlock_write_null_safety) {
    asx_task_metadata md = {0, 0, 0, 0, 0, ASX_TASK_METADATA_NO_OWNER, 0};
    asx_seqlock sl;

    asx_seqlock_init(&sl, sizeof(asx_task_metadata));
    asx_seqlock_write(NULL, &md, sizeof(md)); /* should not crash */
    asx_seqlock_write(&sl, NULL, sizeof(md)); /* should not crash */
}

/* ================================================================== */
/* EBR TESTS                                                          */
/* ================================================================== */

TEST(ebr_init_clears_state) {
    asx_ebr_state ebr;
    asx_ebr_init(&ebr, 4);

    ASSERT_EQ(asx_ebr_current_epoch(&ebr), (uint32_t)0);
    ASSERT_EQ(ebr.reader_count, (uint32_t)4);
    ASSERT_EQ(ebr.total_deferred, (uint32_t)0);
    ASSERT_EQ(ebr.total_reclaimed, (uint32_t)0);
    ASSERT_EQ(asx_ebr_pending_count(&ebr), (uint32_t)0);
}

TEST(ebr_init_clamps_readers) {
    asx_ebr_state ebr;
    asx_ebr_init(&ebr, 1000);
    ASSERT_EQ(ebr.reader_count, (uint32_t)ASX_EBR_MAX_READERS);
}

TEST(ebr_init_null_safety) { asx_ebr_init(NULL, 4); /* should not crash */ }

TEST(ebr_reader_enter_leave) {
    asx_ebr_state ebr;
    uint32_t entered_epoch;

    asx_ebr_init(&ebr, 4);
    entered_epoch = asx_ebr_reader_enter(&ebr, 0);
    ASSERT_EQ(entered_epoch, (uint32_t)0);
    ASSERT_EQ(ebr.reader_epoch[0].value, (uint32_t)0);

    asx_ebr_reader_leave(&ebr, 0);
    ASSERT_EQ(ebr.reader_epoch[0].value, ASX_EBR_INACTIVE);
}

TEST(ebr_defer_tracks_items) {
    asx_ebr_state ebr;

    asx_ebr_init(&ebr, 2);
    ASSERT_TRUE(asx_ebr_defer(&ebr, 5, 10));
    ASSERT_TRUE(asx_ebr_defer(&ebr, 12, 20));

    ASSERT_EQ(asx_ebr_pending_count(&ebr), (uint32_t)2);
    ASSERT_EQ(ebr.total_deferred, (uint32_t)2);
}

TEST(ebr_defer_rejects_when_full) {
    asx_ebr_state ebr;
    uint32_t i;

    asx_ebr_init(&ebr, 2);

    /* Fill defer ring for epoch 0 */
    for (i = 0; i < ASX_EBR_DEFER_CAPACITY; i++) { ASSERT_TRUE(asx_ebr_defer(&ebr, i, i)); }

    /* 33rd should fail */
    ASSERT_TRUE(!asx_ebr_defer(&ebr, 99, 99));
}

TEST(ebr_advance_without_readers) {
    asx_ebr_state ebr;

    asx_ebr_init(&ebr, 2);
    /* No readers active — should advance freely */
    ASSERT_TRUE(asx_ebr_try_advance(&ebr, NULL, NULL));
    ASSERT_EQ(asx_ebr_current_epoch(&ebr), (uint32_t)1);
    ASSERT_EQ(ebr.epoch_advances, (uint32_t)1);
}

TEST(ebr_advance_blocked_by_reader) {
    asx_ebr_state ebr;

    asx_ebr_init(&ebr, 2);

    /* Advance to epoch 1 first (need a reader in epoch that blocks) */
    ASSERT_TRUE(asx_ebr_try_advance(&ebr, NULL, NULL)); /* 0 → 1 */
    ASSERT_TRUE(asx_ebr_try_advance(&ebr, NULL, NULL)); /* 1 → 2 */

    /* Reader enters epoch 0 (the reclaim epoch for advancing 2→0) */
    asx_ebr_reader_enter(&ebr, 0); /* reader enters at current epoch (2) */

    /*
     * The reclaim epoch for advancing from 2 is (2+3-2)%3 = 0.
     * Reader is in epoch 2, not 0, so advance should succeed.
     * Let's test the actual blocking case:
     */
    asx_ebr_reader_leave(&ebr, 0);

    /* Put reader in epoch 0 directly to test blocking */
    ebr.reader_epoch[0].value = 0;

    /* Now try advance: reclaim_epoch = (2+3-2)%3 = 0, reader in epoch 0 → blocked */
    ASSERT_TRUE(!asx_ebr_try_advance(&ebr, NULL, NULL));
}

TEST(ebr_reclaim_fires_callback) {
    asx_ebr_state ebr;

    reset_reclaim_log();
    asx_ebr_init(&ebr, 2);

    /* Defer items in epoch 0 */
    asx_ebr_defer(&ebr, 7, 100);
    asx_ebr_defer(&ebr, 13, 200);

    /* Advance three times to reclaim epoch 0 items:
     * epoch 0→1: reclaim_epoch = (0+3-2)%3 = 1, nothing to reclaim there
     * epoch 1→2: reclaim_epoch = (1+3-2)%3 = 2, nothing
     * epoch 2→0: reclaim_epoch = (2+3-2)%3 = 0, this reclaims our items!
     */
    asx_ebr_try_advance(&ebr, test_reclaim_fn, NULL); /* 0→1 */
    asx_ebr_try_advance(&ebr, test_reclaim_fn, NULL); /* 1→2 */
    asx_ebr_try_advance(&ebr, test_reclaim_fn, NULL); /* 2→0 */

    ASSERT_EQ(reclaim_count, (uint32_t)2);
    ASSERT_EQ(reclaim_log[0], (uint32_t)7);
    ASSERT_EQ(reclaim_gen[0], (uint32_t)100);
    ASSERT_EQ(reclaim_log[1], (uint32_t)13);
    ASSERT_EQ(reclaim_gen[1], (uint32_t)200);
    ASSERT_EQ(ebr.total_reclaimed, (uint32_t)2);
}

TEST(ebr_full_lifecycle) {
    asx_ebr_state ebr;
    uint32_t epoch;

    reset_reclaim_log();
    asx_ebr_init(&ebr, 4);

    /* Reader 0 enters */
    epoch = asx_ebr_reader_enter(&ebr, 0);
    ASSERT_EQ(epoch, (uint32_t)0);

    /* Writer defers a slot for reclamation */
    asx_ebr_defer(&ebr, 42, 5);

    /* Reader 0 leaves */
    asx_ebr_reader_leave(&ebr, 0);

    /* Advance through 3 epochs to reclaim */
    asx_ebr_try_advance(&ebr, test_reclaim_fn, NULL);
    asx_ebr_try_advance(&ebr, test_reclaim_fn, NULL);
    asx_ebr_try_advance(&ebr, test_reclaim_fn, NULL);

    /* Slot 42 should be reclaimed */
    ASSERT_EQ(reclaim_count, (uint32_t)1);
    ASSERT_EQ(reclaim_log[0], (uint32_t)42);
    ASSERT_EQ(reclaim_gen[0], (uint32_t)5);
}

TEST(ebr_null_safety) {
    asx_ebr_init(NULL, 4);
    ASSERT_EQ(asx_ebr_current_epoch(NULL), (uint32_t)0);
    ASSERT_EQ(asx_ebr_pending_count(NULL), (uint32_t)0);
    ASSERT_TRUE(!asx_ebr_defer(NULL, 0, 0));
    ASSERT_TRUE(!asx_ebr_try_advance(NULL, NULL, NULL));
    asx_ebr_reader_leave(NULL, 0); /* should not crash */
}

/* ================================================================== */
/* SPINLOCK TESTS                                                     */
/* ================================================================== */

TEST(spinlock_init_unlocked) {
    asx_spinlock lock;
    asx_spinlock_init(&lock);
    ASSERT_EQ(lock.locked.value, (uint32_t)0);
    ASSERT_EQ(lock.acquisitions, (uint32_t)0);
}

TEST(spinlock_lock_unlock) {
    asx_spinlock lock;
    asx_spinlock_init(&lock);

    asx_spinlock_lock(&lock);
    ASSERT_EQ(lock.locked.value, (uint32_t)1);
    ASSERT_EQ(lock.acquisitions, (uint32_t)1);

    asx_spinlock_unlock(&lock);
    ASSERT_EQ(lock.locked.value, (uint32_t)0);
}

TEST(spinlock_try_lock) {
    asx_spinlock lock;
    asx_spinlock_init(&lock);

    ASSERT_TRUE(asx_spinlock_try_lock(&lock));
    ASSERT_EQ(lock.locked.value, (uint32_t)1);

    /* Try again while held — should fail */
    ASSERT_TRUE(!asx_spinlock_try_lock(&lock));
    ASSERT_EQ(lock.contentions, (uint32_t)1);

    asx_spinlock_unlock(&lock);
    ASSERT_TRUE(asx_spinlock_try_lock(&lock));
}

TEST(spinlock_null_safety) {
    asx_spinlock_init(NULL); /* should not crash */
    asx_spinlock_lock(NULL);
    asx_spinlock_unlock(NULL);
    ASSERT_TRUE(!asx_spinlock_try_lock(NULL));
}

/* ================================================================== */
/* PARITY TESTS — Same result through all three strategies            */
/* ================================================================== */

TEST(parity_all_strategies_read_same_data) {
    asx_seqlock sl;
    asx_spinlock lock;
    asx_task_metadata md = {5, 99, 1, 12345, 2, 8, 0xABCDEFu};
    asx_task_metadata snap_seq, snap_spin, snap_raw;

    /* Seqlock path */
    asx_seqlock_init(&sl, sizeof(asx_task_metadata));
    asx_seqlock_write(&sl, &md, sizeof(md));
    asx_seqlock_read(&sl, &snap_seq, sizeof(snap_seq));

    /* Spinlock path */
    asx_spinlock_init(&lock);
    asx_spinlock_lock(&lock);
    memcpy(&snap_spin, &md, sizeof(snap_spin));
    asx_spinlock_unlock(&lock);

    /* Raw path */
    memcpy(&snap_raw, &md, sizeof(snap_raw));

    /* All three must agree */
    ASSERT_EQ(snap_seq.state, snap_spin.state);
    ASSERT_EQ(snap_seq.state, snap_raw.state);
    ASSERT_EQ(snap_seq.generation, snap_spin.generation);
    ASSERT_EQ(snap_seq.generation, snap_raw.generation);
    ASSERT_EQ(snap_seq.alive, snap_spin.alive);
    ASSERT_EQ(snap_seq.alive, snap_raw.alive);
    ASSERT_EQ(snap_seq.cancel_epoch, snap_spin.cancel_epoch);
    ASSERT_EQ(snap_seq.cancel_epoch, snap_raw.cancel_epoch);
    ASSERT_EQ(snap_seq.lane, snap_spin.lane);
    ASSERT_EQ(snap_seq.lane, snap_raw.lane);
    ASSERT_EQ(snap_seq.owner_worker, snap_spin.owner_worker);
    ASSERT_EQ(snap_seq.owner_worker, snap_raw.owner_worker);
    ASSERT_EQ(snap_seq.trace_sequence, snap_spin.trace_sequence);
    ASSERT_EQ(snap_seq.trace_sequence, snap_raw.trace_sequence);
}

TEST(parity_write_update_read_cycle) {
    asx_seqlock sl;
    asx_spinlock lock;
    asx_task_metadata md;
    asx_task_metadata snap_seq, snap_spin;
    uint32_t i;

    asx_seqlock_init(&sl, sizeof(asx_task_metadata));
    asx_spinlock_init(&lock);

    for (i = 0; i < 50; i++) {
        md.state = i % 6;
        md.generation = (uint16_t)(i + 1);
        md.alive = (i % 10 != 0) ? 1 : 0;
        md.cancel_epoch = i * 7;
        md.lane = (uint16_t)(i % 3u);
        md.owner_worker = (uint16_t)(i % 16u);
        md.trace_sequence = (uint64_t)i * 1000u;

        /* Write via both paths */
        asx_seqlock_write(&sl, &md, sizeof(md));

        /* Read back via both paths */
        asx_seqlock_read(&sl, &snap_seq, sizeof(snap_seq));
        asx_spinlock_lock(&lock);
        memcpy(&snap_spin, &md, sizeof(snap_spin));
        asx_spinlock_unlock(&lock);

        /* Must agree */
        ASSERT_EQ(snap_seq.state, snap_spin.state);
        ASSERT_EQ(snap_seq.generation, snap_spin.generation);
        ASSERT_EQ(snap_seq.alive, snap_spin.alive);
        ASSERT_EQ(snap_seq.cancel_epoch, snap_spin.cancel_epoch);
        ASSERT_EQ(snap_seq.lane, snap_spin.lane);
        ASSERT_EQ(snap_seq.owner_worker, snap_spin.owner_worker);
        ASSERT_EQ(snap_seq.trace_sequence, snap_spin.trace_sequence);
    }
}

/* ================================================================== */
/* BENCHMARK                                                          */
/* ================================================================== */

TEST(benchmark_runs_without_crash) {
    asx_concurrency_bench bench;
    asx_concurrency_bench_init(&bench);
    asx_concurrency_bench_run(&bench, 10000);

    /* All strategies should have processed operations */
    ASSERT_TRUE(bench.read_ops > 0);
    ASSERT_TRUE(bench.write_ops > 0);

    /* In single-threaded mode, seqlock should never need retries */
    ASSERT_EQ(bench.seqlock_retries, (uint32_t)0);
}

TEST(benchmark_cycles_nonzero) {
    asx_concurrency_bench bench;
    asx_concurrency_bench_init(&bench);
    asx_concurrency_bench_run(&bench, 1000);

    /*
     * All cycle counts should be > 0 on real hardware.
     * On non-x86, the counter is synthetic and still non-zero.
     */
    ASSERT_TRUE(bench.seqlock_read_cycles > 0);
    ASSERT_TRUE(bench.spinlock_read_cycles > 0);
    ASSERT_TRUE(bench.raw_read_cycles > 0);
}

/* ================================================================== */
/* DETERMINISM                                                        */
/* ================================================================== */

TEST(seqlock_deterministic_trajectory) {
    asx_seqlock s1, s2;
    asx_task_metadata md;
    asx_task_metadata snap1, snap2;
    uint32_t i;

    asx_seqlock_init(&s1, sizeof(asx_task_metadata));
    asx_seqlock_init(&s2, sizeof(asx_task_metadata));

    /* Identical write sequence on both */
    for (i = 0; i < 25; i++) {
        md.state = i % 6;
        md.generation = (uint16_t)i;
        md.alive = 1;
        md.cancel_epoch = i * 3;
        md.lane = (uint16_t)(i % 3u);
        md.owner_worker = (uint16_t)(i % 8u);
        md.trace_sequence = (uint64_t)i * 77u;

        asx_seqlock_write(&s1, &md, sizeof(md));
        asx_seqlock_write(&s2, &md, sizeof(md));
    }

    /* Identical reads */
    asx_seqlock_read(&s1, &snap1, sizeof(snap1));
    asx_seqlock_read(&s2, &snap2, sizeof(snap2));

    ASSERT_EQ(snap1.state, snap2.state);
    ASSERT_EQ(snap1.generation, snap2.generation);
    ASSERT_EQ(snap1.cancel_epoch, snap2.cancel_epoch);
    ASSERT_EQ(snap1.lane, snap2.lane);
    ASSERT_EQ(snap1.owner_worker, snap2.owner_worker);
    ASSERT_EQ(snap1.trace_sequence, snap2.trace_sequence);
    ASSERT_EQ(asx_seqlock_sequence(&s1), asx_seqlock_sequence(&s2));
}

TEST(ebr_deterministic_trajectory) {
    asx_ebr_state e1, e2;
    uint32_t i;

    reset_reclaim_log();
    asx_ebr_init(&e1, 4);
    asx_ebr_init(&e2, 4);

    /* Identical defer+advance sequence */
    for (i = 0; i < 5; i++) {
        asx_ebr_defer(&e1, i, i * 10);
        asx_ebr_defer(&e2, i, i * 10);
        asx_ebr_try_advance(&e1, NULL, NULL);
        asx_ebr_try_advance(&e2, NULL, NULL);
    }

    ASSERT_EQ(asx_ebr_current_epoch(&e1), asx_ebr_current_epoch(&e2));
    ASSERT_EQ(e1.total_deferred, e2.total_deferred);
    ASSERT_EQ(e1.total_reclaimed, e2.total_reclaimed);
    ASSERT_EQ(e1.epoch_advances, e2.epoch_advances);
    ASSERT_EQ(asx_ebr_pending_count(&e1), asx_ebr_pending_count(&e2));
}

/* ================================================================== */
/* TASK METADATA SLOT GRADUATION                                      */
/* ================================================================== */

TEST(task_metadata_init_covers_parallel_fields) {
    asx_task_metadata md;

    asx_task_metadata_init(&md, 4u, 12u, 1, 33u, 2u, 15u, 0x12345678u);

    ASSERT_EQ(md.state, (uint32_t)4);
    ASSERT_EQ(md.generation, (uint16_t)12);
    ASSERT_EQ(md.alive, 1);
    ASSERT_EQ(md.cancel_epoch, (uint32_t)33);
    ASSERT_EQ(md.lane, (uint16_t)2);
    ASSERT_EQ(md.owner_worker, (uint16_t)15);
    ASSERT_EQ(md.trace_sequence, (uint64_t)0x12345678u);
    ASSERT_TRUE(asx_task_metadata_is_current(&md, 12u));
    ASSERT_TRUE(!asx_task_metadata_is_current(&md, 11u));
}

TEST(task_metadata_slot_publish_snapshot_roundtrip) {
    asx_task_metadata_slot slot;
    asx_task_metadata md;
    asx_task_metadata snap;

    asx_task_metadata_slot_init(&slot, 9u, 4u);
    asx_task_metadata_init(&md, 2u, 3u, 1, 44u, 1u, 6u, 700u);

    ASSERT_TRUE(asx_task_metadata_slot_publish(&slot, &md));
    ASSERT_TRUE(asx_task_metadata_slot_snapshot(&slot, 0u, &snap));

    ASSERT_EQ(slot.slot_index, (uint32_t)9);
    ASSERT_EQ(slot.current_generation, (uint16_t)3);
    ASSERT_EQ(snap.state, (uint32_t)2);
    ASSERT_EQ(snap.generation, (uint16_t)3);
    ASSERT_EQ(snap.alive, 1);
    ASSERT_EQ(snap.cancel_epoch, (uint32_t)44);
    ASSERT_EQ(snap.lane, (uint16_t)1);
    ASSERT_EQ(snap.owner_worker, (uint16_t)6);
    ASSERT_EQ(snap.trace_sequence, (uint64_t)700u);
    ASSERT_EQ(slot.ebr.reader_epoch[0].value, ASX_EBR_INACTIVE);
}

TEST(task_metadata_slot_rejects_stale_generation_retire) {
    asx_task_metadata_slot slot;
    asx_task_metadata md;

    asx_task_metadata_slot_init(&slot, 4u, 2u);
    asx_task_metadata_init(&md, 1u, 7u, 1, 0u, 0u, 1u, 10u);
    ASSERT_TRUE(asx_task_metadata_slot_publish(&slot, &md));

    ASSERT_TRUE(!asx_task_metadata_slot_retire(&slot, 6u));
    ASSERT_EQ(slot.stale_generation_rejects, (uint32_t)1);
    ASSERT_EQ(asx_ebr_pending_count(&slot.ebr), (uint32_t)0);
}

TEST(task_metadata_slot_retire_defers_then_reclaims) {
    asx_task_metadata_slot slot;
    asx_task_metadata md;
    asx_task_metadata snap;

    reset_reclaim_log();
    asx_task_metadata_slot_init(&slot, 17u, 2u);
    asx_task_metadata_init(&md, 1u, 8u, 1, 2u, 2u, 5u, 99u);
    ASSERT_TRUE(asx_task_metadata_slot_publish(&slot, &md));

    ASSERT_TRUE(asx_task_metadata_slot_retire(&slot, 8u));
    ASSERT_TRUE(asx_task_metadata_slot_snapshot(&slot, 0u, &snap));
    ASSERT_EQ(snap.alive, 0);
    ASSERT_EQ(snap.owner_worker, (uint16_t)ASX_TASK_METADATA_NO_OWNER);
    ASSERT_EQ(asx_ebr_pending_count(&slot.ebr), (uint32_t)1);

    ASSERT_TRUE(asx_task_metadata_slot_try_reclaim(&slot, test_reclaim_fn, NULL));
    ASSERT_TRUE(asx_task_metadata_slot_try_reclaim(&slot, test_reclaim_fn, NULL));
    ASSERT_TRUE(asx_task_metadata_slot_try_reclaim(&slot, test_reclaim_fn, NULL));
    ASSERT_EQ(reclaim_count, (uint32_t)1);
    ASSERT_EQ(reclaim_log[0], (uint32_t)17);
    ASSERT_EQ(reclaim_gen[0], (uint32_t)8);
}

TEST(task_metadata_reader_blocks_reclaim_until_leave) {
    asx_task_metadata_slot slot;
    asx_task_metadata md;
    asx_task_metadata snap;
    uint32_t epoch;

    reset_reclaim_log();
    asx_task_metadata_slot_init(&slot, 21u, 2u);
    asx_task_metadata_init(&md, 1u, 4u, 1, 0u, 1u, 3u, 88u);
    ASSERT_TRUE(asx_task_metadata_slot_publish(&slot, &md));

    epoch = asx_task_metadata_slot_reader_enter(&slot, 0u);
    ASSERT_EQ(epoch, (uint32_t)0);
    ASSERT_TRUE(asx_task_metadata_slot_snapshot_in_epoch(&slot, &snap));
    ASSERT_EQ(snap.generation, (uint16_t)4);

    ASSERT_TRUE(asx_task_metadata_slot_retire(&slot, 4u));
    ASSERT_TRUE(asx_task_metadata_slot_try_reclaim(&slot, test_reclaim_fn, NULL));
    ASSERT_TRUE(asx_task_metadata_slot_try_reclaim(&slot, test_reclaim_fn, NULL));
    ASSERT_TRUE(!asx_task_metadata_slot_try_reclaim(&slot, test_reclaim_fn, NULL));
    ASSERT_EQ(reclaim_count, (uint32_t)0);

    asx_task_metadata_slot_reader_leave(&slot, 0u);
    ASSERT_TRUE(asx_task_metadata_slot_try_reclaim(&slot, test_reclaim_fn, NULL));
    ASSERT_EQ(reclaim_count, (uint32_t)1);
    ASSERT_EQ(reclaim_log[0], (uint32_t)21);
    ASSERT_EQ(reclaim_gen[0], (uint32_t)4);
}

/* ================================================================== */
/* INTEGRATION: Seqlock + EBR workflow                                */
/* ================================================================== */

TEST(integration_seqlock_protects_slot_during_ebr_reclaim) {
    asx_seqlock sl;
    asx_ebr_state ebr;
    asx_task_metadata md;
    asx_task_metadata snap;
    uint32_t reader_epoch;

    reset_reclaim_log();
    asx_seqlock_init(&sl, sizeof(asx_task_metadata));
    asx_ebr_init(&ebr, 2);

    /* Writer creates slot with generation 1 */
    md.state = 1; /* ASX_TASK_RUNNING */
    md.generation = 1;
    md.alive = 1;
    md.cancel_epoch = 0;
    md.lane = 2;
    md.owner_worker = 9;
    md.trace_sequence = 77u;
    asx_seqlock_write(&sl, &md, sizeof(md));

    /* Reader enters EBR epoch, reads metadata via seqlock */
    reader_epoch = asx_ebr_reader_enter(&ebr, 0);
    asx_seqlock_read(&sl, &snap, sizeof(snap));
    ASSERT_EQ(snap.alive, 1);
    ASSERT_EQ(snap.generation, (uint16_t)1);
    ASSERT_EQ(snap.lane, (uint16_t)2);
    ASSERT_EQ(snap.owner_worker, (uint16_t)9);
    ASSERT_EQ(snap.trace_sequence, (uint64_t)77u);

    /* Writer "frees" the slot: marks dead, defers for reclamation */
    md.state = 5; /* ASX_TASK_COMPLETED */
    md.alive = 0;
    asx_seqlock_write(&sl, &md, sizeof(md));
    asx_ebr_defer(&ebr, 0, 1); /* slot 0, gen 1 */

    /* Reader leaves EBR */
    asx_ebr_reader_leave(&ebr, 0);

    /* Advance epochs to reclaim */
    asx_ebr_try_advance(&ebr, test_reclaim_fn, NULL);
    asx_ebr_try_advance(&ebr, test_reclaim_fn, NULL);
    asx_ebr_try_advance(&ebr, test_reclaim_fn, NULL);

    /* Slot should be reclaimed */
    ASSERT_EQ(reclaim_count, (uint32_t)1);
    ASSERT_EQ(reclaim_log[0], (uint32_t)0);
    ASSERT_EQ(reclaim_gen[0], (uint32_t)1);

    /* Suppress unused variable warning */
    (void)reader_epoch;
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */

int main(void) {
    fprintf(stderr, "=== seqlock/EBR/spinlock tests (bd-3vt.7) ===\n");

    /* Seqlock (9) */
    RUN_TEST(seqlock_init_zeros);
    RUN_TEST(seqlock_init_clamps_size);
    RUN_TEST(seqlock_init_null_safety);
    RUN_TEST(seqlock_write_advances_sequence);
    RUN_TEST(seqlock_consistent_read);
    RUN_TEST(seqlock_read_fails_during_write);
    RUN_TEST(seqlock_multiple_writes);
    RUN_TEST(seqlock_read_null_safety);
    RUN_TEST(seqlock_write_null_safety);

    /* EBR (10) */
    RUN_TEST(ebr_init_clears_state);
    RUN_TEST(ebr_init_clamps_readers);
    RUN_TEST(ebr_init_null_safety);
    RUN_TEST(ebr_reader_enter_leave);
    RUN_TEST(ebr_defer_tracks_items);
    RUN_TEST(ebr_defer_rejects_when_full);
    RUN_TEST(ebr_advance_without_readers);
    RUN_TEST(ebr_advance_blocked_by_reader);
    RUN_TEST(ebr_reclaim_fires_callback);
    RUN_TEST(ebr_full_lifecycle);
    RUN_TEST(ebr_null_safety);

    /* Spinlock (4) */
    RUN_TEST(spinlock_init_unlocked);
    RUN_TEST(spinlock_lock_unlock);
    RUN_TEST(spinlock_try_lock);
    RUN_TEST(spinlock_null_safety);

    /* Parity (2) */
    RUN_TEST(parity_all_strategies_read_same_data);
    RUN_TEST(parity_write_update_read_cycle);

    /* Benchmark (2) */
    RUN_TEST(benchmark_runs_without_crash);
    RUN_TEST(benchmark_cycles_nonzero);

    /* Determinism (2) */
    RUN_TEST(seqlock_deterministic_trajectory);
    RUN_TEST(ebr_deterministic_trajectory);

    /* Task metadata slot graduation (5) */
    RUN_TEST(task_metadata_init_covers_parallel_fields);
    RUN_TEST(task_metadata_slot_publish_snapshot_roundtrip);
    RUN_TEST(task_metadata_slot_rejects_stale_generation_retire);
    RUN_TEST(task_metadata_slot_retire_defers_then_reclaims);
    RUN_TEST(task_metadata_reader_blocks_reclaim_until_leave);

    /* Integration (1) */
    RUN_TEST(integration_seqlock_protects_slot_during_ebr_reclaim);

    TEST_REPORT();
    return test_failures;
}
