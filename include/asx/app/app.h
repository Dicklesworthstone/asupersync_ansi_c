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

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <asx/runtime/rt.h>
#include <asx/runtime/runtime.h>
#include <asx/core/budget.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Exit codes
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_EXIT_OK            = 0,
    ASX_EXIT_ERROR         = 1,
    ASX_EXIT_USAGE         = 2,
    ASX_EXIT_INIT_FAILED   = 3,
    ASX_EXIT_TASK_FAILED   = 4,
    ASX_EXIT_DOCTOR_FAILED = 5
} asx_exit_code;

/* -------------------------------------------------------------------
 * CLI command
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_APP_CMD_RUN    = 0,   /* run the main task */
    ASX_APP_CMD_DOCTOR = 1,   /* run diagnostic checks */
    ASX_APP_CMD_REPLAY = 2    /* replay a recorded scenario */
} asx_app_command;

/* -------------------------------------------------------------------
 * Parsed CLI arguments
 * ------------------------------------------------------------------- */

typedef struct {
    asx_app_command command;
    const char     *scenario;    /* replay scenario name (for REPLAY) */
    uint64_t        seed;        /* PRNG seed (0 = default) */
    int             verbose;     /* verbosity level */
    int             help;        /* 1 if --help requested */
} asx_app_args;

/* -------------------------------------------------------------------
 * Application config
 * ------------------------------------------------------------------- */

typedef struct {
    const char        *name;     /* application name for diagnostics */
    uint32_t           poll_budget; /* max polls per run (0 = 10000) */
} asx_app_config;

/* -------------------------------------------------------------------
 * Application object
 * ------------------------------------------------------------------- */

typedef struct {
    asx_app_config     config;
    asx_runtime        runtime;
    asx_region_id      region;
    int                initialized;
    asx_exit_code      exit_code;
} asx_app;

/* -------------------------------------------------------------------
 * Application lifecycle
 * ------------------------------------------------------------------- */

/* Initialize an application with the given config.
 * Sets up the runtime, opens a root region, and prepares for
 * task execution. */
ASX_API ASX_MUST_USE asx_status asx_app_init(
    asx_app *app, const asx_app_config *config);

/* Parse CLI arguments into an args struct.
 * Recognizes: run, doctor, replay, --seed=N, --verbose, --help.
 * Returns ASX_OK on success. */
ASX_API ASX_MUST_USE asx_status asx_app_parse_args(
    asx_app_args *args, int argc, const char **argv);

/* Run the application's main task.
 * Spawns the user's poll function as a task in the app's region,
 * drives the scheduler until the task completes or budget is exhausted,
 * then shuts down. Returns the exit code. */
ASX_API asx_exit_code asx_app_run(
    asx_app *app, asx_task_poll_fn main_fn, void *user_data);

/* Shut down the application and release resources. */
ASX_API void asx_app_shutdown(asx_app *app);

/* Get the app's root region (for spawning additional tasks). */
ASX_API asx_region_id asx_app_region(const asx_app *app);

#ifdef __cplusplus
}
#endif

#endif /* ASX_APP_APP_H */
