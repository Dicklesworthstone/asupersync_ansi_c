/*
 * e2e_native_host.c — smoke lane for native host surfaces
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/bytes/buf.h>
#include <asx/fs/fs.h>
#include <asx/process/process.h>
#include <asx/signal/signal.h>
#include <stdio.h>

static unsigned long long mix_u64(unsigned long long state, unsigned long long v) {
    state ^= v + 0x9e3779b97f4a7c15ULL + (state << 6) + (state >> 2);
    return state;
}

int main(void) {
    asx_fs_path dir_path, file_path;
    asx_file_handle file;
    asx_fs_metadata meta;
    asx_buf payload;
    asx_buf_mut dst;
    asx_process_handle process;
    asx_process_spawn_options opts;
    asx_signal_subscription subscription;
    uint32_t count = 0u;
    uint32_t n = 0u;
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

    if (asx_fs_file_close(file) != ASX_OK) {
        printf("SCENARIO native_host.fs_close fail close_failed\n");
        return 1;
    }
    printf("SCENARIO native_host.fs_close pass\n");
    digest = mix_u64(digest, 5u);

    printf("DIGEST %016llx\n", digest);
    return 0;
}
