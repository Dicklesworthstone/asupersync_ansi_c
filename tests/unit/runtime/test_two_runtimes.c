/*
 * test_two_runtimes.c — two runtimes live at once (K18, bd-9kll.3.20)
 *
 * The second runtime brings its own storage block (asx_runtime_config.storage)
 * and takes registry tag 1. Their regions, tasks and obligations are each
 * their own: the same slot and generation, told apart by the tag a handle
 * carries; a handle of one is refused by the other with NOT_FOUND; each
 * schedules and quiesces without the other. A runtime beyond the first is
 * accepted only while no still process-wide subsystem holds state, and the
 * subsystems that are refuse, by name, to acquire state while two runtimes
 * are live.
 *
 * It needs ASX_MAX_RUNTIMES >= 2 and the deterministic build (a live build
 * switches the network, file-system, process and signal adapters to their
 * native backends, which one runtime owns): the capacity lane's x4 build
 * runs it. Elsewhere it is reported as SKIP, not as passed. The hook-table
 * comparison a second runtime is admitted by needs no second runtime, and
 * runs in every build.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../../src/runtime/runtime_internal.h"
#include "../../test_harness.h"
#include <asx/asx.h>
#include <stdlib.h>
#include <string.h>

#if ASX_MAX_RUNTIMES >= 2u && ASX_DETERMINISTIC
#define TWO_RUNTIMES_RUN 1
#else
#define TWO_RUNTIMES_RUN 0
#endif

/* The members of a table, leaving its padding bytes as they were. */
static void hooks_copy_members(asx_runtime_hooks *dst, const asx_runtime_hooks *src) {
    dst->allocator = src->allocator;
    dst->clock = src->clock;
    dst->entropy = src->entropy;
    dst->reactor = src->reactor;
    dst->blocking = src->blocking;
    dst->log = src->log;
    dst->deterministic_seeded_prng = src->deterministic_seeded_prng;
    dst->allocator_sealed = src->allocator_sealed;
}

TEST(hook_tables_with_equal_members_match_whatever_their_padding_bytes) {
    asx_runtime_hooks base;
    asx_runtime_hooks first;
    asx_runtime_hooks second;

    asx_runtime_hooks_init(&base);
    memset(&first, 0xA5, sizeof(first));
    memset(&second, 0, sizeof(second));
    hooks_copy_members(&first, &base);
    hooks_copy_members(&second, &base);
    ASSERT_EQ(asx_runtime_set_hooks(&first), ASX_OK);

    /* Same members, different padding: the same table. */
    ASSERT_TRUE(asx_runtime_hooks_match_installed_internal(&second));

    /* The allocator seal is state of the installed copy, not what is asked. */
    second.allocator_sealed = (uint8_t)(base.allocator_sealed ? 0u : 1u);
    ASSERT_TRUE(asx_runtime_hooks_match_installed_internal(&second));

    /* One member that differs, in the middle or at the end, is another table. */
    second.log.ctx = &second;
    ASSERT_FALSE(asx_runtime_hooks_match_installed_internal(&second));
    second.log.ctx = base.log.ctx;
    ASSERT_TRUE(asx_runtime_hooks_match_installed_internal(&second));
    second.deterministic_seeded_prng = (uint8_t)(base.deterministic_seeded_prng ? 0u : 1u);
    ASSERT_FALSE(asx_runtime_hooks_match_installed_internal(&second));
}

#if TWO_RUNTIMES_RUN

static asx_status done_poll(void *user_data, asx_task_id self) {
    (void)user_data;
    (void)self;
    return ASX_OK;
}

static asx_status pool_make_one(void *factory_data, void **out) {
    static int resource;
    (void)factory_data;
    *out = &resource;
    return ASX_OK;
}

/* An aligned block of asx_runtime_storage_size() bytes. */
typedef struct {
    unsigned char *raw;
    unsigned char *block;
    size_t size;
} two_block;

