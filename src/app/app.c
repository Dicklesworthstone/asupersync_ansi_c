/*
 * app.c — application bootstrap, run loop, and CLI dispatch
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/app/app.h>
#include <asx/app/doctor.h>
#include <string.h>

static void asx_app_server_report_reset(asx_app_server_report *report) {
    if (report == NULL) return;
    memset(report, 0, sizeof(*report));
    report->last_status = ASX_OK;
    report->exit_code = ASX_EXIT_ERROR;
}

static void asx_app_server_summary(asx_report_buf *out,
                                   const asx_app_server_report *report) {
    if (out == NULL || report == NULL) return;
    asx_report_buf_init(out);
    asx_report_buf_append(out, "Server Summary:\n");
    asx_report_buf_append(out, "  config_loaded: ");
    asx_report_buf_append(out, report->config_loaded ? "yes" : "no");
    asx_report_buf_append(out, "\n  bootstrap_process: ");
    asx_report_buf_append(out, report->bootstrap_process_spawned ? "yes" : "no");
    asx_report_buf_append(out, "\n  signal_subscription: ");
    asx_report_buf_append(out, report->signal_subscription_active ? "yes" : "no");
    asx_report_buf_append(out, "\n  shutdown_requested: ");
    asx_report_buf_append(out, report->shutdown_requested ? "yes" : "no");
    asx_report_buf_append(out, "\n  main_task_spawned: ");
    asx_report_buf_append(out, report->main_task_spawned ? "yes" : "no");
    asx_report_buf_append(out, "\n  bootstrap_exit_code: ");
    asx_report_buf_append_u32(out, (uint32_t)report->bootstrap_process_exit_code);
    asx_report_buf_append(out, "\n  exit_code: ");
    asx_report_buf_append_u32(out, (uint32_t)report->exit_code);
    asx_report_buf_append(out, "\n");
}

static void asx_app_note_cleanup_status(asx_app_server_report *report, asx_status cleanup_st) {
    if (report == NULL) return;
    if (report->last_status == ASX_OK && cleanup_st != ASX_OK) { report->last_status = cleanup_st; }
}

static asx_status asx_app_validate_cx(const asx_app *app, const asx_cx *app_cx) {
    if (app == NULL || app_cx == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!app->initialized) return ASX_E_INVALID_STATE;
    if (!asx_cx_is_valid(app_cx)) return ASX_E_INVALID_STATE;
    if (asx_cx_region(app_cx) != app->region) return ASX_E_PERMISSION_DENIED;
    if (!asx_cx_has_cap(app_cx, ASX_CAP_SPAWN)) return ASX_E_PERMISSION_DENIED;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* CLI argument parsing                                                */
/* ------------------------------------------------------------------ */

asx_status asx_app_parse_args(asx_app_args *args, int argc, const char **argv) {
    int i;

    if (args == NULL) return ASX_E_INVALID_ARGUMENT;

    /* Defaults */
    args->command = ASX_APP_CMD_RUN;
    args->scenario = NULL;
    args->seed = 0;
    args->verbose = 0;
    args->help = 0;

    for (i = 1; i < argc; i++) {
        if (argv[i] == NULL) continue;

        if (strcmp(argv[i], "run") == 0) {
            args->command = ASX_APP_CMD_RUN;
        } else if (strcmp(argv[i], "doctor") == 0) {
            args->command = ASX_APP_CMD_DOCTOR;
        } else if (strcmp(argv[i], "replay") == 0) {
            args->command = ASX_APP_CMD_REPLAY;
            if (i + 1 < argc && argv[i + 1] != NULL && argv[i + 1][0] != '-') {
                args->scenario = argv[++i];
            } else {
                return ASX_E_INVALID_ARGUMENT;
            }
        } else if (strcmp(argv[i], "server") == 0) {
            args->command = ASX_APP_CMD_SERVER;
        } else if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) {
            args->verbose++;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            args->help = 1;
        } else if (strncmp(argv[i], "--seed=", 7) == 0) {
            /* Simple decimal parse */
            const char *p = argv[i] + 7;
            uint64_t val = 0;
            if (*p == '\0') return ASX_E_INVALID_ARGUMENT;
            while (*p >= '0' && *p <= '9') {
                val = val * 10 + (uint64_t)(*p - '0');
                p++;
            }
            if (*p != '\0') return ASX_E_INVALID_ARGUMENT;
            args->seed = val;
        } else {
            return ASX_E_INVALID_ARGUMENT;
        }
    }

    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Application lifecycle                                               */
/* ------------------------------------------------------------------ */

asx_status asx_app_init(asx_app *app, const asx_app_config *config) {
    asx_status st;

    if (app == NULL || config == NULL) return ASX_E_INVALID_ARGUMENT;

    memset(app, 0, sizeof(*app));
    app->config = *config;

    /* Set default poll budget */
    if (app->config.poll_budget == 0) { app->config.poll_budget = 10000; }

    /* Initialize runtime with defaults */
    st = asx_runtime_init_default(&app->runtime);
    if (st != ASX_OK) return st;

    /* Open root region */
    st = asx_region_open(&app->region);
    if (st != ASX_OK) {
        asx_runtime_shutdown(&app->runtime);
        return st;
    }

    app->initialized = 1;
    return ASX_OK;
}

