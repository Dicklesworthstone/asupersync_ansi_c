/*
 * asx/process/process.h — child-process host surface
 *
 * Two backends behind one API:
 *   MEMORY — deterministic simulation (lab runtime, replay, portable
 *            core): the caller scripts the exit (polls_until_exit /
 *            exit_code / auto_exit); argv/env/cwd are recorded but not
 *            executed; piped stdio behaves like an empty pipe that reaches
 *            EOF once the simulated child exits.
 *   NATIVE — real child processes (POSIX builds): fork + execve with
 *            argv, environment (inherit / clear / set / remove), working
 *            directory and stdio (inherit / null / piped). Pipes are
 *            non-blocking and registered with the IO driver; waiting for
 *            exit uses a pidfd on Linux (parked on reactor readiness) and
 *            otherwise a short, backed-off task timer. Children are always
 *            reaped: by a wait, by asx_process_release(), or by the
 *            detached-child reaper.
 *
 * Backend selection: asx_process_reset() restores MEMORY;
 * asx_runtime_init() switches to NATIVE when it installs a live readiness
 * reactor; asx_process_set_backend() selects one explicitly. Handles keep
 * the backend they were spawned with.
 *
 * Poll functions (poll_wait / poll_status / poll_output / pipe reads and
 * writes) never block: with the NATIVE backend, an operation that must
 * wait arms a wake source for the task currently being polled, parks it,
 * and returns ASX_E_PENDING. asx_process_wait_with_output() is the one
 * blocking convenience call.
 *
 * Exit status: a normal exit reports ASX_PROCESS_EXITED with the exit
 * code; death by signal reports ASX_PROCESS_TERMINATED with the signal
 * number and exit code 128 + signal (MEMORY: asx_process_kill reports
 * ASX_PROCESS_SIGKILL and the code passed to it).
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

/* Conventional signal numbers reported by the MEMORY backend. */
#define ASX_PROCESS_SIGKILL 9
#define ASX_PROCESS_SIGTERM 15

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

typedef enum { ASX_PROCESS_BACKEND_MEMORY = 0, ASX_PROCESS_BACKEND_NATIVE = 1 } asx_process_backend;

/* Final status of a child process. */
typedef struct {
    asx_process_state state; /* ASX_PROCESS_EXITED or ASX_PROCESS_TERMINATED */
    int32_t code;            /* exit code; 128 + signal when terminated by a signal */
    int32_t signal;          /* terminating signal number, 0 for a normal exit */
} asx_process_exit_status;

/* Select the backend for processes spawned from now on.
 * Returns ASX_OK, ASX_E_INVALID_ARGUMENT for unknown values, or
 * ASX_E_PERMISSION_DENIED when NATIVE is unavailable in this build. */
ASX_API ASX_MUST_USE asx_status asx_process_set_backend(asx_process_backend backend);

/* Report the backend used for newly spawned processes. */
ASX_API asx_process_backend asx_process_get_backend(void);

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
    ASX_PROCESS_STDIO_PIPED = 1,
    ASX_PROCESS_STDIO_NULL = 2
} asx_process_stdio;

typedef struct {
    char key[ASX_PROCESS_ARG_MAX];
    char value[ASX_PROCESS_ARG_MAX];
    uint8_t remove; /* 1: remove `key` from the inherited environment */
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
    /* Deterministic simulation fields (MEMORY backend only) */
    uint32_t polls_until_exit;
    int32_t exit_code;
    int auto_exit;
} asx_process_command;

/* Initialize a command builder. `program` is borrowed until spawn; a name
 * without '/' is searched in PATH (the command's PATH override if set,
 * else the parent's). */
ASX_API void asx_process_command_init(asx_process_command *cmd, const char *program);

/* Add an argument (argv[1..]). */
ASX_API ASX_MUST_USE asx_status asx_process_command_arg(asx_process_command *cmd, const char *arg);

/* Set an environment variable for the child. Keys must be non-empty and
 * must not contain '='. */
