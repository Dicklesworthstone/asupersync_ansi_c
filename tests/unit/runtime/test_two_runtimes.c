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
 * runs it. Elsewhere it is reported as SKIP, not as passed.
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

#if TWO_RUNTIMES_RUN

static asx_status done_poll(void *user_data, asx_task_id self) {
    (void)user_data;
    (void)self;
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

    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    asx_runtime_reset();
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
    ASSERT_EQ(asx_region_open(&ra), ASX_OK);
    ASSERT_EQ(asx_region_open_kernel_internal(g_kernels[1], &rb), ASX_OK);
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
    asx_task_id tid;

    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    asx_runtime_reset();
    ASSERT_TRUE(two_block_make(&blk));
    asx_runtime_config_init(&cfg_a);
    asx_runtime_hooks_init(&hooks);
    cfg_b = cfg_a;
    cfg_b.storage = blk.block;
    cfg_b.storage_size = blk.size;

    /* A channel is in use: a second runtime would share the table. */
    ASSERT_EQ(asx_runtime_init(&a, &cfg_a, &hooks), ASX_OK);
    ASSERT_EQ(asx_region_open(&ra), ASX_OK);
    ASSERT_EQ(asx_channel_create(ra, 4u, &ch), ASX_OK);
    ASSERT_EQ(asx_runtime_init(&b, &cfg_b, &hooks), ASX_E_INVALID_STATE);
    ASSERT_TRUE(reason_names("channels"));
    ASSERT_FALSE(asx_runtime_is_initialized(&b));
    ASSERT_TRUE(g_kernels[1] == NULL);

    /* A runtime without a block of its own is refused as a state, by name. */
    asx_runtime_reset();
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
    ASSERT_EQ(asx_region_open(&ra), ASX_OK);
    ASSERT_EQ(asx_channel_create(ra, 4u, &ch), ASX_E_INVALID_STATE);
    ASSERT_TRUE(reason_names("channels"));
    ASSERT_EQ(asx_semaphore_create(1u, &sem), ASX_E_INVALID_STATE);
    ASSERT_TRUE(reason_names("semaphores"));
    ASSERT_EQ(asx_task_spawn(ra, done_poll, NULL, &tid), ASX_OK);

    /* One runtime again: the subsystem takes state, as before. */
    asx_runtime_shutdown(&b);
    ASSERT_EQ(asx_channel_create(ra, 4u, &ch), ASX_OK);
    asx_runtime_shutdown(&a);
    two_block_free(&blk);
}

#else /* the cases below are reported SKIP by RUN_TEST_IF */

TEST(two_runtimes_have_independent_regions_tasks_and_quiescence) {}
TEST(a_second_runtime_is_refused_by_name_while_a_shared_subsystem_is_in_use) {}

#endif

int main(void) {
    fprintf(stderr, "=== test_two_runtimes ===\n");
    RUN_TEST_IF(TWO_RUNTIMES_RUN, two_runtimes_have_independent_regions_tasks_and_quiescence,
                "needs ASX_MAX_RUNTIMES >= 2 and a deterministic build");
    RUN_TEST_IF(TWO_RUNTIMES_RUN,
                a_second_runtime_is_refused_by_name_while_a_shared_subsystem_is_in_use,
                "needs ASX_MAX_RUNTIMES >= 2 and a deterministic build");
    TEST_REPORT();
    return test_failures;
}