static int two_block_make(two_block *b) {
    size_t size = asx_runtime_storage_size();
    size_t align = asx_runtime_storage_align();
    uintptr_t p;

    memset(b, 0, sizeof(*b));
    b->raw = (unsigned char *)malloc(size + align);
    if (b->raw == NULL) return 0;
    p = ((uintptr_t)b->raw + (align - 1u)) & ~((uintptr_t)align - 1u);
    b->block = (unsigned char *)p;
    b->size = size;
    return 1;
}

static void two_block_free(two_block *b) { free(b->raw); }

static int reason_names(const char *what) {
    return strstr(asx_runtime_last_refusal_reason(), what) != NULL;
}

TEST(two_runtimes_have_independent_regions_tasks_and_quiescence) {
    asx_runtime a;
    asx_runtime b;
    asx_runtime_config cfg_a;
    asx_runtime_config cfg_b;
    asx_runtime_hooks hooks;
    two_block blk;
    asx_region_id ra;
    asx_region_id rb;
    asx_task_id ta;
    asx_task_id tb;
    asx_obligation_id ob;
    asx_task_state state;
    asx_budget run;
    asx_waker wa;
    asx_waker wb;
    asx_task_id drained[4];
    asx_timer_wheel *wheel_a;
    asx_timer_wheel *wheel_b;
    asx_timer_handle th;
    asx_deadline dl;
    asx_wait_queue wq;
    uint32_t wid;
    asx_semaphore_handle sa;
    asx_semaphore_handle sb;
    asx_semaphore_permit pb;
    asx_mutex_handle mb;
    asx_mutex_guard gb;
    asx_rwlock_handle rwa;
    asx_rwlock_handle rwb;
    asx_rwlock_write_guard wg;
    asx_notify_handle nb;
    asx_barrier_handle bb;
    asx_once_handle once_b;
    asx_pool_config pcfg;
    asx_pool_handle poa;
    asx_pool_handle pob;
    asx_contended_mutex_handle cmb;
    asx_channel_id cha;
    asx_channel_id chb;
    asx_send_permit chpermit;
    uint64_t chval;

    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    asx_runtime_reset(&a);
    ASSERT_TRUE(two_block_make(&blk));
    asx_runtime_config_init(&cfg_a);
    asx_runtime_hooks_init(&hooks);
    cfg_b = cfg_a;
    cfg_b.storage = blk.block;
    cfg_b.storage_size = blk.size;

    ASSERT_EQ(asx_runtime_init(&a, &cfg_a, &hooks), ASX_OK);
    ASSERT_EQ(asx_runtime_init(&b, &cfg_b, &hooks), ASX_OK);
    ASSERT_EQ(a.tag, (uint16_t)0u);
    ASSERT_EQ(b.tag, (uint16_t)1u);
    ASSERT_TRUE(asx_runtime_is_initialized(&a));
    ASSERT_TRUE(asx_runtime_is_initialized(&b));
    ASSERT_TRUE(g_kernels[1] == (asx_kernel *)(void *)blk.block);

    /* The same slot and generation in each, told apart by the tag. */
    ASSERT_EQ(asx_region_open(&a, &ra), ASX_OK);
    ASSERT_EQ(asx_region_open(&b, &rb), ASX_OK);
    ASSERT_EQ(asx_handle_runtime_tag(ra), (uint16_t)0u);
    ASSERT_EQ(asx_handle_runtime_tag(rb), (uint16_t)1u);
    ASSERT_EQ(asx_handle_slot(ra), asx_handle_slot(rb));
    ASSERT_EQ(asx_handle_generation(ra), asx_handle_generation(rb));
    ASSERT_EQ(asx_task_spawn(ra, done_poll, NULL, &ta), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rb, done_poll, NULL, &tb), ASX_OK);
    ASSERT_EQ(asx_handle_runtime_tag(ta), (uint16_t)0u);
    ASSERT_EQ(asx_handle_runtime_tag(tb), (uint16_t)1u);
    ASSERT_EQ(asx_handle_slot(ta), asx_handle_slot(tb));

    /* Each runtime's counts are its own. */
    ASSERT_EQ(asx_runtime_region_count(&a), 1u);
    ASSERT_EQ(asx_runtime_region_count(&b), 1u);
    ASSERT_EQ(asx_runtime_task_count(&a), 1u);
    ASSERT_EQ(asx_runtime_task_count(&b), 1u);

    /* Wakers are each runtime's own: the arena index is the same, the tag
     * tells them apart, and a signal and its drain stay in one arena. */
    ASSERT_EQ(asx_waker_register(ta, &wa), ASX_OK);
    ASSERT_EQ(asx_waker_register(tb, &wb), ASX_OK);
    ASSERT_EQ(wa.slot, 0u);
    ASSERT_EQ(wb.slot, 1u << 16);
    ASSERT_EQ(asx_waker_active_count(&a), 1u);
    ASSERT_EQ(asx_waker_active_count(&b), 1u);
    ASSERT_EQ(asx_waker_wake(&wb), ASX_OK);
    ASSERT_EQ(asx_waker_drain_signaled(&a, drained, 4u), 0u);
    ASSERT_EQ(asx_waker_drain_signaled(&b, drained, 4u), 1u);
    ASSERT_EQ(drained[0], tb);
    asx_waker_deregister(&wa);
    asx_waker_deregister(&wb);
    ASSERT_EQ(asx_waker_active_count(&a), 0u);
    ASSERT_EQ(asx_waker_active_count(&b), 0u);

    /* Application timers are each runtime's own: a timer of one is not in
     * the other's wheel, and a deadline remembers the wheel that holds it. */
    wheel_a = asx_timer_wheel_global(&a);
    wheel_b = asx_timer_wheel_global(&b);
    ASSERT_TRUE(wheel_a != NULL && wheel_b != NULL && wheel_a != wheel_b);
    ASSERT_TRUE(asx_timer_wheel_global(NULL) == NULL);
    ASSERT_EQ(asx_timer_register(wheel_b, 50u, NULL, &th), ASX_OK);
    ASSERT_EQ(asx_timer_active_count(wheel_a), 0u);
    ASSERT_EQ(asx_timer_active_count(wheel_b), 1u);
    ASSERT_EQ(asx_deadline_init(&dl, 1000u), ASX_OK);
    ASSERT_EQ(asx_deadline_arm(&b, &dl, NULL), ASX_OK);
    ASSERT_EQ(asx_timer_active_count(wheel_a), 0u);
    ASSERT_EQ(asx_timer_active_count(wheel_b), 2u);
    ASSERT_EQ(asx_deadline_disarm(&dl), ASX_OK);
    ASSERT_EQ(asx_timer_active_count(wheel_b), 1u);
    ASSERT_TRUE(asx_timer_cancel(wheel_b, &th));
    ASSERT_EQ(asx_timer_active_count(wheel_b), 0u);

    /* Wait nodes are each runtime's own: a queue bound to b takes its nodes
     * from b's pool, and the id it hands out carries b's tag above the
     * index. */
    asx_wait_queue_init(&wq, NULL, 1u);
    wid = asx_wait_record_add(&wq);
    ASSERT_TRUE(wid != ASX_WAIT_NIL);
    ASSERT_EQ(wid >> 24, 1u);
    ASSERT_TRUE(asx_wait_node_at(wid) == &g_kernels[1]->waits.nodes[wid & 0xFFFFFFu]);
    ASSERT_EQ(g_kernels[1]->waits.in_use, 1u);
    ASSERT_EQ(g_kernels[0]->waits.in_use, 0u);
    ASSERT_TRUE(asx_wait_record_get(&wq, wid, asx_wait_node_at(wid)->generation) != NULL);
    asx_wait_record_release(&wq, wid);
    ASSERT_EQ(g_kernels[1]->waits.in_use, 0u);
    ASSERT_EQ(asx_wait_queue_len(&wq), 0u);

    /* Semaphores and mutexes are each runtime's own: the same arena index in
     * each, told apart by the tag, and one's permits are not the other's. */
    ASSERT_EQ(asx_semaphore_create(&a, 1u, &sa), ASX_OK);
    ASSERT_EQ(asx_semaphore_create(&b, 2u, &sb), ASX_OK);
    ASSERT_EQ(sa.slot, 0u);
    ASSERT_EQ(sb.slot, 1u << 16);
    ASSERT_EQ(asx_semaphore_available(sa), 1u);
    ASSERT_EQ(asx_semaphore_available(sb), 2u);
    ASSERT_EQ(asx_semaphore_try_acquire(sb, &pb), ASX_OK);
    ASSERT_EQ(asx_semaphore_available(sb), 1u);
    ASSERT_EQ(asx_semaphore_available(sa), 1u);
    ASSERT_EQ(asx_semaphore_release(pb), ASX_OK);
    ASSERT_EQ(asx_mutex_create(&b, &mb), ASX_OK);
    ASSERT_EQ(asx_mutex_try_lock(mb, &gb), ASX_OK);
    ASSERT_TRUE(asx_mutex_is_locked(mb));
    ASSERT_EQ(asx_mutex_unlock(gb), ASX_OK);
    ASSERT_EQ(asx_mutex_close(mb), ASX_OK);
    ASSERT_EQ(asx_semaphore_close(sa), ASX_OK);
    ASSERT_EQ(asx_semaphore_close(sb), ASX_OK);
    ASSERT_EQ(asx_semaphore_create(NULL, 1u, &sa), ASX_E_INVALID_ARGUMENT);

    /* Read-write locks, notifies, barriers and once cells likewise: the same
     * arena index in each runtime, told apart by the tag. */
    ASSERT_EQ(asx_rwlock_create(&a, &rwa), ASX_OK);
    ASSERT_EQ(asx_rwlock_create(&b, &rwb), ASX_OK);
    ASSERT_EQ(rwa.slot, 0u);
    ASSERT_EQ(rwb.slot, 1u << 16);
    ASSERT_EQ(asx_rwlock_try_write(rwb, &wg), ASX_OK);
    ASSERT_TRUE(asx_rwlock_is_write_locked(rwb));
    ASSERT_FALSE(asx_rwlock_is_write_locked(rwa));
    ASSERT_EQ(asx_rwlock_write_unlock(wg), ASX_OK);
    ASSERT_EQ(asx_notify_create(&b, &nb), ASX_OK);
    ASSERT_EQ(nb.slot, 1u << 16);
    ASSERT_EQ(asx_notify_one(nb), ASX_OK);
    ASSERT_EQ(asx_notify_stored_count(nb), 1u);
    ASSERT_EQ(asx_barrier_create(&b, 2u, &bb), ASX_OK);
    ASSERT_EQ(bb.slot, 1u << 16);
    ASSERT_EQ(asx_barrier_waiting_count(bb), 0u);
    ASSERT_EQ(asx_once_create(&b, &once_b), ASX_OK);
    ASSERT_EQ(once_b.slot, 1u << 16);
    ASSERT_FALSE(asx_once_is_initialized(once_b));
    ASSERT_EQ(asx_rwlock_close(rwa), ASX_OK);
    ASSERT_EQ(asx_rwlock_close(rwb), ASX_OK);
    ASSERT_EQ(asx_notify_close(nb), ASX_OK);
    ASSERT_EQ(asx_barrier_close(bb), ASX_OK);
    ASSERT_EQ(asx_once_close(once_b), ASX_OK);
    ASSERT_EQ(asx_rwlock_create(NULL, &rwa), ASX_E_INVALID_ARGUMENT);

    /* Pools and contended mutexes: the same index, told apart by the tag; the
     * contended mutex's inner lock is its runtime's. */
    memset(&pcfg, 0, sizeof(pcfg));
    pcfg.max_size = 1u;
    pcfg.create_fn = pool_make_one;
    ASSERT_EQ(asx_pool_create(&a, &pcfg, &poa), ASX_OK);
    ASSERT_EQ(asx_pool_create(&b, &pcfg, &pob), ASX_OK);
    ASSERT_EQ(poa.slot, 0u);
    ASSERT_EQ(pob.slot, 1u << 16);
    ASSERT_EQ(asx_pool_close(pob), ASX_OK);
    ASSERT_FALSE(asx_pool_is_closed(poa));
    ASSERT_EQ(asx_pool_close(poa), ASX_OK);
    ASSERT_EQ(asx_contended_mutex_create(&b, &cmb), ASX_OK);
    ASSERT_EQ(cmb.slot, 1u << 16);
    ASSERT_EQ(asx_contended_mutex_try_lock(cmb, &gb), ASX_OK);
    ASSERT_TRUE(asx_contended_mutex_is_locked(cmb));
    ASSERT_EQ(asx_contended_mutex_unlock(gb), ASX_OK);
    ASSERT_EQ(asx_contended_mutex_close(cmb), ASX_OK);

    /* Channels likewise: a channel belongs to the runtime of its region; the
     * same slot index, told apart by the tag, and a message of one is not
     * the other's. */
    ASSERT_EQ(asx_channel_create(ra, 2u, &cha), ASX_OK);
    ASSERT_EQ(asx_channel_create(rb, 2u, &chb), ASX_OK);
    ASSERT_EQ(asx_handle_runtime_tag(cha), (uint16_t)0u);
    ASSERT_EQ(asx_handle_runtime_tag(chb), (uint16_t)1u);
    ASSERT_EQ(asx_handle_slot(cha), asx_handle_slot(chb));
    ASSERT_EQ(asx_channel_try_reserve(chb, &chpermit), ASX_OK);
    ASSERT_EQ(asx_send_permit_send(&chpermit, 41u), ASX_OK);
    ASSERT_EQ(asx_channel_try_recv(cha, &chval), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(asx_channel_try_recv(chb, &chval), ASX_OK);
    ASSERT_EQ(chval, 41u);

    /* A handle of the other runtime is no member of this one's world. */
    ASSERT_EQ(asx_task_watch(ta, tb), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_obligation_reserve_ex(ra, ASX_OBLIGATION_KIND_GENERIC, tb, &ob), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_runtime_obligation_count(&a), 0u);
    ASSERT_EQ(asx_runtime_obligation_count(&b), 0u);

    /* Each schedules without the other: running a completes a's task and
     * leaves b's untouched. */
    ASSERT_FALSE(asx_runtime_is_quiescent(&b));
    run = asx_budget_from_polls(8u);
    ASSERT_EQ(asx_scheduler_run(ra, &run), ASX_OK);
    ASSERT_EQ(asx_task_get_state(ta, &state), ASX_OK);
    ASSERT_EQ(state, ASX_TASK_COMPLETED);
    ASSERT_EQ(asx_task_get_state(tb, &state), ASX_OK);
    ASSERT_EQ(state, ASX_TASK_CREATED);
    ASSERT_TRUE(asx_runtime_is_quiescent(&a));
    ASSERT_FALSE(asx_runtime_is_quiescent(&b));

    run = asx_budget_from_polls(8u);
    ASSERT_EQ(asx_scheduler_run(rb, &run), ASX_OK);
    ASSERT_EQ(asx_task_get_state(tb, &state), ASX_OK);
    ASSERT_EQ(state, ASX_TASK_COMPLETED);
    ASSERT_TRUE(asx_runtime_is_quiescent(&b));

    /* Shutting b down leaves a live; the last one out stops the rest. */
    asx_runtime_shutdown(&b);
    ASSERT_FALSE(asx_runtime_is_initialized(&b));
    ASSERT_TRUE(g_kernels[1] == NULL);
    ASSERT_TRUE(asx_runtime_is_initialized(&a));
    ASSERT_EQ(asx_task_get_state(ta, &state), ASX_OK);
    ASSERT_EQ(state, ASX_TASK_COMPLETED);
    asx_runtime_shutdown(&a);
    ASSERT_FALSE(asx_runtime_is_initialized(&a));
    two_block_free(&blk);
}

