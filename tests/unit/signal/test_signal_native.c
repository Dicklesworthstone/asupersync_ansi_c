/*
 * test_signal_native.c — real POSIX signals through the self-pipe and the
 * wake-driven scheduler
 *
 * Signals are sent with libc raise(), which delivers to the calling
 * thread before returning, so the handler has always written the
 * self-pipe by the time the next scheduler step runs. Task scenarios
 * assert small poll counts: subscribers park on reactor readiness instead
 * of re-polling.
 *
 * Handler install/restore, shutdown flag and ctrl_c run in every POSIX
 * build (they need no reactor); task wakeups and the app graceful-shutdown
 * integration need the live reactor of non-deterministic builds.
 *
 * SPDX-License-Identifier: MIT
 */

#if defined(ASX_PROFILE_POSIX) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "test_harness.h"
#include <asx/asx.h>
#include <asx/runtime/rt.h>
#include <asx/runtime/runtime.h>
#include <asx/signal/signal.h>
#include <asx/time/sleep.h>
#include <string.h>

#if defined(ASX_PROFILE_POSIX)
#include <asx/app/app.h>
#include <signal.h>
#include <unistd.h>

static volatile sig_atomic_t g_custom_hits;

static void custom_handler(int signo) {
    (void)signo;
    g_custom_hits++;
}

/* Current handler function for `signo` (SIG_DFL / SIG_IGN / a handler). */
static void (*current_handler(int signo))(int) {
    struct sigaction sa;
    if (sigaction(signo, NULL, &sa) != 0) return SIG_ERR;
    return sa.sa_handler;
}

TEST(backend_selection_and_reset) {
    asx_signal_reset();
    ASSERT_EQ(asx_signal_get_backend(), ASX_SIGNAL_BACKEND_MEMORY);
    ASSERT_EQ(asx_signal_set_backend((asx_signal_backend)5), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_signal_set_backend(ASX_SIGNAL_BACKEND_NATIVE), ASX_OK);
    ASSERT_EQ(asx_signal_get_backend(), ASX_SIGNAL_BACKEND_NATIVE);
    asx_signal_reset();
    ASSERT_EQ(asx_signal_get_backend(), ASX_SIGNAL_BACKEND_MEMORY);
}

TEST(subscribe_installs_handler_and_restores_previous) {
    struct sigaction custom;
    struct sigaction original;
    asx_signal_subscription sub;
    uint32_t count = 0;

    memset(&custom, 0, sizeof(custom));
    custom.sa_handler = custom_handler;
    (void)sigemptyset(&custom.sa_mask);
    ASSERT_EQ(sigaction(SIGUSR1, &custom, &original), 0);
    g_custom_hits = 0;

    asx_signal_reset();
    ASSERT_EQ(asx_signal_set_backend(ASX_SIGNAL_BACKEND_NATIVE), ASX_OK);
    ASSERT_EQ(asx_signal_subscribe(&sub, ASX_SIGNAL_USR1), ASX_OK);
    ASSERT_TRUE(current_handler(SIGUSR1) != custom_handler);
    ASSERT_EQ(asx_signal_poll(sub, &count), ASX_E_PENDING);
    ASSERT_EQ(raise(SIGUSR1), 0);
    ASSERT_EQ(g_custom_hits, 0); /* ours ran, not the previous handler */
    ASSERT_EQ(asx_signal_poll(sub, &count), ASX_OK);
    ASSERT_EQ(count, 1u);
    ASSERT_EQ(raise(SIGUSR1), 0);
    ASSERT_EQ(raise(SIGUSR1), 0);
    ASSERT_EQ(asx_signal_poll(sub, &count), ASX_OK);
    ASSERT_EQ(count, 2u);
    ASSERT_EQ(asx_signal_poll(sub, &count), ASX_E_PENDING);

    /* Last unsubscribe restores the previous disposition. */
    ASSERT_EQ(asx_signal_unsubscribe(sub), ASX_OK);
    ASSERT_TRUE(current_handler(SIGUSR1) == custom_handler);
    ASSERT_EQ(raise(SIGUSR1), 0);
    ASSERT_EQ(g_custom_hits, 1);

    /* So does reset, for every kind still subscribed. */
    ASSERT_EQ(asx_signal_subscribe(&sub, ASX_SIGNAL_USR1), ASX_OK);
    ASSERT_EQ(asx_signal_subscribe(&sub, ASX_SIGNAL_USR1), ASX_OK); /* refcounted */
    ASSERT_TRUE(current_handler(SIGUSR1) != custom_handler);
    asx_signal_reset();
    ASSERT_TRUE(current_handler(SIGUSR1) == custom_handler);
    ASSERT_EQ(asx_signal_poll(sub, &count), ASX_E_NOT_FOUND);

    ASSERT_EQ(sigaction(SIGUSR1, &original, NULL), 0);
}

