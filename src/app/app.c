/*
 * app.c — application bootstrap, run loop, and CLI dispatch
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/app/app.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* CLI argument parsing                                                */
/* ------------------------------------------------------------------ */

asx_status asx_app_parse_args(
    asx_app_args *args, int argc, const char **argv)
{
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
            if (i + 1 < argc) {
                args->scenario = argv[++i];
            }
        } else if (strcmp(argv[i], "--verbose") == 0 ||
                   strcmp(argv[i], "-v") == 0) {
            args->verbose++;
        } else if (strcmp(argv[i], "--help") == 0 ||
                   strcmp(argv[i], "-h") == 0) {
            args->help = 1;
        } else if (strncmp(argv[i], "--seed=", 7) == 0) {
            /* Simple decimal parse */
            const char *p = argv[i] + 7;
            uint64_t val = 0;
            while (*p >= '0' && *p <= '9') {
                val = val * 10 + (uint64_t)(*p - '0');
                p++;
            }
            args->seed = val;
        }
    }

    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Application lifecycle                                               */
/* ------------------------------------------------------------------ */

asx_status asx_app_init(asx_app *app, const asx_app_config *config)
{
    asx_status st;

    if (app == NULL || config == NULL) return ASX_E_INVALID_ARGUMENT;

    memset(app, 0, sizeof(*app));
    app->config = *config;

    /* Set default poll budget */
    if (app->config.poll_budget == 0) {
        app->config.poll_budget = 10000;
    }

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

asx_exit_code asx_app_run(
    asx_app *app, asx_task_poll_fn main_fn, void *user_data)
{
    asx_task_id tid;
    asx_budget budget;
    asx_status st;

    if (app == NULL || main_fn == NULL) return ASX_EXIT_ERROR;
    if (!app->initialized) return ASX_EXIT_INIT_FAILED;

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

void asx_app_shutdown(asx_app *app)
{
    if (app == NULL) return;
    if (!app->initialized) return;

    asx_runtime_shutdown(&app->runtime);
    app->initialized = 0;
}

asx_region_id asx_app_region(const asx_app *app)
{
    if (app == NULL || !app->initialized) return 0;
    return app->region;
}