TEST(a_second_runtime_is_refused_by_name_while_a_shared_subsystem_is_in_use) {
    asx_runtime a;
    asx_runtime b;
    asx_runtime_config cfg_a;
    asx_runtime_config cfg_b;
    asx_runtime_hooks hooks;
    asx_runtime_hooks other;
    two_block blk;
    asx_region_id ra;
    asx_channel_id ch;
    asx_semaphore_handle sem;
    asx_rwlock_handle rw;
    asx_contended_mutex_handle cm;
    asx_watch_sender wtx;
    asx_watch_receiver wrx;
    asx_task_id tid;

    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    asx_runtime_reset(&a);
    ASSERT_TRUE(two_block_make(&blk));
    asx_runtime_config_init(&cfg_a);
    asx_runtime_hooks_init(&hooks);
    cfg_b = cfg_a;
    cfg_b.storage = blk.block;
    cfg_b.storage_size = blk.size;

    /* A watch channel is in use: a second runtime would share the table. */
    ASSERT_EQ(asx_runtime_init(&a, &cfg_a, &hooks), ASX_OK);
    ASSERT_EQ(asx_region_open(&a, &ra), ASX_OK);
    ASSERT_EQ(asx_watch_create(0u, &wtx, &wrx), ASX_OK);
    ASSERT_EQ(asx_runtime_init(&b, &cfg_b, &hooks), ASX_E_INVALID_STATE);
    ASSERT_TRUE(reason_names("watches"));
    ASSERT_FALSE(asx_runtime_is_initialized(&b));
    ASSERT_TRUE(g_kernels[1] == NULL);

    /* A runtime without a block of its own is refused as a state, by name. */
    asx_runtime_reset(&a);
    ASSERT_EQ(asx_runtime_init(&a, &cfg_a, &hooks), ASX_OK);
    ASSERT_EQ(asx_runtime_init(&b, &cfg_a, &hooks), ASX_E_INVALID_STATE);
    ASSERT_TRUE(reason_names("storage block"));

    /* The hook table is one for the process. */
    other = hooks;
    other.entropy.ctx = &other;
    ASSERT_EQ(asx_runtime_init(&b, &cfg_b, &other), ASX_E_INVALID_STATE);
    ASSERT_TRUE(reason_names("hooks"));
    ASSERT_TRUE(g_kernels[1] == NULL);

    /* Two runtimes live: the still process-wide subsystems refuse to acquire
     * state, naming themselves, and a runtime's own state is not refused. */
    ASSERT_EQ(asx_runtime_init(&b, &cfg_b, &hooks), ASX_OK);
    ASSERT_EQ(asx_region_open(&a, &ra), ASX_OK);
    ASSERT_EQ(asx_channel_create(ra, 4u, &ch), ASX_OK);
    ASSERT_EQ(asx_watch_create(0u, &wtx, &wrx), ASX_E_INVALID_STATE);
    ASSERT_TRUE(reason_names("watches"));
    ASSERT_EQ(asx_contended_mutex_create(&a, &cm), ASX_OK);
    ASSERT_EQ(asx_contended_mutex_close(cm), ASX_OK);
    ASSERT_EQ(asx_semaphore_create(&a, 1u, &sem), ASX_OK);
    ASSERT_EQ(asx_semaphore_close(sem), ASX_OK);
    ASSERT_EQ(asx_rwlock_create(&a, &rw), ASX_OK);
    ASSERT_EQ(asx_rwlock_close(rw), ASX_OK);
    ASSERT_EQ(asx_task_spawn(ra, done_poll, NULL, &tid), ASX_OK);

    /* One runtime again: the subsystem takes state, as before. */
    asx_runtime_shutdown(&b);
    ASSERT_EQ(asx_watch_create(0u, &wtx, &wrx), ASX_OK);
    asx_runtime_shutdown(&a);
    two_block_free(&blk);
}

