/*
 * test_reset_hooks.c — the optional subsystems register their reset with the
 * runtime the first time they can hold state (bd-dkfs)
 *
 * The kernel does not name net, fs, process or signal: each registers its
 * reset hook where it first allocates a slot, takes an ephemeral port or
 * selects a backend, and asx_runtime_reset runs the registered hooks in slot
 * order. A place that can hold state and forgets to register would leave it
 * out of every later reset, so each such place is exercised here after
 * every registration was forgotten.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/asx.h>
#include <asx/fs/fs.h>
#include <asx/net/net.h>
#include <asx/process/process.h>
#include <asx/runtime/runtime.h>
#include <asx/signal/signal.h>
#include <string.h>

#define SLOT_COUNT ((unsigned)ASX_RESET_SLOT_COUNT)

static int g_calls;
static unsigned g_order[8];

static void count_hook(void) { g_calls++; }

static void order_hook_net(void) { g_order[g_calls++] = 0u; }
static void order_hook_fs(void) { g_order[g_calls++] = 1u; }
static void order_hook_process(void) { g_order[g_calls++] = 2u; }
static void order_hook_signal(void) { g_order[g_calls++] = 3u; }

static void forget(void) { asx_runtime_reset_hooks_forget_internal(); }

static int registered(asx_reset_slot slot) {
    return asx_runtime_reset_hook_registered_internal(slot);
}

/* ------------------------------------------------------------------ */
/* The table                                                           */
/* ------------------------------------------------------------------ */

TEST(forgetting_empties_every_slot) {
    unsigned i;
    asx_runtime_reset_hook_register_internal(ASX_RESET_SLOT_NET, count_hook);
    asx_runtime_reset_hook_register_internal(ASX_RESET_SLOT_SIGNAL, count_hook);
    ASSERT_TRUE(registered(ASX_RESET_SLOT_NET));
    ASSERT_TRUE(registered(ASX_RESET_SLOT_SIGNAL));
    forget();
    for (i = 0u; i < SLOT_COUNT; i++) ASSERT_FALSE(registered((asx_reset_slot)i));
}

TEST(a_bad_slot_or_hook_is_ignored) {
    unsigned i;
    forget();
    asx_runtime_reset_hook_register_internal(ASX_RESET_SLOT_COUNT, count_hook);
    asx_runtime_reset_hook_register_internal((asx_reset_slot)(SLOT_COUNT + 7u), count_hook);
    asx_runtime_reset_hook_register_internal(ASX_RESET_SLOT_FS, NULL);
    for (i = 0u; i < SLOT_COUNT; i++) ASSERT_FALSE(registered((asx_reset_slot)i));
    ASSERT_FALSE(registered(ASX_RESET_SLOT_COUNT));
}

TEST(a_registered_hook_runs_once_per_reset_and_survives_it) {
    forget();
    g_calls = 0;
    asx_runtime_reset_hook_register_internal(ASX_RESET_SLOT_PROCESS, count_hook);
    asx_runtime_reset_hook_register_internal(ASX_RESET_SLOT_PROCESS, count_hook);
    asx_runtime_reset();
    ASSERT_EQ(g_calls, 1);
    ASSERT_TRUE(registered(ASX_RESET_SLOT_PROCESS));
    asx_runtime_reset();
    ASSERT_EQ(g_calls, 2);
    forget();
    asx_runtime_reset();
    ASSERT_EQ(g_calls, 2);
}

TEST(hooks_run_in_slot_order_whatever_the_registration_order) {
    forget();
    g_calls = 0;
    memset(g_order, 0, sizeof(g_order));
    asx_runtime_reset_hook_register_internal(ASX_RESET_SLOT_SIGNAL, order_hook_signal);
    asx_runtime_reset_hook_register_internal(ASX_RESET_SLOT_NET, order_hook_net);
    asx_runtime_reset_hook_register_internal(ASX_RESET_SLOT_PROCESS, order_hook_process);
    asx_runtime_reset_hook_register_internal(ASX_RESET_SLOT_FS, order_hook_fs);
    asx_runtime_reset();
    ASSERT_EQ(g_calls, 4);
    ASSERT_EQ(g_order[0], 0u);
    ASSERT_EQ(g_order[1], 1u);
    ASSERT_EQ(g_order[2], 2u);
    ASSERT_EQ(g_order[3], 3u);
    forget();
}

/* ------------------------------------------------------------------ */
/* net                                                                 */
/* ------------------------------------------------------------------ */