TEST(term_sets_shutdown_flag_before_any_poll) {
    asx_signal_subscription sub;
    uint32_t count = 0;

    asx_signal_reset();
    ASSERT_EQ(asx_signal_set_backend(ASX_SIGNAL_BACKEND_NATIVE), ASX_OK);
    ASSERT_EQ(asx_signal_subscribe(&sub, ASX_SIGNAL_TERM), ASX_OK);
    ASSERT_FALSE(asx_signal_shutdown_requested());
    ASSERT_EQ(raise(SIGTERM), 0); /* caught: the process survives */
    ASSERT_TRUE(asx_signal_shutdown_requested());
    ASSERT_EQ(asx_signal_poll(sub, &count), ASX_OK);
    ASSERT_EQ(count, 1u);
    asx_signal_clear_shutdown();
    ASSERT_FALSE(asx_signal_shutdown_requested());
    /* In-process injection reaches native subscriptions too. */
    ASSERT_EQ(asx_signal_raise(ASX_SIGNAL_TERM), ASX_OK);
    ASSERT_TRUE(asx_signal_shutdown_requested());
    ASSERT_EQ(asx_signal_poll(sub, &count), ASX_OK);
    ASSERT_EQ(count, 1u);
    asx_signal_reset();
    ASSERT_FALSE(asx_signal_shutdown_requested());
}

TEST(ctrl_c_helper_catches_sigint_once) {
    asx_signal_ctrl_c c;
    void (*before)(int);

    asx_signal_reset();
    before = current_handler(SIGINT);
    ASSERT_EQ(asx_signal_set_backend(ASX_SIGNAL_BACKEND_NATIVE), ASX_OK);
    ASSERT_EQ(asx_signal_ctrl_c_init(&c), ASX_OK);
    ASSERT_EQ(asx_signal_ctrl_c_poll(&c), ASX_E_PENDING);
    ASSERT_EQ(raise(SIGINT), 0);
    ASSERT_EQ(asx_signal_ctrl_c_poll(&c), ASX_OK);
    ASSERT_EQ(c.active, 0);
    ASSERT_EQ(asx_signal_ctrl_c_poll(&c), ASX_E_INVALID_STATE);
    ASSERT_TRUE(current_handler(SIGINT) == before); /* listener gone */
    asx_signal_ctrl_c_cancel(&c);                   /* idempotent */
    ASSERT_EQ(asx_signal_ctrl_c_init(&c), ASX_OK);
    asx_signal_ctrl_c_cancel(&c);
    ASSERT_TRUE(current_handler(SIGINT) == before);
    asx_signal_reset();
}

TEST(native_rejects_uncatchable_kinds) {
    asx_signal_subscription sub;
    asx_signal_reset();
    ASSERT_EQ(asx_signal_set_backend(ASX_SIGNAL_BACKEND_NATIVE), ASX_OK);
    ASSERT_EQ(asx_signal_subscribe(&sub, (asx_signal_kind)9), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_signal_set_backend(ASX_SIGNAL_BACKEND_MEMORY), ASX_OK);
    ASSERT_EQ(asx_signal_subscribe(&sub, (asx_signal_kind)9), ASX_OK); /* model accepts any */
    asx_signal_reset();
}

#if !ASX_DETERMINISTIC

static asx_runtime g_rt;

static int setup(void) {
    if (asx_runtime_init_default(&g_rt) != ASX_OK) return 0;
    return asx_signal_get_backend() == ASX_SIGNAL_BACKEND_NATIVE;
}

typedef struct {
    asx_signal_subscription sub;
    uint32_t count;
    uint32_t polls;
} receiver_state;

static asx_status receiver_poll(void *ud, asx_task_id self) {
    receiver_state *r = (receiver_state *)ud;
    (void)self;
    r->polls++;
    return asx_signal_poll(r->sub, &r->count);
}

typedef struct {
    asx_sleep_state sleep;
    int os_signal;            /* raise() this, or 0 */
    asx_signal_kind injected; /* else asx_signal_raise() this */
} sender_state;

static asx_status sender_poll(void *ud, asx_task_id self) {
    sender_state *s = (sender_state *)ud;
    asx_status st = asx_sleep_poll(&s->sleep, self);
    if (st != ASX_OK) return st;
    if (s->os_signal != 0) return raise(s->os_signal) == 0 ? ASX_OK : ASX_E_INVALID_STATE;
    return asx_signal_raise(s->injected);
}