/* ------------------------------------------------------------------ */
/* Test (d): an interleaved run is each runtime's run alone            */
/* ------------------------------------------------------------------ */

/* One runtime's script: ten calls, each one logged by its status. Region
 * open, a child region, three tasks (one reserves and commits an obligation,
 * one in the child reserves and aborts one, one completes), two scheduler
 * runs, closes, a last run. No cancel, no channel: nothing that still is
 * process-wide. */
#define SCRIPT_STEPS 10u

typedef struct {
    asx_region_id region;
    int abort_it;
    asx_status reserve_st;
    asx_status settle_st;
} script_job;

typedef struct {
    asx_runtime *rt;
    asx_region_id root;
    asx_region_id child;
    asx_task_id tid[3];
    script_job commit_job;
    script_job abort_job;
    asx_status log[SCRIPT_STEPS];
    uint64_t digest;  /* the runtime's trace digest at the end */
    uint64_t emitted; /* and the events it emitted */
    uint32_t regions;
    uint32_t tasks;
    uint32_t obligations;
} script;

static asx_status obligation_poll(void *user_data, asx_task_id self) {
    script_job *j = (script_job *)user_data;
    asx_obligation_id ob;
    (void)self;
    j->reserve_st = asx_obligation_reserve(j->region, &ob);
    if (j->reserve_st == ASX_OK) {
        j->settle_st = j->abort_it ? asx_obligation_abort(ob) : asx_obligation_commit(ob);
    }
    return ASX_OK;
}

