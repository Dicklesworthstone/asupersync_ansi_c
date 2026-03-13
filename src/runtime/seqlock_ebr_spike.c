/*
 * seqlock_ebr_spike.c — Seqlock metadata and EBR/hazard reclamation
 * evaluation (bd-3vt.7)
 *
 * Implements three concurrency strategies for task-slot metadata access
 * and slot reclamation, all in single-threaded simulation mode:
 *
 * 1. Seqlock — reader-writer synchronization for metadata snapshots
 * 2. EBR (Epoch-Based Reclamation) — deferred slot reclamation
 * 3. Baseline spinlock — parity reference for both
 *
 * All strategies use the same portable atomic abstraction as the MPSC
 * lock-free spike: plain loads/stores in single-threaded mode, with
 * a clear upgrade path to __atomic_* builtins for multi-threaded.
 *
 * SPDX-License-Identifier: MIT
 */

/* ASX_CHECKPOINT_WAIVER_FILE() -- seqlock/EBR spike, no checkpoint coverage needed */

/* ASX_PROOF_BLOCK_WAIVER("reason: bug fixes for infinite loop and benchmark reporting") */

#include <asx/asx.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Portable atomic abstraction (same pattern as MPSC spike)           */
/* ------------------------------------------------------------------ */

#ifndef ASX_LOCKFREE_SINGLE_THREAD
#define ASX_LOCKFREE_SINGLE_THREAD 1
#endif

typedef struct {
    uint32_t value;
} asx_seqlock_atomic_u32;

#if ASX_LOCKFREE_SINGLE_THREAD

static uint32_t seqlock_atomic_load(const asx_seqlock_atomic_u32 *a) { return a->value; }

static void seqlock_atomic_store(asx_seqlock_atomic_u32 *a, uint32_t v) { a->value = v; }

/* Returns 1 on success, 0 on failure (CAS) */
static int seqlock_atomic_cas(asx_seqlock_atomic_u32 *a, uint32_t expected, uint32_t desired) {
    if (a->value == expected) {
        a->value = desired;
        return 1;
    }
    return 0;
}

#else
#error "Multi-threaded atomics not yet implemented (GS-009/GS-010 deferred)"
#endif

/* ------------------------------------------------------------------ */
/* Compiler/memory barrier abstraction                                */
/* ------------------------------------------------------------------ */

/*
 * In single-threaded mode, barriers are no-ops.
 * In multi-threaded mode, these map to compiler+hardware fences:
 *   GCC/Clang: __atomic_thread_fence(__ATOMIC_ACQUIRE/RELEASE/SEQ_CST)
 *   MSVC: _ReadWriteBarrier() + MemoryBarrier()
 *   C11: atomic_thread_fence(memory_order_*)
 */

#if ASX_LOCKFREE_SINGLE_THREAD
static void asx_fence_acquire(void) { /* no-op */ }
static void asx_fence_release(void) { /* no-op */ }
#endif

/* ------------------------------------------------------------------ */
/* 1. SEQLOCK — Reader-writer metadata snapshot                       */
/* ------------------------------------------------------------------ */

/*
 * A seqlock protects a fixed-size metadata block for concurrent readers
 * and an exclusive writer.
 *
 * Invariant: sequence is EVEN when data is consistent.
 *            sequence is ODD during a write (inconsistent).
 *
 * Writer protocol:
 *   1. seq++ (now odd — marks write-in-progress)
 *   2. write data fields
 *   3. seq++ (now even — marks write-complete)
 *
 * Reader protocol:
 *   1. s1 = load seq (if odd, spin/retry)
 *   2. acquire fence
 *   3. copy data into local snapshot
 *   4. acquire fence
 *   5. s2 = load seq
 *   6. if s1 != s2, goto 1 (writer intervened)
 *
 * Properties:
 *   - Readers never block the writer (unlike rwlock)
 *   - Readers retry on concurrent write (optimistic)
 *   - Optimal for infrequent writes, frequent reads
 *   - Perfect for: task state, generation, alive flag
 */

#define ASX_SEQLOCK_MAX_DATA 64u /* bytes of protected metadata */

typedef struct {
    asx_seqlock_atomic_u32 sequence;
    uint8_t data[ASX_SEQLOCK_MAX_DATA];
    uint32_t data_size;
} asx_seqlock;