ASX_API ASX_MUST_USE asx_status asx_process_command_env(asx_process_command *cmd, const char *key,
                                                        const char *value);

/* Clear the environment before adding new variables. */
ASX_API void asx_process_command_env_clear(asx_process_command *cmd);

/* Remove an environment variable: drops any value set on the builder and
 * hides an inherited one. Returns ASX_E_RESOURCE_EXHAUSTED when the
 * builder has no room to record the removal. */
ASX_API ASX_MUST_USE asx_status asx_process_command_env_remove(asx_process_command *cmd,
                                                               const char *key);

/* Set the working directory ("" inherits the parent's). Returns
 * ASX_E_BUFFER_TOO_SMALL (builder unchanged) when it does not fit. */
ASX_API ASX_MUST_USE asx_status asx_process_command_current_dir(asx_process_command *cmd,
                                                                const char *dir);

/* Configure stdin. */
ASX_API void asx_process_command_stdin(asx_process_command *cmd, asx_process_stdio mode);
/* Configure stdout. */
ASX_API void asx_process_command_stdout(asx_process_command *cmd, asx_process_stdio mode);
/* Configure stderr. */
ASX_API void asx_process_command_stderr(asx_process_command *cmd, asx_process_stdio mode);

/* Set kill-on-drop: asx_process_release() (and reset) of a still-running
 * child sends SIGKILL and reaps it instead of detaching it. */
ASX_API void asx_process_command_kill_on_drop(asx_process_command *cmd, uint8_t enabled);

/* Spawn from command builder. NATIVE: returns once the child has exec'd;
 * ASX_E_NOT_FOUND / ASX_E_PERMISSION_DENIED report a missing or
 * non-executable program or working directory (no child is left behind);
 * ASX_E_RESOURCE_EXHAUSTED when the handle table or the OS is out of
 * resources. */
ASX_API ASX_MUST_USE asx_status asx_process_command_spawn(const asx_process_command *cmd,
                                                          asx_process_handle *out);

/* -------------------------------------------------------------------
 * Process lifecycle (original API)
 * ------------------------------------------------------------------- */

/* Spawn a child process from spawn options. NATIVE executes `program`
 * with no arguments, inherited environment and stdio; the simulation
 * fields are ignored. */
ASX_API ASX_MUST_USE asx_status asx_process_spawn(asx_process_handle *out,
                                                  const asx_process_spawn_options *options);
/* Poll a process for exit: ASX_OK with the exit code once it has exited
 * (128 + signal when killed by a signal), ASX_E_PENDING while running
 * (NATIVE: the polled task is parked until the child exits). */
ASX_API ASX_MUST_USE asx_status asx_process_poll_wait(asx_process_handle process,
                                                      int32_t *out_exit_code);
/* Poll a process for exit, reporting the full exit status. Same waiting
 * behavior as asx_process_poll_wait(). */
ASX_API ASX_MUST_USE asx_status asx_process_poll_status(asx_process_handle process,
                                                        asx_process_exit_status *out);
/* Request graceful shutdown (NATIVE: SIGTERM; MEMORY: clean exit 0). */
ASX_API ASX_MUST_USE asx_status asx_process_request_shutdown(asx_process_handle process);
/* Forcefully kill a process (NATIVE: SIGKILL, reaped by the next wait;
 * MEMORY: terminated immediately with `exit_code`). Killing a child that
 * has already been reaped is a no-op. */
ASX_API ASX_MUST_USE asx_status asx_process_kill(asx_process_handle process, int32_t exit_code);
/* Get the program name of a spawned process. */
ASX_API ASX_MUST_USE asx_status asx_process_program(asx_process_handle process,
                                                    const char **out_program);
/* Query the current state (running/exited/terminated) of a process.
 * NATIVE reaps a child that has exited without blocking. */
ASX_API ASX_MUST_USE asx_status asx_process_state_query(asx_process_handle process,
                                                        asx_process_state *out_state);