asx_exit_code asx_app_run(asx_app *app, asx_task_poll_fn main_fn, void *user_data) {
    asx_cx root_cx;
    asx_status st;

    if (app == NULL || main_fn == NULL) return ASX_EXIT_ERROR;
    if (!app->initialized) return ASX_EXIT_INIT_FAILED;

    st = asx_cx_init(&root_cx, app->region, ASX_INVALID_ID, ASX_CAP_SPAWN);
    if (st != ASX_OK) {
        app->exit_code = ASX_EXIT_INIT_FAILED;
        return app->exit_code;
    }

    return asx_app_run_with_cx(app, &root_cx, main_fn, user_data);
}

asx_exit_code asx_app_run_with_cx(asx_app *app, const asx_cx *app_cx, asx_task_poll_fn main_fn,
                                  void *user_data) {
    asx_task_id tid;
    asx_budget budget;
    asx_status st;

    if (app == NULL || app_cx == NULL || main_fn == NULL) return ASX_EXIT_ERROR;
    if (!app->initialized) return ASX_EXIT_INIT_FAILED;
    st = asx_app_validate_cx(app, app_cx);
    if (st != ASX_OK) {
        app->exit_code = ASX_EXIT_TASK_FAILED;
        return app->exit_code;
    }

    /* Spawn main task */
    st = asx_task_spawn(app->region, main_fn, user_data, &tid);
    if (st != ASX_OK) {
        app->exit_code = ASX_EXIT_TASK_FAILED;
        return app->exit_code;
    }

    /* Drive scheduler until main task completes or budget exhausted */
    budget = asx_budget_infinite();
    budget.poll_quota = app->config.poll_budget;

    st = asx_scheduler_run(app->region, &budget);

    if (st == ASX_OK) {
        app->exit_code = ASX_EXIT_OK;
    } else if (st == ASX_E_POLL_BUDGET_EXHAUSTED) {
        app->exit_code = ASX_EXIT_OK; /* normal for bounded runs */
    } else {
        app->exit_code = ASX_EXIT_TASK_FAILED;
    }

    return app->exit_code;
}

void asx_app_shutdown(asx_app *app) {
    if (app == NULL) return;
    if (!app->initialized) return;

    asx_runtime_shutdown(&app->runtime);
    app->initialized = 0;
}

asx_region_id asx_app_region(const asx_app *app) {
    if (app == NULL || !app->initialized) return 0;
    return app->region;
}

asx_exit_code asx_app_run_server(asx_app *app,
                                 const asx_app_server_config *server_config,
                                 asx_task_poll_fn main_fn,
                                 void *user_data,
                                 asx_app_server_report *out_report,
                                 asx_report_buf *out_summary) {
    asx_cx root_cx;
    asx_status st;

    if (app == NULL || main_fn == NULL) return ASX_EXIT_ERROR;
    if (!app->initialized) return ASX_EXIT_INIT_FAILED;

    st = asx_cx_init(&root_cx, app->region, ASX_INVALID_ID, ASX_CAP_SPAWN);
    if (st != ASX_OK) {
        app->exit_code = ASX_EXIT_INIT_FAILED;
        return app->exit_code;
    }

    return asx_app_run_server_with_cx(app, &root_cx, server_config, main_fn, user_data, out_report,
                                      out_summary);
}