static int spawn_receiver(asx_region_id r, asx_signal_kind kind, receiver_state **out) {
    asx_task_id t;
    if (asx_task_spawn_captured(r, receiver_poll, (uint32_t)sizeof(receiver_state), NULL, &t,
                                (void **)out) != ASX_OK) {
        return 0;
    }
    return asx_signal_subscribe(&(*out)->sub, kind) == ASX_OK;
}

static int spawn_sender(asx_region_id r, uint32_t delay_ms, int os_signal,
                        asx_signal_kind injected) {
    asx_task_id t;
    sender_state *s = NULL;
    if (asx_task_spawn_captured(r, sender_poll, (uint32_t)sizeof(sender_state), NULL, &t,
                                (void **)&s) != ASX_OK) {
        return 0;
    }
    s->os_signal = os_signal;
    s->injected = injected;
    return asx_sleep_init(&s->sleep, (asx_time)delay_ms * 1000000u) == ASX_OK;
}

TEST(raised_sigusr1_wakes_parked_task) {
    asx_region_id r;
    receiver_state *rx = NULL;
    asx_budget budget;

    ASSERT_TRUE(setup());
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_TRUE(spawn_receiver(r, ASX_SIGNAL_USR1, &rx));
    ASSERT_TRUE(spawn_sender(r, 20u, SIGUSR1, ASX_SIGNAL_USR1));
    budget = asx_budget_from_polls(50);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_OK);
    ASSERT_EQ(rx->count, 1u);
    ASSERT_TRUE(rx->polls <= 3u); /* parked until the self-pipe became readable */
    ASSERT_EQ(asx_signal_unsubscribe(rx->sub), ASX_OK);
}

TEST(one_signal_wakes_every_subscriber) {
    asx_region_id r;
    receiver_state *a = NULL;
    receiver_state *b = NULL;
    receiver_state *other = NULL;
    asx_budget budget;

    ASSERT_TRUE(setup());
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_TRUE(spawn_receiver(r, ASX_SIGNAL_USR2, &a));
    ASSERT_TRUE(spawn_receiver(r, ASX_SIGNAL_USR2, &b));
    ASSERT_TRUE(spawn_receiver(r, ASX_SIGNAL_HUP, &other));
    ASSERT_TRUE(spawn_sender(r, 20u, SIGUSR2, ASX_SIGNAL_USR2));
    ASSERT_TRUE(spawn_sender(r, 60u, SIGHUP, ASX_SIGNAL_HUP));
    budget = asx_budget_from_polls(50);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_OK);
    /* One SIGUSR2 reached both USR2 subscribers and no HUP subscriber. */
    ASSERT_EQ(a->count, 1u);
    ASSERT_EQ(b->count, 1u);
    ASSERT_EQ(other->count, 1u);
    ASSERT_TRUE(a->polls <= 3u);
    ASSERT_TRUE(b->polls <= 3u);
    /* The HUP subscriber may see one spurious readiness wake (the pipe is
     * shared) but never completes early. */
    ASSERT_TRUE(other->polls <= 4u);
}

TEST(injected_raise_wakes_parked_native_subscriber) {
    asx_region_id r;
    receiver_state *rx = NULL;
    asx_budget budget;

    ASSERT_TRUE(setup());
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_TRUE(spawn_receiver(r, ASX_SIGNAL_HUP, &rx));
    ASSERT_TRUE(spawn_sender(r, 20u, 0, ASX_SIGNAL_HUP));
    budget = asx_budget_from_polls(50);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_OK);
    ASSERT_EQ(rx->count, 1u);
    ASSERT_TRUE(rx->polls <= 3u);
}

/* -------------------------------------------------------------------
 * App graceful shutdown on a real SIGTERM
 * ------------------------------------------------------------------- */

typedef struct {
    int armed;
    int raised;
    int cancelled;
    uint32_t polls;
} server_main_state;

/* A server that would run forever: after 20 ms it sends itself SIGTERM,
 * then parks with no wake source of its own. Only the app's shutdown
 * watcher (cancelling the region) can end the run. */
static asx_status server_main_poll(void *ud, asx_task_id self) {
    server_main_state *m = (server_main_state *)ud;
    asx_checkpoint_result cp;
    asx_time now = 0;
    m->polls++;
    if (asx_checkpoint(self, &cp) == ASX_OK && cp.cancelled) {
        m->cancelled = 1;
        return ASX_OK;
    }
    if (!m->armed) {
        m->armed = 1;
        if (asx_runtime_now_ns(&now) != ASX_OK) return ASX_E_INVALID_STATE;
        if (asx_task_arm_timer(self, now + 20u * 1000000u) != ASX_OK) return ASX_E_INVALID_STATE;
    } else if (!m->raised) {
        m->raised = 1;
        if (raise(SIGTERM) != 0) return ASX_E_INVALID_STATE;
    }
    if (asx_task_park(self) != ASX_OK) return ASX_E_INVALID_STATE;
    return ASX_E_PENDING;
}

