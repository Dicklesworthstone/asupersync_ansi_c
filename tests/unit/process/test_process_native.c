/*
 * test_process_native.c — real child processes driven by the wake-driven
 * scheduler (POSIX, non-deterministic builds)
 *
 * Every task-based scenario runs on one thread: progress is only possible
 * if pipe I/O and exit waits park the task and the reactor (pipe
 * readiness, pidfd readiness or the backoff timer) wakes it. Poll counts
 * are bounded to prove the runtime waits instead of spinning.
 *
 * Deterministic POSIX builds have no live reactor, so they only exercise
 * backend selection plus the blocking wait_with_output path (which does
 * not need the scheduler); non-POSIX builds report a skip.
 *
 * SPDX-License-Identifier: MIT
 */

#if defined(ASX_PROFILE_POSIX) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700 /* POSIX.1-2008 + XSI: realpath, mkdtemp, setenv */
#endif

#include "test_harness.h"
#include <asx/asx.h>
#include <asx/process/process.h>
#include <asx/runtime/rt.h>
#include <asx/runtime/runtime.h>
#include <asx/time/sleep.h>
#include <string.h>

#if defined(ASX_PROFILE_POSIX)
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Run `cmd` to completion with the blocking helper. */
static asx_status run_blocking(const asx_process_command *cmd, asx_process_output *out) {
    asx_process_handle h;
    asx_status st = asx_process_command_spawn(cmd, &h);
    if (st != ASX_OK) return st;
    st = asx_process_wait_with_output(h, out);
    if (asx_process_release(h) != ASX_OK && st == ASX_OK) st = ASX_E_INVALID_STATE;
    return st;
}

TEST(backend_selection_and_reset) {
    asx_process_reset();
    ASSERT_EQ(asx_process_get_backend(), ASX_PROCESS_BACKEND_MEMORY);
    ASSERT_EQ(asx_process_set_backend((asx_process_backend)9), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_process_set_backend(ASX_PROCESS_BACKEND_NATIVE), ASX_OK);
    ASSERT_EQ(asx_process_get_backend(), ASX_PROCESS_BACKEND_NATIVE);
    asx_process_reset();
    ASSERT_EQ(asx_process_get_backend(), ASX_PROCESS_BACKEND_MEMORY);
}

TEST(blocking_wait_with_output_captures_everything) {
    asx_process_command cmd;
    asx_process_output out;

    asx_process_reset();
    ASSERT_EQ(asx_process_set_backend(ASX_PROCESS_BACKEND_NATIVE), ASX_OK);
    asx_process_command_init(&cmd, "/bin/sh");
    ASSERT_EQ(asx_process_command_arg(&cmd, "-c"), ASX_OK);
    ASSERT_EQ(asx_process_command_arg(&cmd, "echo out; echo err 1>&2; exit 7"), ASX_OK);
    asx_process_command_stdout(&cmd, ASX_PROCESS_STDIO_PIPED);
    asx_process_command_stderr(&cmd, ASX_PROCESS_STDIO_PIPED);
    ASSERT_EQ(run_blocking(&cmd, &out), ASX_OK);
    ASSERT_EQ(out.exit_code, 7);
    ASSERT_EQ(out.term_signal, 0);
    ASSERT_EQ(out.success, 0);
    ASSERT_EQ(out.stdout_len, 4u);
    ASSERT_EQ(memcmp(out.stdout_data, "out\n", 4u), 0);
    ASSERT_EQ(out.stderr_len, 4u);
    ASSERT_EQ(memcmp(out.stderr_data, "err\n", 4u), 0);
    asx_process_reset();
}

