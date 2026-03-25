/*
 * asx/process/process.h — deterministic child-process host surface
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_PROCESS_PROCESS_H
#define ASX_PROCESS_PROCESS_H

#include <asx/asx_config.h>
#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if ASX_HAS_NATIVE_RUNTIME_SURFACES

#ifndef ASX_MAX_PROCESSES
#define ASX_MAX_PROCESSES 8u
#endif

#ifndef ASX_PROCESS_WAIT_MAX_POLLS
#define ASX_PROCESS_WAIT_MAX_POLLS 10000u
#endif

#ifndef ASX_PROCESS_PROGRAM_MAX
#define ASX_PROCESS_PROGRAM_MAX 96u
#endif

typedef struct {
    const char *program;
    uint32_t polls_until_exit;
    int32_t exit_code;
    int auto_exit;
} asx_process_spawn_options;

typedef enum {
    ASX_PROCESS_RUNNING = 0,
    ASX_PROCESS_EXITED = 1,
    ASX_PROCESS_TERMINATED = 2
} asx_process_state;

typedef struct {
    uint32_t slot;
    uint32_t generation;
} asx_process_handle;

/* -------------------------------------------------------------------
 * Process command builder
 * ------------------------------------------------------------------- */

#ifndef ASX_PROCESS_MAX_ARGS
#define ASX_PROCESS_MAX_ARGS 16u
#endif

#ifndef ASX_PROCESS_ARG_MAX
#define ASX_PROCESS_ARG_MAX 128u
#endif

#ifndef ASX_PROCESS_MAX_ENV
#define ASX_PROCESS_MAX_ENV 16u
#endif

#ifndef ASX_PROCESS_ENV_MAX
#define ASX_PROCESS_ENV_MAX 128u
#endif

typedef enum {
    ASX_PROCESS_STDIO_INHERIT = 0,
    ASX_PROCESS_STDIO_PIPED   = 1,
    ASX_PROCESS_STDIO_NULL    = 2
} asx_process_stdio;

typedef struct {
    char key[ASX_PROCESS_ARG_MAX];
    char value[ASX_PROCESS_ARG_MAX];
} asx_process_env_pair;

typedef struct {
    const char *program;
    char args[ASX_PROCESS_MAX_ARGS][ASX_PROCESS_ARG_MAX];
    uint32_t arg_count;
    asx_process_env_pair env_vars[ASX_PROCESS_MAX_ENV];
    uint32_t env_count;
    char current_dir[ASX_PROCESS_ARG_MAX];
    asx_process_stdio stdin_mode;
    asx_process_stdio stdout_mode;
    asx_process_stdio stderr_mode;
    uint8_t kill_on_drop;
    uint8_t env_clear;
    /* Deterministic simulation fields */
    uint32_t polls_until_exit;
    int32_t exit_code;
    int auto_exit;
} asx_process_command;

/* Initialize a command builder. */
ASX_API void asx_process_command_init(asx_process_command *cmd, const char *program);

/* Add an argument. */
ASX_API ASX_MUST_USE asx_status asx_process_command_arg(asx_process_command *cmd, const char *arg);

/* Set an environment variable. */
ASX_API ASX_MUST_USE asx_status asx_process_command_env(asx_process_command *cmd, const char *key,
                                                          const char *value);

/* Clear the environment before adding new variables. */
ASX_API void asx_process_command_env_clear(asx_process_command *cmd);

/* Remove an environment variable. */
ASX_API void asx_process_command_env_remove(asx_process_command *cmd, const char *key);

/* Set the working directory. */
ASX_API void asx_process_command_current_dir(asx_process_command *cmd, const char *dir);

/* Configure stdin/stdout/stderr. */
ASX_API void asx_process_command_stdin(asx_process_command *cmd, asx_process_stdio mode);
ASX_API void asx_process_command_stdout(asx_process_command *cmd, asx_process_stdio mode);
ASX_API void asx_process_command_stderr(asx_process_command *cmd, asx_process_stdio mode);

/* Set kill-on-drop behavior. */
ASX_API void asx_process_command_kill_on_drop(asx_process_command *cmd, uint8_t enabled);

/* Spawn from command builder. */
ASX_API ASX_MUST_USE asx_status asx_process_command_spawn(const asx_process_command *cmd,
                                                            asx_process_handle *out);

/* -------------------------------------------------------------------
 * Process lifecycle (original API)
 * ------------------------------------------------------------------- */

ASX_API ASX_MUST_USE asx_status asx_process_spawn(asx_process_handle *out,
                                                  const asx_process_spawn_options *options);
ASX_API ASX_MUST_USE asx_status asx_process_poll_wait(asx_process_handle process,
                                                      int32_t *out_exit_code);
ASX_API ASX_MUST_USE asx_status asx_process_request_shutdown(asx_process_handle process);
ASX_API ASX_MUST_USE asx_status asx_process_kill(asx_process_handle process, int32_t exit_code);
ASX_API ASX_MUST_USE asx_status asx_process_program(asx_process_handle process,
                                                    const char **out_program);
ASX_API ASX_MUST_USE asx_status asx_process_state_query(asx_process_handle process,
                                                        asx_process_state *out_state);
ASX_API int asx_process_is_alive(asx_process_handle process);

/* -------------------------------------------------------------------
 * Process output capture
 * ------------------------------------------------------------------- */

#ifndef ASX_PROCESS_OUTPUT_MAX
#define ASX_PROCESS_OUTPUT_MAX 4096u
#endif

typedef struct {
    int32_t exit_code;
    uint8_t stdout_data[ASX_PROCESS_OUTPUT_MAX];
    uint32_t stdout_len;
    uint8_t stderr_data[ASX_PROCESS_OUTPUT_MAX];
    uint32_t stderr_len;
    int success; /* exit_code == 0 */
} asx_process_output;

/* Wait for process to complete and capture all output. */
ASX_API ASX_MUST_USE asx_status asx_process_wait_with_output(asx_process_handle process,
                                                               asx_process_output *out);

/* Get the process ID (deterministic: slot index in core profile). */
ASX_API uint32_t asx_process_id(asx_process_handle process);

/* Check if exit was successful (exit code 0). */
ASX_API int asx_process_exited_successfully(asx_process_handle process);

ASX_API void asx_process_reset(void);

#endif /* ASX_HAS_NATIVE_RUNTIME_SURFACES */

#ifdef __cplusplus
}
#endif

#endif /* ASX_PROCESS_PROCESS_H */