TEST(net_registers_wherever_it_can_hold_state) {
    asx_socket_addr any = asx_socket_addr_loopback(0u);
    asx_socket_addr at;
    asx_unix_addr path;
    asx_tcp_listener listener;
    asx_tcp_stream client;
    asx_udp_socket udp;
    asx_unix_listener ulistener;
    asx_unix_stream ustream;
    asx_unix_dgram udgram;
    asx_status st;

    ASSERT_EQ(asx_unix_addr_from_path(&path, "/tmp/asx-reset-hooks"), ASX_OK);

    asx_net_reset();
    forget();
    ASSERT_EQ(asx_net_set_backend(ASX_NET_BACKEND_MEMORY), ASX_OK);
    ASSERT_TRUE(registered(ASX_RESET_SLOT_NET));

    asx_net_reset();
    forget();
    st = asx_udp_bind(&udp, &any);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_TRUE(registered(ASX_RESET_SLOT_NET));

    asx_net_reset();
    forget();
    st = asx_tcp_listener_bind(&listener, &any);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_TRUE(registered(ASX_RESET_SLOT_NET));
    ASSERT_EQ(asx_tcp_listener_local_addr(listener, &at), ASX_OK);

    /* A connect allocates its stream (and, with port 0, an ephemeral port). */
    forget();
    st = asx_tcp_connect(&client, &at);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_TRUE(registered(ASX_RESET_SLOT_NET));

    asx_net_reset();
    forget();
    st = asx_unix_listener_bind(&ulistener, &path);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_TRUE(registered(ASX_RESET_SLOT_NET));

    forget();
    st = asx_unix_connect(&ustream, &path);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_TRUE(registered(ASX_RESET_SLOT_NET));

    asx_net_reset();
    forget();
    st = asx_unix_dgram_bind(&udgram, &path);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_TRUE(registered(ASX_RESET_SLOT_NET));

    asx_net_reset();
    forget();
}

TEST(a_runtime_reset_reaches_net_after_its_first_use) {
    asx_socket_addr at = asx_socket_addr_loopback(18080u);
    asx_tcp_listener first;
    asx_tcp_listener second;
    asx_status st;

    asx_net_reset();
    forget();
    st = asx_tcp_listener_bind(&first, &at);
    ASSERT_EQ(st, ASX_OK);
    st = asx_tcp_listener_bind(&second, &at);
    ASSERT_EQ(st, ASX_E_ALREADY_EXISTS);
    asx_runtime_reset();
    st = asx_tcp_listener_bind(&second, &at);
    ASSERT_EQ(st, ASX_OK);
    asx_runtime_reset();
}

#if ASX_HAS_NATIVE_RUNTIME_SURFACES

/* ------------------------------------------------------------------ */
/* fs                                                                  */
/* ------------------------------------------------------------------ */

TEST(fs_registers_wherever_it_can_hold_state) {
    asx_fs_path dir;
    asx_fs_path file;
    asx_fs_dir_handle dh;
    asx_file_handle fh;
    asx_status st;

    ASSERT_EQ(asx_fs_path_from_cstr(&dir, "/reset-hooks"), ASX_OK);
    ASSERT_EQ(asx_fs_path_from_cstr(&file, "/reset-hooks/f"), ASX_OK);

    asx_fs_reset();
    forget();
    ASSERT_EQ(asx_fs_set_backend(ASX_FS_BACKEND_MEMORY), ASX_OK);
    ASSERT_TRUE(registered(ASX_RESET_SLOT_FS));

    asx_fs_reset();
    forget();
    ASSERT_EQ(asx_fs_dir_create(&dir), ASX_OK);
    ASSERT_TRUE(registered(ASX_RESET_SLOT_FS));

    forget();
    st = asx_fs_file_open(&fh, &file, (uint32_t)ASX_FS_OPEN_CREATE | (uint32_t)ASX_FS_OPEN_WRITE);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_TRUE(registered(ASX_RESET_SLOT_FS));
    ASSERT_EQ(asx_fs_file_close(fh), ASX_OK);

    /* Opening what already exists takes a handle slot, and may be native. */
    forget();
    st = asx_fs_file_open(&fh, &file, (uint32_t)ASX_FS_OPEN_READ);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_TRUE(registered(ASX_RESET_SLOT_FS));
    ASSERT_EQ(asx_fs_file_close(fh), ASX_OK);

    forget();
    st = asx_fs_dir_open(&dh, &dir);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_TRUE(registered(ASX_RESET_SLOT_FS));
    ASSERT_EQ(asx_fs_dir_close(dh), ASX_OK);

    asx_fs_reset();
    forget();
}

TEST(a_runtime_reset_reaches_fs_after_its_first_use) {
    asx_fs_path dir;
    asx_fs_reset();
    forget();
    ASSERT_EQ(asx_fs_path_from_cstr(&dir, "/reset-hooks-fs"), ASX_OK);
    ASSERT_EQ(asx_fs_dir_create(&dir), ASX_OK);
    ASSERT_TRUE(asx_fs_exists(&dir));
    asx_runtime_reset();
    ASSERT_FALSE(asx_fs_exists(&dir));
}