static asx_status quick_main_poll(void *ud, asx_task_id self) {
    (void)ud;
    (void)self;
    return ASX_OK;
}

TEST(app_server_drains_on_real_sigterm) {
    asx_app app;
    asx_app_config config;
    asx_app_server_config server;
    asx_app_server_report report;
    server_main_state m;
    void (*before)(int) = current_handler(SIGTERM);

    memset(&config, 0, sizeof(config));
    memset(&server, 0, sizeof(server));
    memset(&m, 0, sizeof(m));
    config.name = "native-srv";
    config.poll_budget = 200;
    server.shutdown_signal = ASX_SIGNAL_TERM;

    ASSERT_EQ(asx_app_init(&app, &config), ASX_OK);
    ASSERT_EQ(asx_signal_get_backend(), ASX_SIGNAL_BACKEND_NATIVE);
    ASSERT_EQ(asx_app_run_server(&app, &server, server_main_poll, &m, &report, NULL), ASX_EXIT_OK);
    ASSERT_EQ(m.raised, 1);
    ASSERT_EQ(m.cancelled, 1);
    ASSERT_TRUE(m.polls <= 6u);
    ASSERT_EQ(report.signal_subscription_active, 1);
    ASSERT_EQ(report.shutdown_requested, 1);
    ASSERT_FALSE(asx_signal_shutdown_requested());   /* cleared after the run */
    ASSERT_TRUE(current_handler(SIGTERM) == before); /* disposition restored */

    /* A main task that finishes on its own ends the run without shutdown. */
    ASSERT_EQ(asx_app_run_server(&app, &server, quick_main_poll, NULL, &report, NULL), ASX_EXIT_OK);
    ASSERT_EQ(report.shutdown_requested, 0);
    ASSERT_EQ(report.last_status, ASX_OK);
    asx_app_shutdown(&app);
}

#endif /* !ASX_DETERMINISTIC */

static int run_native(void) {
    RUN_TEST(backend_selection_and_reset);
    RUN_TEST(subscribe_installs_handler_and_restores_previous);
    RUN_TEST(term_sets_shutdown_flag_before_any_poll);
    RUN_TEST(ctrl_c_helper_catches_sigint_once);
    RUN_TEST(native_rejects_uncatchable_kinds);
#if !ASX_DETERMINISTIC
    RUN_TEST(raised_sigusr1_wakes_parked_task);
    RUN_TEST(one_signal_wakes_every_subscriber);
    RUN_TEST(injected_raise_wakes_parked_native_subscriber);
    RUN_TEST(app_server_drains_on_real_sigterm);
#else
    fprintf(stderr, "  SKIP: task wake-up scenarios need a live reactor\n");
#endif
    asx_signal_reset();
    return 0;
}

#elif ASX_HAS_NATIVE_RUNTIME_SURFACES

/* Non-POSIX builds keep in-process delivery; NATIVE is refused. */
TEST(native_backend_unavailable_without_posix) {
    asx_signal_reset();
    ASSERT_EQ(asx_signal_get_backend(), ASX_SIGNAL_BACKEND_MEMORY);
    ASSERT_EQ(asx_signal_set_backend(ASX_SIGNAL_BACKEND_NATIVE), ASX_E_PERMISSION_DENIED);
    ASSERT_EQ(asx_signal_get_backend(), ASX_SIGNAL_BACKEND_MEMORY);
}

static int run_native(void) {
    fprintf(stderr, "  SKIP: native signal scenarios need a POSIX build\n");
    RUN_TEST(native_backend_unavailable_without_posix);
    return 0;
}

#else

TEST(signal_surface_hidden_in_browser) { ASSERT_EQ(ASX_HAS_NATIVE_RUNTIME_SURFACES, 0); }

static int run_native(void) {
    RUN_TEST(signal_surface_hidden_in_browser);
    return 0;
}

#endif

int main(void) {
    fprintf(stderr, "=== test_signal_native ===\n");
#if defined(ASX_PROFILE_POSIX) && !ASX_DETERMINISTIC
    (void)alarm(60u); /* a lost wakeup fails the suite instead of hanging it */
#endif
    (void)run_native();
    TEST_REPORT();
    return test_failures;
}