TEST(spawn_failures_leave_nothing_behind) {
    asx_process_command cmd;
    asx_process_handle h;

    asx_process_reset();
    ASSERT_EQ(asx_process_set_backend(ASX_PROCESS_BACKEND_NATIVE), ASX_OK);
    asx_process_command_init(&cmd, "asx-definitely-not-a-program");
    ASSERT_EQ(asx_process_command_spawn(&cmd, &h), ASX_E_NOT_FOUND);
    asx_process_command_init(&cmd, "/nonexistent/asx/prog");
    ASSERT_EQ(asx_process_command_spawn(&cmd, &h), ASX_E_NOT_FOUND);
    asx_process_command_init(&cmd, "/bin/sh");
    ASSERT_EQ(asx_process_command_current_dir(&cmd, "/nonexistent/asx/dir"), ASX_OK);
    ASSERT_EQ(asx_process_command_spawn(&cmd, &h), ASX_E_NOT_FOUND); /* chdir in the child */
    ASSERT_EQ(waitpid(-1, NULL, WNOHANG), -1);                       /* no child, no zombie */
    ASSERT_EQ(errno, ECHILD);
    asx_process_command_init(&cmd, "");
    ASSERT_EQ(asx_process_command_spawn(&cmd, &h), ASX_E_INVALID_ARGUMENT);
    asx_process_command_init(&cmd, "/bin/sh");
    ASSERT_EQ(asx_process_command_env(&cmd, "A=B", "x"), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_process_command_env(&cmd, "", "x"), ASX_E_INVALID_ARGUMENT);
    asx_process_reset();
}

#if !ASX_DETERMINISTIC

static asx_runtime g_rt;

/* Gone = reaped (a zombie still answers kill(pid, 0)). */
static int pid_gone(uint32_t pid) { return kill((pid_t)pid, 0) != 0 && errno == ESRCH; }

static void sleep_ms(long ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

static int setup(void) {
    if (asx_runtime_init_default(&g_rt) != ASX_OK) return 0;
    return asx_process_get_backend() == ASX_PROCESS_BACKEND_NATIVE;
}

/* -------------------------------------------------------------------
 * sh: stdout + stderr + exit code, collected by a parked task
 * ------------------------------------------------------------------- */

typedef struct {
    asx_process_handle h;
    asx_process_output out;
    uint32_t polls;
} capture_state;

static asx_status capture_poll(void *ud, asx_task_id self) {
    capture_state *c = (capture_state *)ud;
    (void)self;
    c->polls++;
    return asx_process_poll_output(c->h, &c->out);
}

TEST(sh_output_and_exit_code_with_parked_task) {
    asx_region_id r;
    asx_task_id t;
    capture_state *c = NULL;
    asx_process_command cmd;
    asx_budget budget;

    ASSERT_TRUE(setup());
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_task_spawn_captured(r, capture_poll, (uint32_t)sizeof(capture_state), NULL, &t,
                                      (void **)&c),
              ASX_OK);
    asx_process_output_init(&c->out);
    asx_process_command_init(&cmd, "/bin/sh");
    ASSERT_EQ(asx_process_command_arg(&cmd, "-c"), ASX_OK);
    ASSERT_EQ(asx_process_command_arg(&cmd, "echo hi; echo err 1>&2; exit 3"), ASX_OK);
    asx_process_command_stdout(&cmd, ASX_PROCESS_STDIO_PIPED);
    asx_process_command_stderr(&cmd, ASX_PROCESS_STDIO_PIPED);
    ASSERT_EQ(asx_process_command_spawn(&cmd, &c->h), ASX_OK);
    ASSERT_TRUE(asx_process_id(c->h) > 0u);

    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_OK);
    ASSERT_EQ(c->out.stdout_len, 3u);
    ASSERT_EQ(memcmp(c->out.stdout_data, "hi\n", 3u), 0);
    ASSERT_EQ(c->out.stderr_len, 4u);
    ASSERT_EQ(memcmp(c->out.stderr_data, "err\n", 4u), 0);
    ASSERT_EQ(c->out.exit_code, 3);
    ASSERT_EQ(c->out.term_signal, 0);
    ASSERT_EQ(c->out.success, 0);
    ASSERT_TRUE(c->polls <= 10u); /* parked on pipes / exit, not spinning */
    {
        asx_process_state state;
        ASSERT_EQ(asx_process_state_query(c->h, &state), ASX_OK);
        ASSERT_EQ(state, ASX_PROCESS_EXITED);
        ASSERT_FALSE(asx_process_exited_successfully(c->h));
    }
    ASSERT_EQ(asx_process_release(c->h), ASX_OK);
    ASSERT_FALSE(asx_process_is_alive(c->h));
}