static void script_step(script *s, uint32_t step) {
    asx_budget run;
    asx_status st;

    switch (step) {
    case 0u: st = asx_region_open(s->rt, &s->root); break;
    case 1u: st = asx_region_open_child(s->root, &s->child); break;
    case 2u:
        s->commit_job.region = s->root;
        s->commit_job.abort_it = 0;
        st = asx_task_spawn(s->root, obligation_poll, &s->commit_job, &s->tid[0]);
        break;
    case 3u:
        s->abort_job.region = s->child;
        s->abort_job.abort_it = 1;
        st = asx_task_spawn(s->child, obligation_poll, &s->abort_job, &s->tid[1]);
        break;
    case 4u: st = asx_task_spawn(s->root, done_poll, NULL, &s->tid[2]); break;
    case 5u:
        run = asx_budget_from_polls(2u);
        st = asx_scheduler_run(s->root, &run);
        break;
    case 6u:
        run = asx_budget_from_polls(8u);
        st = asx_scheduler_run(s->root, &run);
        break;
    case 7u: st = asx_region_close(s->child); break;
    case 8u: st = asx_region_close(s->root); break;
    default:
        run = asx_budget_from_polls(8u);
        st = asx_scheduler_run(s->root, &run);
        break;
    }
    s->log[step] = st;
}

