/*
 * asx/app/app.h — application bootstrap, run loop, and CLI dispatch
 *
 * Provides the minimal app integration layer:
 *   - asx_app: top-level application object wrapping runtime
 *   - CLI argument parsing with command dispatch
 *   - Run loop that drives a user-provided main task
 *   - Structured exit codes
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_APP_APP_H
#define ASX_APP_APP_H

#include <asx/asx_config.h>
#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <asx/core/budget.h>
#include <asx/cx/cx.h>
#include <asx/app/report.h>
#include <asx/runtime/rt.h>
#include <asx/runtime/runtime.h>

#ifdef __cplusplus
extern "C" {
#endif

#if ASX_HAS_NATIVE_RUNTIME_SURFACES
#include <asx/fs/fs.h>
#include <asx/process/process.h>
#include <asx/signal/signal.h>

/* -------------------------------------------------------------------
 * Exit codes
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_EXIT_OK = 0,
    ASX_EXIT_ERROR = 1,
    ASX_EXIT_USAGE = 2,
    ASX_EXIT_INIT_FAILED = 3,
    ASX_EXIT_TASK_FAILED = 4,
    ASX_EXIT_DOCTOR_FAILED = 5
} asx_exit_code;

/* -------------------------------------------------------------------
 * CLI command
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_APP_CMD_RUN = 0,    /* run the main task */
    ASX_APP_CMD_DOCTOR = 1, /* run diagnostic checks */
    ASX_APP_CMD_REPLAY = 2, /* replay a recorded scenario */
    ASX_APP_CMD_SERVER = 3  /* run managed server/bootstrap flow */
} asx_app_command;

/* -------------------------------------------------------------------
 * Parsed CLI arguments
 * ------------------------------------------------------------------- */

typedef struct {
    asx_app_command command;
    const char *scenario; /* replay scenario name (for REPLAY) */
    uint64_t seed;        /* PRNG seed (0 = default) */
    int verbose;          /* verbosity level */
    int help;             /* 1 if --help requested */
} asx_app_args;

/* -------------------------------------------------------------------
 * Application config
 * ------------------------------------------------------------------- */

#ifndef ASX_APP_DEFAULT_POLL_BUDGET
#define ASX_APP_DEFAULT_POLL_BUDGET 10000u
#endif

typedef struct {
    const char *name;     /* application name for diagnostics */
    uint32_t poll_budget; /* max polls per run (0 = ASX_APP_DEFAULT_POLL_BUDGET) */
} asx_app_config;

/* -------------------------------------------------------------------
 * Application object
 * ------------------------------------------------------------------- */

typedef struct {
    asx_app_config config;
    asx_runtime runtime;
    asx_region_id region;
    int initialized;
    asx_exit_code exit_code;
} asx_app;

typedef struct {
    const asx_fs_path *config_path;
    const char *bootstrap_process_name;
    asx_signal_kind shutdown_signal;
    uint32_t run_poll_budget;
    uint32_t bootstrap_polls_until_exit;
    int32_t bootstrap_exit_code;
    int require_config;
} asx_app_server_config;

typedef struct {
    int config_loaded;
    int bootstrap_process_spawned;
    int signal_subscription_active;
    int shutdown_requested;
    int bootstrap_process_exited;
    int main_task_spawned;
    int32_t bootstrap_process_exit_code;
    asx_status last_status;
    asx_exit_code exit_code;
} asx_app_server_report;

/* -------------------------------------------------------------------
 * Application lifecycle
 * ------------------------------------------------------------------- */

/* Initialize an application with the given config.
 * Sets up the runtime, opens a root region, and prepares for
 * task execution. */
ASX_API ASX_MUST_USE asx_status asx_app_init(asx_app *app, const asx_app_config *config);

/* Parse CLI arguments into an args struct.
 * Recognizes: run, doctor, replay, --seed=N, --verbose, --help.
 * Returns ASX_OK on success. */
ASX_API ASX_MUST_USE asx_status asx_app_parse_args(asx_app_args *args, int argc, const char **argv);

/* Run the application's main task.
 * Spawns the user's poll function as a task in the app's region,
 * drives the scheduler until the task completes or budget is exhausted,
 * and returns the exit code. The caller remains responsible for
 * asx_app_shutdown(). */
ASX_API asx_exit_code asx_app_run(asx_app *app, asx_task_poll_fn main_fn, void *user_data);

/* Run the application's main task under explicit authority.
 * Fails closed if app_cx is invalid, bound to a different region, or
 * lacks ASX_CAP_SPAWN. */
ASX_API asx_exit_code asx_app_run_with_cx(asx_app *app, const asx_cx *app_cx,
                                          asx_task_poll_fn main_fn, void *user_data);

/* Shut down the application and release resources. */
ASX_API void asx_app_shutdown(asx_app *app);

/* Get the app's root region (for spawning additional tasks). */
ASX_API asx_region_id asx_app_region(const asx_app *app);

/* Run a managed native server/bootstrap flow.
 * Loads config from ghost fs when configured, optionally spawns a bootstrap
 * sidecar process, subscribes to a shutdown signal, runs the main task, and
 * emits a human-readable summary plus doctor/inspection diagnostics into
 * out_summary when provided. */
ASX_API asx_exit_code asx_app_run_server(asx_app *app,
                                         const asx_app_server_config *server_config,
                                         asx_task_poll_fn main_fn,
                                         void *user_data,
                                         asx_app_server_report *out_report,
                                         asx_report_buf *out_summary);

/* Run a managed native server/bootstrap flow under explicit authority.
 * Fails closed before side effects if app_cx is invalid, bound to a
 * different region, or lacks ASX_CAP_SPAWN. When out_summary is provided,
 * it includes the same structured summary plus doctor/inspection output as
 * asx_app_run_server(). */
ASX_API asx_exit_code asx_app_run_server_with_cx(asx_app *app,
                                                 const asx_cx *app_cx,
                                                 const asx_app_server_config *server_config,
                                                 asx_task_poll_fn main_fn,
                                                 void *user_data,
                                                 asx_app_server_report *out_report,
                                                 asx_report_buf *out_summary);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ASX_APP_APP_H */