/* -------------------------------------------------------------------
 * stdin piped into cat, echoed back through stdout
 * ------------------------------------------------------------------- */

typedef struct {
    asx_co_state co;
    asx_process_handle h;
    const char *msg;
    uint32_t sent;
    uint8_t got[64];
    uint32_t got_len;
    asx_process_exit_status status;
    uint32_t polls;
} cat_state;

static asx_status cat_poll(void *ud, asx_task_id self) {
    cat_state *c = (cat_state *)ud;
    asx_status st;
    uint32_t n;
    (void)self;
    c->polls++;
    ASX_CO_BEGIN(&c->co);
    while (c->sent < (uint32_t)strlen(c->msg)) {
        st = asx_process_stdin_write(c->h, (const uint8_t *)c->msg + c->sent,
                                     (uint32_t)strlen(c->msg) - c->sent, &n);
        if (st == ASX_E_PENDING) {
            ASX_CO_YIELD(&c->co);
            continue;
        }
        if (st != ASX_OK) return st;
        c->sent += n;
    }
    st = asx_process_stdin_close(c->h);
    if (st != ASX_OK) return st;
    for (;;) {
        st = asx_process_stdout_read(c->h, &c->got[c->got_len],
                                     (uint32_t)sizeof(c->got) - c->got_len, &n);
        if (st == ASX_E_PENDING) {
            ASX_CO_YIELD(&c->co);
            continue;
        }
        if (st != ASX_OK) return st;
        if (n == 0u) break; /* EOF: cat exited after stdin closed */
        c->got_len += n;
    }
    for (;;) {
        st = asx_process_poll_status(c->h, &c->status);
        if (st == ASX_OK) break;
        if (st != ASX_E_PENDING) return st;
        ASX_CO_YIELD(&c->co);
    }
    ASX_CO_END(&c->co);
}

TEST(stdin_piped_to_cat_round_trips) {
    asx_region_id r;
    asx_task_id t;
    cat_state *c = NULL;
    asx_process_command cmd;
    asx_budget budget;
    uint32_t n = 0;

    ASSERT_TRUE(setup());
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_task_spawn_captured(r, cat_poll, (uint32_t)sizeof(cat_state), NULL, &t,
                                      (void **)&c),
              ASX_OK);
    c->msg = "ping pong\n";
    asx_process_command_init(&cmd, "cat"); /* resolved through PATH */
    asx_process_command_stdin(&cmd, ASX_PROCESS_STDIO_PIPED);
    asx_process_command_stdout(&cmd, ASX_PROCESS_STDIO_PIPED);
    ASSERT_EQ(asx_process_command_spawn(&cmd, &c->h), ASX_OK);

    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_OK);
    ASSERT_EQ(c->got_len, (uint32_t)strlen("ping pong\n"));
    ASSERT_EQ(memcmp(c->got, "ping pong\n", c->got_len), 0);
    ASSERT_EQ(c->status.state, ASX_PROCESS_EXITED);
    ASSERT_EQ(c->status.code, 0);
    ASSERT_EQ(c->status.signal, 0);
    ASSERT_TRUE(asx_process_exited_successfully(c->h));
    ASSERT_TRUE(c->polls <= 12u);
    /* stdin is closed now; reads stay at EOF. */
    ASSERT_EQ(asx_process_stdin_write(c->h, (const uint8_t *)"x", 1u, &n), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_process_stdout_read(c->h, c->got, 8u, &n), ASX_OK);
    ASSERT_EQ(n, 0u);
    ASSERT_EQ(asx_process_stderr_read(c->h, c->got, 8u, &n), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_process_release(c->h), ASX_OK);
}