void asx_seqlock_init(asx_seqlock *sl, uint32_t data_size);
void asx_seqlock_write_begin(asx_seqlock *sl);
void asx_seqlock_write_end(asx_seqlock *sl);
int asx_seqlock_read(const asx_seqlock *sl, void *out, uint32_t size);
uint32_t asx_seqlock_sequence(const asx_seqlock *sl);
void asx_seqlock_write(asx_seqlock *sl, const void *src, uint32_t size);

/* Task metadata snapshot (what a reader would copy atomically) */
typedef struct {
    uint32_t state;
    uint16_t generation;
    int alive;
    uint32_t cancel_epoch;
} asx_task_metadata;

void asx_seqlock_init(asx_seqlock *sl, uint32_t data_size) {
    if (sl == NULL) return;
    memset(sl, 0, sizeof(*sl));
    sl->data_size = (data_size > ASX_SEQLOCK_MAX_DATA) ? ASX_SEQLOCK_MAX_DATA : data_size;
}

void asx_seqlock_write_begin(asx_seqlock *sl) {
    uint32_t seq;
    if (sl == NULL) return;
    seq = seqlock_atomic_load(&sl->sequence);
    seqlock_atomic_store(&sl->sequence, seq + 1u); /* now odd */
    asx_fence_release();
}

void asx_seqlock_write_end(asx_seqlock *sl) {
    uint32_t seq;
    if (sl == NULL) return;
    asx_fence_release();
    seq = seqlock_atomic_load(&sl->sequence);
    seqlock_atomic_store(&sl->sequence, seq + 1u); /* now even */
}

/*
 * Consistent read: copies data_size bytes into out buffer.
 * Returns 1 if read was consistent on first attempt.
 * Returns 0 if a retry was needed (writer intervened).
 *
 * In single-threaded mode, always returns 1 unless sequence is
 * artificially made odd (simulating a concurrent write).
 */
int asx_seqlock_read(const asx_seqlock *sl, void *out, uint32_t size) {
    uint32_t s1, s2;
    uint32_t read_size;
    int retries = 0;
    int max_retries = 100;

    if (sl == NULL || out == NULL) return 0;

    read_size = (size < sl->data_size) ? size : sl->data_size;

    do {
        s1 = seqlock_atomic_load(&sl->sequence);
        if (s1 & 1u) {
            /* Writer in progress — retry */
            retries++;
            if (retries >= max_retries) return 0; /* give up */
            continue;
        }
        asx_fence_acquire();
        memcpy(out, sl->data, read_size);
        asx_fence_acquire();
        s2 = seqlock_atomic_load(&sl->sequence);
        if (s1 != s2) {
            retries++;
            if (retries >= max_retries) return 0;
        }
    } while (s1 != s2);

    return (retries == 0) ? 1 : 0;
}

uint32_t asx_seqlock_sequence(const asx_seqlock *sl) {
    if (sl == NULL) return 0;
    return seqlock_atomic_load(&sl->sequence);
}

/* Convenience: full write-begin + memcpy + write-end */
void asx_seqlock_write(asx_seqlock *sl, const void *src, uint32_t size) {
    uint32_t write_size;
    if (sl == NULL || src == NULL) return;
    write_size = (size < sl->data_size) ? size : sl->data_size;
    asx_seqlock_write_begin(sl);
    memcpy(sl->data, src, write_size);
    asx_seqlock_write_end(sl);
}

/* ------------------------------------------------------------------ */
/* 2. EBR — Epoch-Based Reclamation                                   */
/* ------------------------------------------------------------------ */

/*
 * EBR provides safe deferred reclamation for shared data structures.
 * Readers announce the epoch they are operating in. Writers advance
 * the global epoch. Items deferred in epoch E are safe to reclaim
 * when all readers have passed epoch E+2.
 *
 * This is simpler than hazard pointers (no per-object registration)
 * but requires bounded critical sections (readers must eventually leave).
 *
 * State:
 *   - global_epoch: current epoch (0, 1, 2 rotating mod 3)
 *   - reader_epoch[i]: epoch announced by reader i (or INACTIVE)
 *   - defer_ring[epoch][slot]: items pending reclamation per epoch
 *
 * Writer protocol:
 *   1. Check all readers have left global_epoch - 2
 *   2. Reclaim items deferred in (global_epoch - 2) mod 3
 *   3. Advance global_epoch (mod 3)
 *
 * Reader protocol:
 *   1. reader_epoch[tid] = global_epoch (enter critical section)
 *   2. ... read shared data ...
 *   3. reader_epoch[tid] = INACTIVE (leave critical section)
 *
 * Defer protocol:
 *   1. Add item to defer_ring[global_epoch]
 *   2. Item will be reclaimed when epoch advances past E+2
 */