/* ------------------------------------------------------------------ */
/* process                                                             */
/* ------------------------------------------------------------------ */

TEST(process_registers_wherever_it_can_hold_state) {
    asx_process_spawn_options opts;
    asx_process_command cmd;
    asx_process_handle h;
    asx_status st;

    memset(&opts, 0, sizeof(opts));
    opts.program = "worker";
    opts.polls_until_exit = 9u;
    opts.auto_exit = 1;

    asx_process_reset();
    forget();
    ASSERT_EQ(asx_process_set_backend(ASX_PROCESS_BACKEND_MEMORY), ASX_OK);
    ASSERT_TRUE(registered(ASX_RESET_SLOT_PROCESS));

    asx_process_reset();
    forget();
    st = asx_process_spawn(&h, &opts);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_TRUE(registered(ASX_RESET_SLOT_PROCESS));

    asx_process_reset();
    forget();
    asx_process_command_init(&cmd, "worker");
    cmd.polls_until_exit = 9u;
    cmd.auto_exit = 1;
    st = asx_process_command_spawn(&cmd, &h);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_TRUE(registered(ASX_RESET_SLOT_PROCESS));

    asx_process_reset();
    forget();
}

TEST(a_runtime_reset_reaches_process_after_its_first_use) {
    asx_process_spawn_options opts;
    asx_process_handle h;
    asx_process_state state;
    asx_status st;

    memset(&opts, 0, sizeof(opts));
    opts.program = "worker";
    opts.polls_until_exit = 9u;
    opts.auto_exit = 1;
    asx_process_reset();
    forget();
    st = asx_process_spawn(&h, &opts);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_EQ(asx_process_state_query(h, &state), ASX_OK);
    asx_runtime_reset();
    ASSERT_TRUE(asx_process_state_query(h, &state) != ASX_OK);
}

/* ------------------------------------------------------------------ */
/* signal                                                              */
/* ------------------------------------------------------------------ */

TEST(signal_registers_wherever_it_can_hold_state) {
    asx_signal_subscription sub;

    asx_signal_reset();
    forget();
    ASSERT_EQ(asx_signal_set_backend(ASX_SIGNAL_BACKEND_MEMORY), ASX_OK);
    ASSERT_TRUE(registered(ASX_RESET_SLOT_SIGNAL));

    asx_signal_reset();
    forget();
    ASSERT_EQ(asx_signal_subscribe(&sub, ASX_SIGNAL_USR1), ASX_OK);
    ASSERT_TRUE(registered(ASX_RESET_SLOT_SIGNAL));

    /* Raising with nobody subscribed still sets the shutdown request. */
    asx_signal_reset();
    forget();
    ASSERT_EQ(asx_signal_raise(ASX_SIGNAL_TERM), ASX_OK);
    ASSERT_TRUE(registered(ASX_RESET_SLOT_SIGNAL));
    ASSERT_TRUE(asx_signal_shutdown_requested());

    asx_signal_reset();
    forget();
}

TEST(a_runtime_reset_reaches_signal_after_its_first_use) {
    asx_signal_reset();
    forget();
    ASSERT_EQ(asx_signal_raise(ASX_SIGNAL_TERM), ASX_OK);
    ASSERT_TRUE(asx_signal_shutdown_requested());
    asx_runtime_reset();
    ASSERT_FALSE(asx_signal_shutdown_requested());
}

#endif /* ASX_HAS_NATIVE_RUNTIME_SURFACES */

int main(void) {
    RUN_TEST(forgetting_empties_every_slot);
    RUN_TEST(a_bad_slot_or_hook_is_ignored);
    RUN_TEST(a_registered_hook_runs_once_per_reset_and_survives_it);
    RUN_TEST(hooks_run_in_slot_order_whatever_the_registration_order);
    RUN_TEST(net_registers_wherever_it_can_hold_state);
    RUN_TEST(a_runtime_reset_reaches_net_after_its_first_use);
#if ASX_HAS_NATIVE_RUNTIME_SURFACES
    RUN_TEST(fs_registers_wherever_it_can_hold_state);
    RUN_TEST(a_runtime_reset_reaches_fs_after_its_first_use);
    RUN_TEST(process_registers_wherever_it_can_hold_state);
    RUN_TEST(a_runtime_reset_reaches_process_after_its_first_use);
    RUN_TEST(signal_registers_wherever_it_can_hold_state);
    RUN_TEST(a_runtime_reset_reaches_signal_after_its_first_use);
#endif
    TEST_REPORT();
    return test_failures;
}