/* -------------------------------------------------------------------
 * kill: a parked waiter observes signal termination
 * ------------------------------------------------------------------- */

typedef struct {
    asx_process_handle h;
    asx_process_exit_status status;
    uint32_t polls;
} waiter_state;

static asx_status waiter_poll(void *ud, asx_task_id self) {
    waiter_state *w = (waiter_state *)ud;
    (void)self;
    w->polls++;
    return asx_process_poll_status(w->h, &w->status);
}

typedef struct {
    asx_process_handle h;
    asx_sleep_state sleep;
    int killed;
} killer_state;

static asx_status killer_poll(void *ud, asx_task_id self) {
    killer_state *k = (killer_state *)ud;
    asx_status st = asx_sleep_poll(&k->sleep, self);
    if (st != ASX_OK) return st;
    k->killed = 1;
    return asx_process_kill(k->h, 0);
}

TEST(kill_sleep_reports_signal_termination) {
    asx_region_id r;
    asx_task_id t;
    waiter_state *w = NULL;
    killer_state *k = NULL;
    asx_process_command cmd;
    asx_budget budget;
    asx_time start = 0;
    asx_time end = 0;

    ASSERT_TRUE(setup());
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_task_spawn_captured(r, waiter_poll, (uint32_t)sizeof(waiter_state), NULL, &t,
                                      (void **)&w),
              ASX_OK);
    ASSERT_EQ(asx_task_spawn_captured(r, killer_poll, (uint32_t)sizeof(killer_state), NULL, &t,
                                      (void **)&k),
              ASX_OK);
    asx_process_command_init(&cmd, "/bin/sleep");
    ASSERT_EQ(asx_process_command_arg(&cmd, "30"), ASX_OK);
    asx_process_command_stdin(&cmd, ASX_PROCESS_STDIO_NULL);
    asx_process_command_stdout(&cmd, ASX_PROCESS_STDIO_NULL);
    ASSERT_EQ(asx_process_command_spawn(&cmd, &w->h), ASX_OK);
    k->h = w->h;
    ASSERT_EQ(asx_sleep_init(&k->sleep, 20u * 1000000u), ASX_OK);

    ASSERT_EQ(asx_runtime_now_ns(&start), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_OK);
    ASSERT_EQ(asx_runtime_now_ns(&end), ASX_OK);
    ASSERT_TRUE(k->killed);
    ASSERT_EQ(w->status.state, ASX_PROCESS_TERMINATED);
    ASSERT_EQ(w->status.signal, (int32_t)SIGKILL);
    ASSERT_EQ(w->status.code, 128 + (int32_t)SIGKILL);
    ASSERT_TRUE(w->polls <= 8u);
    ASSERT_TRUE(end - start < (asx_time)5000000000u); /* not the full 30 s */
    /* Killing an already reaped child is a harmless no-op. */
    ASSERT_EQ(asx_process_kill(w->h, 0), ASX_OK);
    ASSERT_EQ(asx_process_release(w->h), ASX_OK);
}

TEST(request_shutdown_sends_sigterm) {
    asx_process_command cmd;
    asx_process_handle h;
    asx_process_output out;
    int32_t code = 0;

    ASSERT_TRUE(setup());
    asx_process_command_init(&cmd, "/bin/sleep");
    ASSERT_EQ(asx_process_command_arg(&cmd, "30"), ASX_OK);
    ASSERT_EQ(asx_process_command_spawn(&cmd, &h), ASX_OK);
    ASSERT_EQ(asx_process_poll_wait(h, &code), ASX_E_PENDING); /* outside a task: no park */
    ASSERT_EQ(asx_process_request_shutdown(h), ASX_OK);
    ASSERT_EQ(asx_process_wait_with_output(h, &out), ASX_OK);
    ASSERT_EQ(out.term_signal, (int32_t)SIGTERM);
    ASSERT_EQ(out.exit_code, 128 + (int32_t)SIGTERM);
    ASSERT_EQ(asx_process_poll_wait(h, &code), ASX_OK);
    ASSERT_EQ(code, 128 + (int32_t)SIGTERM);
    ASSERT_EQ(asx_process_release(h), ASX_OK);
}