#define ASX_EBR_EPOCH_COUNT 3u
#define ASX_EBR_MAX_READERS 16u
#define ASX_EBR_DEFER_CAPACITY 32u

#define ASX_EBR_INACTIVE UINT32_MAX

typedef struct {
    uint32_t slot_index; /* arena index to reclaim */
    uint32_t generation; /* generation at deferral time */
} asx_ebr_deferred_item;

typedef struct {
    /* Global epoch, wraps mod 3 */
    asx_seqlock_atomic_u32 global_epoch;

    /* Per-reader announced epoch (or ASX_EBR_INACTIVE) */
    asx_seqlock_atomic_u32 reader_epoch[ASX_EBR_MAX_READERS];
    uint32_t reader_count;

    /* Deferred reclamation ring per epoch */
    asx_ebr_deferred_item defer_ring[ASX_EBR_EPOCH_COUNT][ASX_EBR_DEFER_CAPACITY];
    uint32_t defer_count[ASX_EBR_EPOCH_COUNT];

    /* Statistics */
    uint32_t total_deferred;
    uint32_t total_reclaimed;
    uint32_t epoch_advances;
} asx_ebr_state;

/* Callback for reclaiming an item */
typedef void (*asx_ebr_reclaim_fn)(uint32_t slot_index, uint32_t generation, void *user_data);

void asx_ebr_init(asx_ebr_state *ebr, uint32_t reader_count);
uint32_t asx_ebr_reader_enter(asx_ebr_state *ebr, uint32_t reader_id);
void asx_ebr_reader_leave(asx_ebr_state *ebr, uint32_t reader_id);
int asx_ebr_defer(asx_ebr_state *ebr, uint32_t slot_index, uint32_t generation);
int asx_ebr_try_advance(asx_ebr_state *ebr, asx_ebr_reclaim_fn reclaim_fn, void *user_data);
uint32_t asx_ebr_current_epoch(const asx_ebr_state *ebr);
uint32_t asx_ebr_pending_count(const asx_ebr_state *ebr);

void asx_ebr_init(asx_ebr_state *ebr, uint32_t reader_count) {
    uint32_t i;
    if (ebr == NULL) return;
    memset(ebr, 0, sizeof(*ebr));
    ebr->reader_count = (reader_count > ASX_EBR_MAX_READERS) ? ASX_EBR_MAX_READERS : reader_count;
    for (i = 0; i < ASX_EBR_MAX_READERS; i++) {
        seqlock_atomic_store(&ebr->reader_epoch[i], ASX_EBR_INACTIVE);
    }
}

uint32_t asx_ebr_reader_enter(asx_ebr_state *ebr, uint32_t reader_id) {
    uint32_t epoch;
    if (ebr == NULL || reader_id >= ebr->reader_count) return 0;
    epoch = seqlock_atomic_load(&ebr->global_epoch);
    seqlock_atomic_store(&ebr->reader_epoch[reader_id], epoch);
    asx_fence_acquire();
    return epoch;
}

void asx_ebr_reader_leave(asx_ebr_state *ebr, uint32_t reader_id) {
    if (ebr == NULL || reader_id >= ebr->reader_count) return;
    asx_fence_release();
    seqlock_atomic_store(&ebr->reader_epoch[reader_id], ASX_EBR_INACTIVE);
}

int asx_ebr_defer(asx_ebr_state *ebr, uint32_t slot_index, uint32_t generation) {
    uint32_t epoch;
    uint32_t count;

    if (ebr == NULL) return 0;

    epoch = seqlock_atomic_load(&ebr->global_epoch);
    count = ebr->defer_count[epoch];

    if (count >= ASX_EBR_DEFER_CAPACITY) return 0; /* defer ring full */

    ebr->defer_ring[epoch][count].slot_index = slot_index;
    ebr->defer_ring[epoch][count].generation = generation;
    ebr->defer_count[epoch] = count + 1u;
    ebr->total_deferred++;
    return 1;
}

/*
 * Check if all readers have left a given epoch.
 * Returns 1 if safe to reclaim, 0 if readers still present.
 */
