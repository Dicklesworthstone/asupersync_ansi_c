/*
 * test_process.c — unit tests for deterministic process host surface
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/process/process.h>

TEST(spawn_and_query_program) {
    asx_process_handle process;
    asx_process_spawn_options opts;
    const char *program = NULL;
    asx_process_reset();
    opts.program = "worker";
    opts.polls_until_exit = 0u;
    opts.exit_code = 17;
    opts.auto_exit = 1;
    ASSERT_EQ(asx_process_spawn(&process, &opts), ASX_OK);
    ASSERT_EQ(asx_process_program(process, &program), ASX_OK);
    ASSERT_STR_EQ(program, "worker");
}

TEST(poll_wait_counts_down_then_exits) {
    asx_process_handle process;
    asx_process_spawn_options opts;
    int32_t exit_code = -1;
    asx_process_reset();
    opts.program = "task-runner";
    opts.polls_until_exit = 2u;
    opts.exit_code = 5;
    opts.auto_exit = 1;
    ASSERT_EQ(asx_process_spawn(&process, &opts), ASX_OK);
    ASSERT_EQ(asx_process_poll_wait(process, &exit_code), ASX_E_PENDING);
    ASSERT_EQ(asx_process_poll_wait(process, &exit_code), ASX_E_PENDING);
    ASSERT_EQ(asx_process_poll_wait(process, &exit_code), ASX_OK);
    ASSERT_EQ(exit_code, 5);
}

TEST(request_shutdown_forces_clean_exit) {
    asx_process_handle process;
    asx_process_spawn_options opts;
    int32_t exit_code = -1;
    asx_process_state state;
    asx_process_reset();
    opts.program = "server";
    opts.polls_until_exit = 9u;
    opts.exit_code = 33;
    opts.auto_exit = 1;
    ASSERT_EQ(asx_process_spawn(&process, &opts), ASX_OK);
    ASSERT_EQ(asx_process_request_shutdown(process), ASX_OK);
    ASSERT_EQ(asx_process_poll_wait(process, &exit_code), ASX_OK);
    ASSERT_EQ(exit_code, 0);
    ASSERT_EQ(asx_process_state_query(process, &state), ASX_OK);
    ASSERT_EQ(state, ASX_PROCESS_EXITED);
}

TEST(kill_marks_terminated) {
    asx_process_handle process;
    asx_process_spawn_options opts;
    int32_t exit_code = 0;
    asx_process_state state;
    asx_process_reset();
    opts.program = "hung-child";
    opts.polls_until_exit = 0u;
    opts.exit_code = 0;
    opts.auto_exit = 0;
    ASSERT_EQ(asx_process_spawn(&process, &opts), ASX_OK);
    ASSERT_EQ(asx_process_kill(process, 137), ASX_OK);
    ASSERT_EQ(asx_process_poll_wait(process, &exit_code), ASX_OK);
    ASSERT_EQ(exit_code, 137);
    ASSERT_EQ(asx_process_state_query(process, &state), ASX_OK);
    ASSERT_EQ(state, ASX_PROCESS_TERMINATED);
}

TEST(process_capacity_is_enforced) {
    asx_process_handle processes[ASX_MAX_PROCESSES];
    asx_process_handle extra;
    asx_process_spawn_options opts;
    uint32_t i;
    asx_process_reset();
    opts.program = "cap";
    opts.polls_until_exit = 0u;
    opts.exit_code = 0;
    opts.auto_exit = 0;
    for (i = 0; i < ASX_MAX_PROCESSES; i++) {
        ASSERT_EQ(asx_process_spawn(&processes[i], &opts), ASX_OK);
    }
    ASSERT_EQ(asx_process_spawn(&extra, &opts), ASX_E_RESOURCE_EXHAUSTED);
}

TEST(process_reset_invalidates_handles) {
    asx_process_handle process;
    asx_process_spawn_options opts;
    asx_process_reset();
    opts.program = "reset";
    opts.polls_until_exit = 0u;
    opts.exit_code = 0;
    opts.auto_exit = 1;
    ASSERT_EQ(asx_process_spawn(&process, &opts), ASX_OK);
    ASSERT_TRUE(asx_process_is_alive(process));
    asx_process_reset();
    ASSERT_FALSE(asx_process_is_alive(process));
}

TEST(process_reset_reuse_bumps_generation) {
    asx_process_handle old_process;
    asx_process_handle new_process;
    asx_process_spawn_options opts;
    const char *program = NULL;

    asx_process_reset();
    opts.program = "first";
    opts.polls_until_exit = 0u;
    opts.exit_code = 0;
    opts.auto_exit = 1;
    ASSERT_EQ(asx_process_spawn(&old_process, &opts), ASX_OK);

    asx_process_reset();

    opts.program = "second";
    ASSERT_EQ(asx_process_spawn(&new_process, &opts), ASX_OK);

    ASSERT_TRUE(new_process.slot == old_process.slot);
    ASSERT_TRUE(new_process.generation != old_process.generation);
    ASSERT_FALSE(asx_process_is_alive(old_process));
    ASSERT_EQ(asx_process_program(old_process, &program), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_process_program(new_process, &program), ASX_OK);
    ASSERT_STR_EQ(program, "second");
}

int main(void) {
    RUN_TEST(spawn_and_query_program);
    RUN_TEST(poll_wait_counts_down_then_exits);
    RUN_TEST(request_shutdown_forces_clean_exit);
    RUN_TEST(kill_marks_terminated);
    RUN_TEST(process_capacity_is_enforced);
    RUN_TEST(process_reset_invalidates_handles);
    RUN_TEST(process_reset_reuse_bumps_generation);
    TEST_REPORT();
    return test_failures;
}