asx_exit_code asx_app_run_server_with_cx(asx_app *app,
                                         const asx_cx *app_cx,
                                         const asx_app_server_config *server_config,
                                         asx_task_poll_fn main_fn,
                                         void *user_data,
                                         asx_app_server_report *out_report,
                                         asx_report_buf *out_summary) {
    asx_app_server_config defaults;
    asx_app_server_report local_report;
    asx_app_server_report *report;
    asx_task_id tid;
    asx_budget budget;
    asx_status st;
    asx_signal_subscription subscription;
    int have_subscription = 0;
    asx_process_handle process;
    int have_process = 0;
    int32_t process_exit_code = 0;
    uint32_t signal_count = 0;
    asx_doctor_report doctor;

    if (app == NULL || app_cx == NULL || main_fn == NULL) return ASX_EXIT_ERROR;
    if (!app->initialized) return ASX_EXIT_INIT_FAILED;

    memset(&defaults, 0, sizeof(defaults));
    defaults.shutdown_signal = ASX_SIGNAL_TERM;
    defaults.run_poll_budget = app->config.poll_budget;
    defaults.bootstrap_polls_until_exit = 0;
    defaults.bootstrap_exit_code = 0;

    report = (out_report != NULL) ? out_report : &local_report;
    asx_app_server_report_reset(report);

    if (server_config == NULL) server_config = &defaults;

    st = asx_app_validate_cx(app, app_cx);
    if (st != ASX_OK) {
        report->last_status = st;
        report->exit_code = ASX_EXIT_TASK_FAILED;
        app->exit_code = report->exit_code;
        asx_app_server_summary(out_summary, report);
        return report->exit_code;
    }

    if (server_config->config_path != NULL) {
        asx_fs_metadata meta;
        st = asx_fs_metadata_query(server_config->config_path, &meta);
        if (st != ASX_OK) {
            if (server_config->require_config) {
                report->last_status = st;
                report->exit_code = ASX_EXIT_INIT_FAILED;
                app->exit_code = report->exit_code;
                asx_app_server_summary(out_summary, report);
                return report->exit_code;
            }
        } else {
            report->config_loaded = 1;
        }
    }

    if (server_config->bootstrap_process_name != NULL) {
        asx_process_spawn_options opts;
        memset(&opts, 0, sizeof(opts));
        opts.program = server_config->bootstrap_process_name;
        opts.polls_until_exit = server_config->bootstrap_polls_until_exit;
        opts.exit_code = server_config->bootstrap_exit_code;
        opts.auto_exit = 1;
        st = asx_process_spawn(&process, &opts);
        if (st != ASX_OK) {
            report->last_status = st;
            report->exit_code = ASX_EXIT_INIT_FAILED;
            app->exit_code = report->exit_code;
            asx_app_server_summary(out_summary, report);
            return report->exit_code;
        }
        have_process = 1;
        report->bootstrap_process_spawned = 1;
    }

    st = asx_signal_subscribe(&subscription, server_config->shutdown_signal);
    if (st == ASX_OK) {
        have_subscription = 1;
        report->signal_subscription_active = 1;
    } else {
        report->last_status = st;
        report->exit_code = ASX_EXIT_INIT_FAILED;
        app->exit_code = report->exit_code;
        asx_app_server_summary(out_summary, report);
        return report->exit_code;
    }

    st = asx_task_spawn(app->region, main_fn, user_data, &tid);
    if (st != ASX_OK) {
        asx_status cleanup_st = ASX_OK;
        report->last_status = st;
        report->exit_code = ASX_EXIT_TASK_FAILED;
        if (have_subscription) {
            cleanup_st = asx_signal_unsubscribe(subscription);
            asx_app_note_cleanup_status(report, cleanup_st);
            asx_signal_clear_shutdown();
        }
        app->exit_code = report->exit_code;
        asx_app_server_summary(out_summary, report);
        return report->exit_code;
    }
    report->main_task_spawned = 1;

    budget = asx_budget_infinite();
    budget.poll_quota = server_config->run_poll_budget != 0u
                        ? server_config->run_poll_budget
                        : app->config.poll_budget;
    st = asx_scheduler_run(app->region, &budget);
    report->last_status = st;

    if (have_process) {
        asx_status pst = asx_process_poll_wait(process, &process_exit_code);
        if (pst == ASX_OK) {
            report->bootstrap_process_exited = 1;
            report->bootstrap_process_exit_code = process_exit_code;
        }
    }

    if (asx_signal_poll(subscription, &signal_count) == ASX_OK || asx_signal_shutdown_requested()) {
        report->shutdown_requested = 1;
        if (have_process) {
            asx_status shutdown_st = asx_process_request_shutdown(process);
            asx_app_note_cleanup_status(report, shutdown_st);
            if (asx_process_poll_wait(process, &process_exit_code) == ASX_OK) {
                report->bootstrap_process_exited = 1;
                report->bootstrap_process_exit_code = process_exit_code;
            }
        }
    }

    if (have_subscription) {
        asx_status cleanup_st = asx_signal_unsubscribe(subscription);
        asx_app_note_cleanup_status(report, cleanup_st);
        asx_signal_clear_shutdown();
    }

    if (report->bootstrap_process_exited && report->bootstrap_process_exit_code != 0 && !report->shutdown_requested) {
        report->exit_code = ASX_EXIT_TASK_FAILED;
    } else if (st == ASX_OK || st == ASX_E_POLL_BUDGET_EXHAUSTED) {
        report->exit_code = ASX_EXIT_OK;
    } else {
        report->exit_code = ASX_EXIT_TASK_FAILED;
    }

    app->exit_code = report->exit_code;

    if (out_summary != NULL) {
        asx_app_server_summary(out_summary, report);
        if (asx_doctor_run(&app->runtime, &doctor) == ASX_OK) {
            asx_status doctor_st;
            asx_report_buf_append(out_summary, "\nDoctor:\n");
            doctor_st = asx_report_doctor_text(&doctor, out_summary);
            if (doctor_st != ASX_OK) {
                asx_app_note_cleanup_status(report, doctor_st);
                asx_report_buf_append(out_summary, "  <doctor render failed>\n");
            }
        }
    }

    return report->exit_code;
}