/* -------------------------------------------------------------------
 * Environment and working directory
 * ------------------------------------------------------------------- */

TEST(env_and_cwd_are_honored) {
    asx_process_command cmd;
    asx_process_output out;
    char tmpl[] = "/tmp/asx-proc-cwd-XXXXXX";
    char *real;
    size_t rlen;

    ASSERT_TRUE(setup());
    ASSERT_TRUE(mkdtemp(tmpl) != NULL);
    real = realpath(tmpl, NULL);
    ASSERT_TRUE(real != NULL);
    rlen = strlen(real);

    /* Inherited environment with one override and one removal. */
    ASSERT_EQ(setenv("ASX_TEST_INHERITED", "kept", 1), 0);
    ASSERT_EQ(setenv("ASX_TEST_REMOVED", "gone", 1), 0);
    asx_process_command_init(&cmd, "/bin/sh");
    ASSERT_EQ(asx_process_command_arg(&cmd, "-c"), ASX_OK);
    ASSERT_EQ(asx_process_command_arg(&cmd,
                                      "printf '%s|%s|%s|' \"$ASX_FOO\" "
                                      "\"$ASX_TEST_INHERITED\" \"${ASX_TEST_REMOVED-unset}\"; "
                                      "pwd -P"),
              ASX_OK);
    ASSERT_EQ(asx_process_command_env(&cmd, "ASX_FOO", "bar"), ASX_OK);
    ASSERT_EQ(asx_process_command_env_remove(&cmd, "ASX_TEST_REMOVED"), ASX_OK);
    ASSERT_EQ(asx_process_command_current_dir(&cmd, tmpl), ASX_OK);
    asx_process_command_stdout(&cmd, ASX_PROCESS_STDIO_PIPED);
    ASSERT_EQ(run_blocking(&cmd, &out), ASX_OK);
    ASSERT_EQ(out.exit_code, 0);
    ASSERT_TRUE(out.stdout_len == strlen("bar|kept|unset|") + rlen + 1u);
    ASSERT_EQ(memcmp(out.stdout_data, "bar|kept|unset|", 15u), 0);
    ASSERT_EQ(memcmp(out.stdout_data + 15, real, rlen), 0);

    /* A cleared environment contains exactly what was set. */
    asx_process_command_init(&cmd, "/usr/bin/env");
    asx_process_command_env_clear(&cmd);
    ASSERT_EQ(asx_process_command_env(&cmd, "ONLY", "1"), ASX_OK);
    asx_process_command_stdout(&cmd, ASX_PROCESS_STDIO_PIPED);
    ASSERT_EQ(run_blocking(&cmd, &out), ASX_OK);
    ASSERT_EQ(out.exit_code, 0);
    ASSERT_EQ(out.stdout_len, 7u);
    ASSERT_EQ(memcmp(out.stdout_data, "ONLY=1\n", 7u), 0);

    ASSERT_EQ(rmdir(tmpl), 0);
    free(real);
    ASSERT_EQ(unsetenv("ASX_TEST_INHERITED"), 0);
    ASSERT_EQ(unsetenv("ASX_TEST_REMOVED"), 0);
}

/* -------------------------------------------------------------------
 * Pipes: no SIGPIPE on a dead reader; large output truncation
 * ------------------------------------------------------------------- */

