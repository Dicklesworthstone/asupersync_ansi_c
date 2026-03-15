/*
 * e2e_native_host.c — smoke lane for native host surfaces
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <stdio.h>
#if defined(__unix__) || defined(__APPLE__)
#include <poll.h>
#include <unistd.h>
#endif

static unsigned long long mix_u64(unsigned long long state, unsigned long long v) {
    state ^= v + 0x9e3779b97f4a7c15ULL + (state << 6) + (state >> 2);
    return state;
}

static uint64_t blocking_add_seven(void *user_data) {
    uint64_t *value = (uint64_t *)user_data;
    return *value + 7u;
}

static asx_status lab_region_roundtrip(asx_lab *lab, void *user_data) {
    asx_region_id region;

    (void)user_data;
    asx_lab_advance_time(lab, 3u);
    if (asx_lab_open_region(lab, &region) != ASX_OK) { return ASX_E_INVALID_STATE; }
    return asx_region_close(region);
}

#if defined(__unix__) || defined(__APPLE__)
typedef struct {
    int read_fd;
} native_reactor_ctx;

static asx_time native_wall_clock(void *ctx) {
    uint64_t *now_ns = (uint64_t *)ctx;
    *now_ns += 1000000u;
    return *now_ns;
}

static asx_status native_poll_wait(void *ctx, uint32_t timeout_ms, uint32_t *ready_count) {
    native_reactor_ctx *native = (native_reactor_ctx *)ctx;
    struct pollfd pfd;
    int rc;

    if (native == NULL || ready_count == NULL || native->read_fd < 0) { return ASX_E_INVALID_ARGUMENT; }

    pfd.fd = native->read_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    rc = poll(&pfd, 1, (int)timeout_ms);
    if (rc < 0) { return ASX_E_INVALID_STATE; }

    *ready_count = (rc > 0 && (pfd.revents & POLLIN) != 0) ? 1u : 0u;
    return ASX_OK;
}

static asx_status native_poll_ghost(void *ctx, uint64_t logical_step, uint32_t *ready_count) {
    (void)logical_step;
    return native_poll_wait(ctx, 0u, ready_count);
}
#endif

int main(void) {
    asx_runtime_builder builder;
    asx_runtime runtime;
    asx_runtime_config runtime_cfg;
    asx_blocking_handle blocking_handle;
    asx_lab lab;
    asx_lab_config lab_cfg;
    asx_lab_scenario lab_scenario;
    asx_lab_result lab_result;
    asx_fs_path dir_path, file_path;
    asx_file_handle file;
    asx_fs_metadata meta;
    asx_buf payload;
    asx_buf_mut dst;
    asx_process_handle process;
    asx_process_spawn_options opts;
    asx_signal_subscription subscription;
    uint64_t blocking_input = 35u;
    uint64_t blocking_result = 0u;
    uint64_t lab_random = 0u;
    uint32_t region_capacity = 0u;
    uint32_t task_capacity = 0u;
    uint32_t obligation_capacity = 0u;
    uint32_t count = 0u;
    uint32_t n = 0u;
    uint32_t ready = 0u;
    int32_t exit_code = -1;
    unsigned long long digest = 0xcbf29ce484222325ULL;

    asx_fs_reset();
    asx_process_reset();
    asx_signal_reset();

    if (asx_fs_path_from_cstr(&dir_path, "/service") != ASX_OK ||
        asx_fs_dir_create(&dir_path) != ASX_OK) {
        printf("SCENARIO native_host.fs_dir fail unable_to_create_dir\n");
        return 1;
    }
    printf("SCENARIO native_host.fs_dir pass\n");
    digest = mix_u64(digest, 1u);

    if (asx_fs_path_from_cstr(&file_path, "/service/config.json") != ASX_OK ||
        asx_fs_file_open(&file, &file_path,
                         ASX_FS_OPEN_CREATE | ASX_FS_OPEN_READ | ASX_FS_OPEN_WRITE) != ASX_OK) {
        printf("SCENARIO native_host.fs_open fail unable_to_open_file\n");
        return 1;
    }
    payload = asx_buf_from_cstr("{\"mode\":\"native\"}");
    if (asx_fs_file_poll_write(file, &payload, &n) != ASX_OK || n != payload.len) {
        printf("SCENARIO native_host.fs_write fail partial_write\n");
        return 1;
    }
    if (asx_fs_file_rewind(file) != ASX_OK) {
        printf("SCENARIO native_host.fs_rewind fail rewind_failed\n");
        return 1;
    }
    asx_buf_mut_init(&dst);
    if (asx_fs_file_poll_read(file, &dst, &n) != ASX_OK || n != payload.len ||
        !asx_buf_eq(asx_buf_mut_freeze(&dst), payload)) {
        printf("SCENARIO native_host.fs_roundtrip fail data_mismatch\n");
        return 1;
    }
    printf("SCENARIO native_host.fs_roundtrip pass\n");
    digest = mix_u64(digest, (unsigned long long)payload.len);

    if (asx_fs_metadata_query(&file_path, &meta) != ASX_OK || meta.size != payload.len) {
        printf("SCENARIO native_host.fs_metadata fail metadata_wrong\n");
        return 1;
    }
    printf("SCENARIO native_host.fs_metadata pass\n");
    digest = mix_u64(digest, (unsigned long long)meta.size);

    if (asx_fs_file_close(file) != ASX_OK) {
        printf("SCENARIO native_host.fs_close fail close_failed\n");
        return 1;
    }
    printf("SCENARIO native_host.fs_close pass\n");
    digest = mix_u64(digest, 5u);

    opts.program = "native-worker";
    opts.polls_until_exit = 1u;
    opts.exit_code = 7;
    opts.auto_exit = 1;
    if (asx_process_spawn(&process, &opts) != ASX_OK) {
        printf("SCENARIO native_host.process_spawn fail spawn_failed\n");
        return 1;
    }
    if (asx_process_poll_wait(process, &exit_code) != ASX_E_PENDING ||
        asx_process_poll_wait(process, &exit_code) != ASX_OK || exit_code != 7) {
        printf("SCENARIO native_host.process_wait fail wrong_exit\n");
        return 1;
    }
    printf("SCENARIO native_host.process_wait pass\n");
    digest = mix_u64(digest, (unsigned long long)(unsigned int)exit_code);

    if (asx_signal_subscribe(&subscription, ASX_SIGNAL_TERM) != ASX_OK ||
        asx_signal_raise(ASX_SIGNAL_TERM) != ASX_OK ||
        asx_signal_poll(subscription, &count) != ASX_OK || count != 1u ||
        !asx_signal_shutdown_requested()) {
        printf("SCENARIO native_host.signal_shutdown fail signal_contract\n");
        return 1;
    }
    printf("SCENARIO native_host.signal_shutdown pass\n");
    digest = mix_u64(digest, (unsigned long long)count);

    if (asx_runtime_builder_init_current_thread(&builder) != ASX_OK ||
        asx_runtime_builder_set_finalizer_poll_budget(&builder, 48u) != ASX_OK ||
        asx_runtime_builder_build(&builder, &runtime) != ASX_OK ||
        !asx_runtime_is_initialized(&runtime) ||
        asx_runtime_get_config(&runtime, &runtime_cfg) != ASX_OK ||
        runtime_cfg.wait_policy != ASX_WAIT_BUSY_SPIN ||
        runtime_cfg.finalizer_poll_budget != 48u) {
        printf("SCENARIO native_host.builder fail builder_contract\n");
        return 1;
    }
    printf("SCENARIO native_host.builder pass\n");
    digest = mix_u64(digest, (unsigned long long)runtime_cfg.finalizer_poll_budget);

    region_capacity = asx_runtime_region_capacity();
    task_capacity = asx_runtime_task_capacity();
    obligation_capacity = asx_runtime_obligation_capacity();
    if (asx_runtime_region_count(&runtime) != 0u || asx_runtime_task_count(&runtime) != 0u ||
        asx_runtime_obligation_count(&runtime) != 0u || region_capacity == 0u ||
        task_capacity == 0u || obligation_capacity == 0u ||
        asx_runtime_safety_profile(&runtime) != asx_safety_profile_active() ||
        asx_runtime_containment_policy(&runtime) != asx_containment_policy_active()) {
        printf("SCENARIO native_host.runtime_state fail state_contract\n");
        asx_runtime_shutdown(&runtime);
        return 1;
    }
    printf("SCENARIO native_host.runtime_state pass\n");
    digest = mix_u64(digest, (unsigned long long)region_capacity);
    digest = mix_u64(digest, (unsigned long long)task_capacity);
    digest = mix_u64(digest, (unsigned long long)obligation_capacity);

    if (asx_spawn_blocking(blocking_add_seven, &blocking_input, NULL, &blocking_handle) != ASX_OK ||
        asx_blocking_get_state(&blocking_handle) != ASX_BLOCKING_COMPLETED ||
        asx_blocking_get_result(&blocking_handle, &blocking_result) != ASX_OK ||
        blocking_result != 42u || asx_blocking_active_count() != 0u) {
        printf("SCENARIO native_host.spawn_blocking fail blocking_contract\n");
        asx_runtime_shutdown(&runtime);
        return 1;
    }
    printf("SCENARIO native_host.spawn_blocking pass\n");
    digest = mix_u64(digest, (unsigned long long)blocking_result);

    asx_runtime_shutdown(&runtime);

    asx_lab_config_init(&lab_cfg);
    lab_cfg.seed = 42u;
    lab_cfg.tick_ns = 1000u;

    if (asx_lab_init(&lab, &lab_cfg) != ASX_OK) {
        printf("SCENARIO native_host.lab fail init_failed\n");
        return 1;
    }

    lab_random = asx_lab_random_u64(&lab);
    asx_lab_scenario_init(&lab_scenario, "native_host.lab");
    if (asx_lab_scenario_add_step(&lab_scenario, lab_region_roundtrip, NULL) != ASX_OK ||
        asx_lab_run_scenario(&lab, &lab_scenario, &lab_result) != ASX_OK ||
        lab_result.steps_completed != 1u || lab_result.steps_total != 1u ||
        lab_result.elapsed_ns != 3000u || asx_lab_now(&lab) != 3000u || lab_random == 0u) {
        printf("SCENARIO native_host.lab fail scenario_contract\n");
        asx_lab_shutdown(&lab);
        return 1;
    }
    printf("SCENARIO native_host.lab pass\n");
    digest = mix_u64(digest, (unsigned long long)lab_random);

    asx_lab_shutdown(&lab);

#if defined(__unix__) || defined(__APPLE__)
    {
        asx_runtime_hooks hooks;
        native_reactor_ctx reactor_ctx;
        uint64_t now_ns = 0u;
        int pipe_fds[2] = {-1, -1};
        const char byte = 'r';

        if (pipe(pipe_fds) != 0) {
            printf("SCENARIO native_host.reactor fail pipe_setup_failed\n");
            return 1;
        }

        if (asx_runtime_hooks_init(&hooks) != ASX_OK) {
            printf("SCENARIO native_host.reactor fail hooks_init_failed\n");
            close(pipe_fds[0]);
            close(pipe_fds[1]);
            return 1;
        }

        reactor_ctx.read_fd = pipe_fds[0];
        hooks.clock.ctx = &now_ns;
        hooks.clock.now_ns_fn = native_wall_clock;
        hooks.clock.logical_now_ns_fn = native_wall_clock;
        hooks.reactor.ctx = &reactor_ctx;
        hooks.reactor.wait_fn = native_poll_wait;
        hooks.reactor.ghost_wait_fn = native_poll_ghost;

        if (asx_runtime_set_hooks(&hooks) != ASX_OK) {
            printf("SCENARIO native_host.reactor fail hook_install_failed\n");
            close(pipe_fds[0]);
            close(pipe_fds[1]);
            return 1;
        }

        if (write(pipe_fds[1], &byte, 1u) != 1) {
            printf("SCENARIO native_host.reactor fail pipe_write_failed\n");
            close(pipe_fds[0]);
            close(pipe_fds[1]);
            return 1;
        }

        if (asx_runtime_reactor_wait(0u, &ready, 0u) != ASX_OK || ready != 1u) {
            printf("SCENARIO native_host.reactor fail readiness_not_observed\n");
            close(pipe_fds[0]);
            close(pipe_fds[1]);
            return 1;
        }

        close(pipe_fds[0]);
        close(pipe_fds[1]);
        printf("SCENARIO native_host.reactor pass\n");
        digest = mix_u64(digest, (unsigned long long)ready);
    }
#else
    printf("SCENARIO native_host.reactor pass unsupported_platform_skip\n");
    digest = mix_u64(digest, 6u);
#endif

    printf("DIGEST %016llx\n", digest);
    return 0;
}