/* Check if a process handle is still valid (not released or reset). */
ASX_API int asx_process_is_alive(asx_process_handle process);

/* Release the handle (Rust's Drop for Child): closes piped stdio; a child
 * still running is killed and reaped when kill_on_drop is set, otherwise
 * detached to the internal reaper so it never becomes a zombie. */
ASX_API ASX_MUST_USE asx_status asx_process_release(asx_process_handle process);

/* -------------------------------------------------------------------
 * Piped stdio (requires ASX_PROCESS_STDIO_PIPED for that stream)
 *
 * Reads: ASX_OK with n > 0 bytes, ASX_OK with 0 bytes at EOF,
 * ASX_E_PENDING when no data is available yet (NATIVE: task parked on
 * readability). Writes: ASX_OK with 1..len bytes (partial writes are
 * normal), ASX_E_PENDING when the pipe is full (task parked on
 * writability), ASX_E_DISCONNECTED when the child closed its stdin.
 * ASX_E_INVALID_STATE when the stream is not piped (or stdin was closed).
 * ------------------------------------------------------------------- */

/* Write to the child's stdin. Never raises SIGPIPE. */
ASX_API ASX_MUST_USE asx_status asx_process_stdin_write(asx_process_handle process,
                                                        const uint8_t *src, uint32_t len,
                                                        uint32_t *out_written);
/* Close the child's stdin (the child reads EOF). Idempotent. */
ASX_API ASX_MUST_USE asx_status asx_process_stdin_close(asx_process_handle process);
/* Read from the child's stdout. */
ASX_API ASX_MUST_USE asx_status asx_process_stdout_read(asx_process_handle process, uint8_t *dst,
                                                        uint32_t cap, uint32_t *out_read);
/* Read from the child's stderr. */
ASX_API ASX_MUST_USE asx_status asx_process_stderr_read(asx_process_handle process, uint8_t *dst,
                                                        uint32_t cap, uint32_t *out_read);

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
    int success;              /* exited normally with exit_code == 0 */
    int32_t term_signal;      /* terminating signal, 0 for a normal exit */
    uint8_t stdout_truncated; /* output beyond ASX_PROCESS_OUTPUT_MAX was discarded */
    uint8_t stderr_truncated;
} asx_process_output;

/* Wait for process to complete and capture all piped output. BLOCKS the
 * calling thread (NATIVE: poll(2) on the pipes and the exit notifier);
 * inside a task use asx_process_poll_output(). Closes stdin first. */
ASX_API ASX_MUST_USE asx_status asx_process_wait_with_output(asx_process_handle process,
                                                             asx_process_output *out);

/* Prepare an output accumulator for asx_process_poll_output(). */
ASX_API void asx_process_output_init(asx_process_output *out);

/* Poll-based wait_with_output: closes stdin, drains available piped
 * stdout/stderr into *out (appending; excess is discarded and flagged),
 * and returns ASX_OK once every piped stream reached EOF and the child
 * has exited (exit fields filled). ASX_E_PENDING otherwise — NATIVE parks
 * the polled task on pipe readiness / child exit. Initialize *out with
 * asx_process_output_init() before the first call. */
ASX_API ASX_MUST_USE asx_status asx_process_poll_output(asx_process_handle process,
                                                        asx_process_output *out);

/* Get the process ID (NATIVE: the OS pid; MEMORY: slot index). */
ASX_API uint32_t asx_process_id(asx_process_handle process);

/* Check if exit was successful (exited normally with code 0). */
ASX_API int asx_process_exited_successfully(asx_process_handle process);

/* Reset all process state (test support): releases every handle of both
 * backends (kill_on_drop children are killed; all are reaped or detached
 * to the reaper) and restores the MEMORY backend. */
ASX_API void asx_process_reset(void);

#endif /* ASX_HAS_NATIVE_RUNTIME_SURFACES */

#ifdef __cplusplus
}
#endif

#endif /* ASX_PROCESS_PROCESS_H */