static void script_collect(script *s) {
    const asx_kernel *k = asx_kernel_for_tag(s->rt->tag);
    s->digest = asx_trace_digest_internal(g_kernels[s->rt->tag]);
    s->emitted = asx_trace_emitted_total_internal(k);
    s->regions = asx_runtime_region_count(s->rt);
    s->tasks = asx_runtime_task_count(s->rt);
    s->obligations = asx_runtime_obligation_count(s->rt);
}

/* Both runtimes are initialized (a on the library's block, b on its own);
 * `which` 0 runs both scripts step for step, 1 only a's, 2 only b's. */
static int run_phase(int which, script *sa, script *sb) {
    asx_runtime a;
    asx_runtime b;
    asx_runtime_config cfg_a;
    asx_runtime_config cfg_b;
    asx_runtime_hooks hooks;
    two_block blk;
    uint32_t i;

    memset(sa, 0, sizeof(*sa));
    memset(sb, 0, sizeof(*sb));
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    asx_runtime_reset(&a);
    if (!two_block_make(&blk)) return 0;
    asx_runtime_config_init(&cfg_a);
    asx_runtime_hooks_init(&hooks);
    cfg_b = cfg_a;
    cfg_b.storage = blk.block;
    cfg_b.storage_size = blk.size;
    if (asx_runtime_init(&a, &cfg_a, &hooks) != ASX_OK) return 0;
    if (asx_runtime_init(&b, &cfg_b, &hooks) != ASX_OK) return 0;
    sa->rt = &a;
    sb->rt = &b;

    for (i = 0u; i < SCRIPT_STEPS; i++) {
        if (which != 2) script_step(sa, i);
        if (which != 1) script_step(sb, i);
    }
    if (which != 2) script_collect(sa);
    if (which != 1) script_collect(sb);

    asx_runtime_shutdown(&b);
    asx_runtime_shutdown(&a);
    two_block_free(&blk);
    return 1;
}

