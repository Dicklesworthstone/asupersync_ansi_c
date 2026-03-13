/*
 * e2e_server_shutdown.c — native server/bootstrap/shutdown acceptance lane
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/app/app.h>
#include <asx/fs/fs.h>
#include <asx/bytes/buf.h>
#include <asx/signal/signal.h>
#include <stdio.h>
#include <string.h>

static asx_status server_poll(void *data, asx_task_id self)
{
    int *remaining = (int *)data;
    (void)self;
    if (*remaining > 0) {
        (*remaining)--;
        return ASX_E_PENDING;
    }
    return ASX_OK;
}

static void record_digest(unsigned long long *digest, unsigned long long value)
{
    *digest ^= value + 0x9e3779b97f4a7c15ULL + (*digest << 6) + (*digest >> 2);
}

int main(void)
{
    asx_app app;
    asx_app_config config;
    asx_app_server_config server;
    asx_app_server_report report;
    asx_report_buf summary;
    asx_fs_path path;
    asx_file_handle file;
    asx_buf payload;
    uint32_t n = 0u;
    int polls_remaining = 2;
    unsigned long long digest = 0xcbf29ce484222325ULL;

    memset(&config, 0, sizeof(config));
    memset(&server, 0, sizeof(server));
    config.name = "server-e2e";
    config.poll_budget = 64;

    if (asx_app_init(&app, &config) != ASX_OK) {
        printf("SCENARIO server.init fail init_failed\n");
        return 1;
    }
    printf("SCENARIO server.init pass\n");
    record_digest(&digest, 1u);

    if (asx_fs_path_from_cstr(&path, "/service/server.toml") != ASX_OK ||
        asx_fs_file_open(&file, &path, ASX_FS_OPEN_CREATE | ASX_FS_OPEN_WRITE) != ASX_OK) {
        printf("SCENARIO server.config_create fail config_create\n");
        return 1;
    }
    payload = asx_buf_from_cstr("mode=serve\nport=8080\n");
    if (asx_fs_file_poll_write(file, &payload, &n) != ASX_OK ||
        asx_fs_file_close(file) != ASX_OK) {
        printf("SCENARIO server.config_write fail config_write\n");
        return 1;
    }
    printf("SCENARIO server.config_write pass\n");
    record_digest(&digest, payload.len);

    server.config_path = &path;
    server.require_config = 1;
    server.bootstrap_process_name = "server-bootstrap";
    server.shutdown_signal = ASX_SIGNAL_TERM;
    server.run_poll_budget = 64;

    if (asx_signal_raise(ASX_SIGNAL_TERM) != ASX_OK) {
        printf("SCENARIO server.signal_raise fail signal_raise\n");
        return 1;
    }

    if (asx_app_run_server(&app, &server, server_poll, &polls_remaining, &report, &summary)
        != ASX_EXIT_OK) {
        printf("SCENARIO server.run fail exit_code_%u\n", (unsigned)report.exit_code);
        return 1;
    }
    printf("SCENARIO server.run pass\n");
    record_digest(&digest, (unsigned long long)(unsigned)report.shutdown_requested);

    if (!(report.config_loaded && report.bootstrap_process_spawned && report.shutdown_requested)) {
        printf("SCENARIO server.report fail report_flags\n");
        return 1;
    }
    if (strstr(asx_report_buf_cstr(&summary), "Server Summary:") == NULL ||
        strstr(asx_report_buf_cstr(&summary), "Doctor:") == NULL) {
        printf("SCENARIO server.summary fail summary_missing\n");
        return 1;
    }
    printf("SCENARIO server.summary pass\n");
    record_digest(&digest, (unsigned long long)(unsigned)report.bootstrap_process_exit_code);

    asx_app_shutdown(&app);
    printf("SCENARIO server.shutdown pass\n");
    record_digest(&digest, 5u);

    printf("DIGEST %016llx\n", digest);
    return 0;
}