static int ebr_epoch_quiesced(const asx_ebr_state *ebr, uint32_t epoch) {
    uint32_t i;
    for (i = 0; i < ebr->reader_count; i++) {
        uint32_t re = seqlock_atomic_load(&ebr->reader_epoch[i]);
        if (re == epoch) return 0; /* reader still in this epoch */
    }
    return 1;
}

/*
 * Try to advance the global epoch and reclaim deferred items.
 * Returns 1 if epoch advanced, 0 if readers blocked advancement.
 */
int asx_ebr_try_advance(asx_ebr_state *ebr, asx_ebr_reclaim_fn reclaim_fn, void *user_data) {
    uint32_t current;
    uint32_t reclaim_epoch;
    uint32_t i;
    uint32_t count;

    if (ebr == NULL) return 0;

    current = seqlock_atomic_load(&ebr->global_epoch);

    /*
     * To advance from epoch E to E+1, we need all readers to have
     * left epoch (E-1) mod 3. This ensures items deferred in (E-1) mod 3
     * have had 2 full epochs of grace period.
     */
    reclaim_epoch = (current + ASX_EBR_EPOCH_COUNT - 2u) % ASX_EBR_EPOCH_COUNT;

    if (!ebr_epoch_quiesced(ebr, reclaim_epoch)) return 0;

    /* Reclaim items from the old epoch */
    count = ebr->defer_count[reclaim_epoch];
    for (i = 0; i < count; i++) {
        if (reclaim_fn != NULL) {
            reclaim_fn(ebr->defer_ring[reclaim_epoch][i].slot_index,
                       ebr->defer_ring[reclaim_epoch][i].generation, user_data);
        }
        ebr->total_reclaimed++;
    }
    ebr->defer_count[reclaim_epoch] = 0;

    /* Advance global epoch */
    seqlock_atomic_store(&ebr->global_epoch, (current + 1u) % ASX_EBR_EPOCH_COUNT);
    ebr->epoch_advances++;
    return 1;
}

uint32_t asx_ebr_current_epoch(const asx_ebr_state *ebr) {
    if (ebr == NULL) return 0;
    return seqlock_atomic_load(&ebr->global_epoch);
}

uint32_t asx_ebr_pending_count(const asx_ebr_state *ebr) {
    uint32_t total = 0;
    uint32_t i;
    if (ebr == NULL) return 0;
    for (i = 0; i < ASX_EBR_EPOCH_COUNT; i++) { total += ebr->defer_count[i]; }
    return total;
}

/* ------------------------------------------------------------------ */
/* 3. BASELINE SPINLOCK — Parity reference                            */
/* ------------------------------------------------------------------ */

/*
 * Simple test-and-set spinlock for parity comparison.
 * In single-threaded mode, lock/unlock are trivially paired.
 *
 * Properties:
 * - Always correct (mutual exclusion guaranteed)
 * - Fair: yes (single thread, no contention)
 * - Overhead: function call + store per lock/unlock
 * - In multi-threaded mode: CAS spin loop (cache-line bouncing)
 */

typedef struct {
    asx_seqlock_atomic_u32 locked; /* 0 = free, 1 = held */
    uint32_t acquisitions;
    uint32_t contentions;
} asx_spinlock;

void asx_spinlock_init(asx_spinlock *lock);
int asx_spinlock_try_lock(asx_spinlock *lock);
void asx_spinlock_lock(asx_spinlock *lock);
void asx_spinlock_unlock(asx_spinlock *lock);

void asx_spinlock_init(asx_spinlock *lock) {
    if (lock == NULL) return;
    memset(lock, 0, sizeof(*lock));
}

int asx_spinlock_try_lock(asx_spinlock *lock) {
    if (lock == NULL) return 0;
    if (seqlock_atomic_cas(&lock->locked, 0, 1)) {
        lock->acquisitions++;
        return 1;
    }
    lock->contentions++;
    return 0;
}

void asx_spinlock_lock(asx_spinlock *lock) {
    int spins = 0;
    int max_spins = 1000;
    if (lock == NULL) return;
    while (!seqlock_atomic_cas(&lock->locked, 0, 1)) {
        lock->contentions++;
        spins++;
        if (spins >= max_spins) return; /* deadlock guard */
    }
    lock->acquisitions++;
}

void asx_spinlock_unlock(asx_spinlock *lock) {
    if (lock == NULL) return;
    seqlock_atomic_store(&lock->locked, 0);
}