static void script_expect_equal(const script *inter, const script *solo, uint32_t *differ) {
    uint32_t i;
    for (i = 0u; i < SCRIPT_STEPS; i++) {
        if (inter->log[i] != solo->log[i]) *differ += 1u;
    }
    if (inter->digest != solo->digest) *differ += 1u;
    if (inter->emitted != solo->emitted) *differ += 1u;
    if (inter->regions != solo->regions) *differ += 1u;
    if (inter->tasks != solo->tasks) *differ += 1u;
    if (inter->obligations != solo->obligations) *differ += 1u;
    if (inter->commit_job.reserve_st != solo->commit_job.reserve_st) *differ += 1u;
    if (inter->commit_job.settle_st != solo->commit_job.settle_st) *differ += 1u;
    if (inter->abort_job.reserve_st != solo->abort_job.reserve_st) *differ += 1u;
    if (inter->abort_job.settle_st != solo->abort_job.settle_st) *differ += 1u;
}

TEST(an_interleaved_run_of_two_runtimes_is_each_runtime_run_alone) {
    script a_inter;
    script b_inter;
    script a_solo;
    script b_solo;
    script idle;
    uint32_t differ = 0u;
    uint32_t i;

    ASSERT_TRUE(run_phase(0, &a_inter, &b_inter));
    ASSERT_TRUE(run_phase(1, &a_solo, &idle));
    ASSERT_TRUE(run_phase(2, &idle, &b_solo));

    /* The script ran, in its own runtime: the setup calls are accepted and
     * each runtime's trace saw events. */
    for (i = 0u; i < 5u; i++) {
        ASSERT_EQ(a_inter.log[i], ASX_OK);
        ASSERT_EQ(b_inter.log[i], ASX_OK);
    }
    ASSERT_TRUE(a_inter.emitted > 0u);
    ASSERT_TRUE(b_inter.emitted > 0u);
    ASSERT_EQ(asx_handle_runtime_tag(a_inter.root), (uint16_t)0u);
    ASSERT_EQ(asx_handle_runtime_tag(b_inter.root), (uint16_t)1u);
    ASSERT_EQ(a_inter.commit_job.settle_st, ASX_OK);
    ASSERT_EQ(b_inter.commit_job.settle_st, ASX_OK);
    ASSERT_EQ(a_inter.abort_job.settle_st, ASX_OK);
    ASSERT_EQ(b_inter.abort_job.settle_st, ASX_OK);

    /* Isolation: with the other runtime interleaved step for step, each
     * runtime's statuses, trace digest, event count and counts are what its
     * run alone gave. */
    script_expect_equal(&a_inter, &a_solo, &differ);
    script_expect_equal(&b_inter, &b_solo, &differ);
    ASSERT_EQ(differ, 0u);
}