TEST(write_to_exited_child_reports_disconnected_not_sigpipe) {
    asx_process_command cmd;
    asx_process_handle h;
    asx_process_exit_status status;
    uint32_t n = 0;
    asx_status st;
    int i;

    ASSERT_TRUE(setup());
    asx_process_command_init(&cmd, "/bin/sh");
    ASSERT_EQ(asx_process_command_arg(&cmd, "-c"), ASX_OK);
    ASSERT_EQ(asx_process_command_arg(&cmd, "exit 0"), ASX_OK);
    asx_process_command_stdin(&cmd, ASX_PROCESS_STDIO_PIPED);
    ASSERT_EQ(asx_process_command_spawn(&cmd, &h), ASX_OK);
    /* Wait (outside a task) until the child has exited and been reaped. */
    for (i = 0; i < 500; i++) {
        st = asx_process_poll_status(h, &status);
        if (st == ASX_OK) break;
        ASSERT_EQ(st, ASX_E_PENDING);
        sleep_ms(2);
    }
    ASSERT_EQ(asx_process_poll_status(h, &status), ASX_OK);
    ASSERT_EQ(asx_process_stdin_write(h, (const uint8_t *)"data", 4u, &n), ASX_E_DISCONNECTED);
    ASSERT_EQ(asx_process_stdin_close(h), ASX_OK);
    ASSERT_EQ(asx_process_stdin_close(h), ASX_OK); /* idempotent */
    ASSERT_EQ(asx_process_release(h), ASX_OK);
}

TEST(large_output_is_drained_and_truncated) {
    asx_process_command cmd;
    asx_process_output out;

    ASSERT_TRUE(setup());
    asx_process_command_init(&cmd, "/bin/sh");
    ASSERT_EQ(asx_process_command_arg(&cmd, "-c"), ASX_OK);
    ASSERT_EQ(asx_process_command_arg(&cmd, "head -c 200000 /dev/zero; echo done 1>&2"), ASX_OK);
    asx_process_command_stdout(&cmd, ASX_PROCESS_STDIO_PIPED);
    asx_process_command_stderr(&cmd, ASX_PROCESS_STDIO_PIPED);
    ASSERT_EQ(run_blocking(&cmd, &out), ASX_OK);
    ASSERT_EQ(out.exit_code, 0);
    ASSERT_EQ(out.success, 1);
    ASSERT_EQ(out.stdout_len, ASX_PROCESS_OUTPUT_MAX);
    ASSERT_EQ(out.stdout_truncated, 1u);
    ASSERT_EQ(out.stderr_len, 5u);
    ASSERT_EQ(out.stderr_truncated, 0u);
}

/* -------------------------------------------------------------------
 * Drop semantics: kill_on_drop reaps; detached children are reaped too
 * ------------------------------------------------------------------- */

TEST(release_kill_on_drop_kills_and_reaps) {
    asx_process_command cmd;
    asx_process_handle h;
    uint32_t pid;

    ASSERT_TRUE(setup());
    asx_process_command_init(&cmd, "/bin/sleep");
    ASSERT_EQ(asx_process_command_arg(&cmd, "30"), ASX_OK);
    asx_process_command_kill_on_drop(&cmd, 1u);
    ASSERT_EQ(asx_process_command_spawn(&cmd, &h), ASX_OK);
    pid = asx_process_id(h);
    ASSERT_TRUE(pid > 0u);
    ASSERT_FALSE(pid_gone(pid));
    ASSERT_EQ(asx_process_release(h), ASX_OK);
    ASSERT_TRUE(pid_gone(pid)); /* killed and reaped: no zombie left */
    ASSERT_EQ(asx_process_release(h), ASX_E_NOT_FOUND);
}