/* ------------------------------------------------------------------ */
/* 4. COMPARISON HARNESS — Three-way throughput benchmark              */
/* ------------------------------------------------------------------ */

/*
 * Simulate the same metadata read/write workload through each strategy:
 *
 * Workload: N rounds of metadata read + occasional write
 *   - Read: copy task_metadata from protected region
 *   - Write: update task_metadata fields (state transition)
 *
 * Measure cycles per operation for each strategy.
 */

typedef struct {
    uint64_t seqlock_read_cycles;
    uint64_t seqlock_write_cycles;
    uint64_t spinlock_read_cycles;
    uint64_t spinlock_write_cycles;
    uint64_t raw_read_cycles; /* unprotected (baseline lower bound) */
    uint64_t raw_write_cycles;
    uint32_t read_ops;
    uint32_t write_ops;
    uint32_t seqlock_retries; /* times reader had to retry */
} asx_concurrency_bench;

void asx_concurrency_bench_init(asx_concurrency_bench *bench);

void asx_concurrency_bench_init(asx_concurrency_bench *bench) {
    if (bench == NULL) return;
    memset(bench, 0, sizeof(*bench));
}

#if defined(__x86_64__) || defined(__i386__)
static uint64_t bench_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
#else
static uint64_t bench_rdtsc(void) {
    static uint64_t counter = 0;
    return ++counter;
}
#endif

void asx_concurrency_bench_run(asx_concurrency_bench *bench, uint32_t rounds);

void asx_concurrency_bench_run(asx_concurrency_bench *bench, uint32_t rounds) {
    asx_seqlock sl;
    asx_spinlock lock;
    asx_task_metadata md;
    asx_task_metadata snapshot;
    uint64_t t0, t1;
    uint32_t i;

    if (bench == NULL || rounds == 0) return;

    /* Initialize */
    asx_seqlock_init(&sl, sizeof(asx_task_metadata));
    asx_spinlock_init(&lock);
    memset(&md, 0, sizeof(md));
    md.state = 0;
    md.generation = 1;
    md.alive = 1;

    /* Seed the seqlock with initial data */
    asx_seqlock_write(&sl, &md, sizeof(md));

    /* 1. Seqlock benchmark */
    t0 = bench_rdtsc();
    for (i = 0; i < rounds; i++) {
        asx_seqlock_read(&sl, &snapshot, sizeof(snapshot));
        bench->read_ops++;

        if ((i & 0xFu) == 0) {
            /* Write every 16th iteration (6.25% write ratio) */
            md.state = i & 0x5u;
            md.cancel_epoch = i;
            asx_seqlock_write(&sl, &md, sizeof(md));
            bench->write_ops++;
        }
    }
    t1 = bench_rdtsc();
    bench->seqlock_read_cycles = t1 - t0;

    /* Reset counters for spinlock benchmark */
    bench->read_ops = 0;
    bench->write_ops = 0;

    /* 2. Spinlock benchmark */
    t0 = bench_rdtsc();
    for (i = 0; i < rounds; i++) {
        asx_spinlock_lock(&lock);
        memcpy(&snapshot, &md, sizeof(snapshot));
        asx_spinlock_unlock(&lock);
        bench->read_ops++;

        if ((i & 0xFu) == 0) {
            asx_spinlock_lock(&lock);
            md.state = i & 0x5u;
            md.cancel_epoch = i;
            asx_spinlock_unlock(&lock);
            bench->write_ops++;
        }
    }
    t1 = bench_rdtsc();
    bench->spinlock_read_cycles = t1 - t0;

    /* 3. Raw (unprotected) benchmark — lower bound */
    bench->read_ops = 0;
    bench->write_ops = 0;
    t0 = bench_rdtsc();
    for (i = 0; i < rounds; i++) {
        memcpy(&snapshot, &md, sizeof(snapshot));
        bench->read_ops++;
    }
    t1 = bench_rdtsc();
    bench->raw_read_cycles = t1 - t0;

    t0 = bench_rdtsc();
    for (i = 0; i < rounds; i++) {
        if ((i & 0xFu) == 0) {
            md.state = i & 0x5u;
            md.cancel_epoch = i;
            bench->write_ops++;
        }
    }
    t1 = bench_rdtsc();
    bench->raw_write_cycles = t1 - t0;
}