#else /* the cases below are reported SKIP by RUN_TEST_IF */

TEST(two_runtimes_have_independent_regions_tasks_and_quiescence) {}
TEST(a_second_runtime_is_refused_by_name_while_a_shared_subsystem_is_in_use) {}
TEST(an_interleaved_run_of_two_runtimes_is_each_runtime_run_alone) {}

#endif

int main(void) {
    fprintf(stderr, "=== test_two_runtimes ===\n");
    RUN_TEST_IF(TWO_RUNTIMES_RUN, two_runtimes_have_independent_regions_tasks_and_quiescence,
                "needs ASX_MAX_RUNTIMES >= 2 and a deterministic build");
    RUN_TEST_IF(TWO_RUNTIMES_RUN,
                a_second_runtime_is_refused_by_name_while_a_shared_subsystem_is_in_use,
                "needs ASX_MAX_RUNTIMES >= 2 and a deterministic build");
    RUN_TEST_IF(TWO_RUNTIMES_RUN, an_interleaved_run_of_two_runtimes_is_each_runtime_run_alone,
                "needs ASX_MAX_RUNTIMES >= 2 and a deterministic build");
    RUN_TEST(hook_tables_with_equal_members_match_whatever_their_padding_bytes);
    TEST_REPORT();
    return test_failures;
}