TEST(released_running_child_is_reaped_later) {
    asx_process_command cmd;
    asx_process_handle h;
    uint32_t pid;
    int i;

    ASSERT_TRUE(setup());
    asx_process_command_init(&cmd, "/bin/sh");
    ASSERT_EQ(asx_process_command_arg(&cmd, "-c"), ASX_OK);
    ASSERT_EQ(asx_process_command_arg(&cmd, "sleep 0.05"), ASX_OK);
    ASSERT_EQ(asx_process_command_spawn(&cmd, &h), ASX_OK);
    pid = asx_process_id(h);
    ASSERT_EQ(asx_process_release(h), ASX_OK); /* still running: detached */
    ASSERT_FALSE(pid_gone(pid));
    for (i = 0; i < 400 && !pid_gone(pid); i++) {
        sleep_ms(5);
        asx_process_reset(); /* any process operation reaps detached children */
    }
    ASSERT_TRUE(pid_gone(pid));
}

TEST(reset_kills_kill_on_drop_children) {
    asx_process_command cmd;
    asx_process_handle h;
    uint32_t pid;

    ASSERT_TRUE(setup());
    asx_process_command_init(&cmd, "/bin/sleep");
    ASSERT_EQ(asx_process_command_arg(&cmd, "30"), ASX_OK);
    asx_process_command_kill_on_drop(&cmd, 1u);
    ASSERT_EQ(asx_process_command_spawn(&cmd, &h), ASX_OK);
    pid = asx_process_id(h);
    asx_runtime_shutdown(&g_rt); /* runtime reset releases every handle */
    ASSERT_TRUE(pid_gone(pid));
    ASSERT_FALSE(asx_process_is_alive(h));
}

#endif /* !ASX_DETERMINISTIC */

static int run_native(void) {
    RUN_TEST(backend_selection_and_reset);
    RUN_TEST(blocking_wait_with_output_captures_everything);
    RUN_TEST(spawn_failures_leave_nothing_behind);
#if !ASX_DETERMINISTIC
    RUN_TEST(sh_output_and_exit_code_with_parked_task);
    RUN_TEST(stdin_piped_to_cat_round_trips);
    RUN_TEST(kill_sleep_reports_signal_termination);
    RUN_TEST(request_shutdown_sends_sigterm);
    RUN_TEST(env_and_cwd_are_honored);
    RUN_TEST(write_to_exited_child_reports_disconnected_not_sigpipe);
    RUN_TEST(large_output_is_drained_and_truncated);
    RUN_TEST(release_kill_on_drop_kills_and_reaps);
    RUN_TEST(released_running_child_is_reaped_later);
    RUN_TEST(reset_kills_kill_on_drop_children);
#else
    fprintf(stderr, "  SKIP: task-driven process scenarios need a live reactor\n");
#endif
    asx_process_reset();
    return 0;
}

#elif ASX_HAS_NATIVE_RUNTIME_SURFACES

/* Non-POSIX builds keep the simulation; NATIVE is refused. */
TEST(native_backend_unavailable_without_posix) {
    asx_process_reset();
    ASSERT_EQ(asx_process_get_backend(), ASX_PROCESS_BACKEND_MEMORY);
    ASSERT_EQ(asx_process_set_backend(ASX_PROCESS_BACKEND_NATIVE), ASX_E_PERMISSION_DENIED);
    ASSERT_EQ(asx_process_get_backend(), ASX_PROCESS_BACKEND_MEMORY);
}

static int run_native(void) {
    fprintf(stderr, "  SKIP: native process scenarios need a POSIX build\n");
    RUN_TEST(native_backend_unavailable_without_posix);
    return 0;
}

#else

TEST(process_surface_hidden_in_browser) { ASSERT_EQ(ASX_HAS_NATIVE_RUNTIME_SURFACES, 0); }

static int run_native(void) {
    RUN_TEST(process_surface_hidden_in_browser);
    return 0;
}

#endif

int main(void) {
    fprintf(stderr, "=== test_process_native ===\n");
#if defined(ASX_PROFILE_POSIX) && !ASX_DETERMINISTIC
    (void)alarm(60u); /* a lost wakeup fails the suite instead of hanging it */
#endif
    (void)run_native();
    TEST_REPORT();
    return test_failures;
}
