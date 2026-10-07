/*
 * process.c — child-process host surface
 *
 * Two backends behind one API:
 *   MEMORY — deterministic simulation: the caller scripts when and how the
 *            child exits; piped stdio models an empty pipe that reaches EOF
 *            once the simulated child has exited.
 *   NATIVE — real children (src/platform/posix/process_posix.c), selected
 *            per process at spawn; native handles carry
 *            ASX_PROCESS_NATIVE_SLOT_BIT in their slot.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/process/process.h>
#include <string.h>

#if ASX_HAS_NATIVE_RUNTIME_SURFACES

#if defined(ASX_PROFILE_POSIX)
#include "process_native.h"
#define ASX_PROCESS_HAS_NATIVE 1
#else
#define ASX_PROCESS_HAS_NATIVE 0
#endif

/* asx_process_reset() always restores the simulation; asx_runtime_init()
 * switches to NATIVE when it installs a live reactor (see rt.c). */
#define ASX_PROCESS_DEFAULT_BACKEND ASX_PROCESS_BACKEND_MEMORY

static asx_process_backend g_process_backend = ASX_PROCESS_DEFAULT_BACKEND;

#if ASX_PROCESS_HAS_NATIVE
#define PROC_NATIVE(h) asx_process_slot_is_native((h).slot)
#define PROC_USE_NATIVE() (g_process_backend == ASX_PROCESS_BACKEND_NATIVE)
#endif

typedef struct {
    char program[ASX_PROCESS_PROGRAM_MAX];
    uint32_t generation;
    uint32_t polls_remaining;
    int32_t exit_code;
    int32_t term_signal;
    asx_process_state state;
    asx_process_stdio modes[3]; /* stdin, stdout, stderr */
    int stdin_closed;
    int auto_exit;
    int alive;
} asx_process_slot;

static asx_process_slot g_processes[ASX_MAX_PROCESSES];

static uint32_t asx_process_next_generation(uint32_t generation) {
    generation++;
    return generation == 0u ? 1u : generation;
}

static asx_process_slot *asx_process_lookup(asx_process_handle process) {
    asx_process_slot *slot;
    if (process.slot >= ASX_MAX_PROCESSES) return NULL;
    slot = &g_processes[process.slot];
    if (!slot->alive) return NULL;
    if (slot->generation != process.generation) return NULL;
    return slot;
}

static asx_status proc_check_program(const char *program) {
    size_t len;
    if (program == NULL) return ASX_E_INVALID_ARGUMENT;
    len = strlen(program);
    if (len == 0u || len >= ASX_PROCESS_PROGRAM_MAX) return ASX_E_INVALID_ARGUMENT;
    return ASX_OK;
}

static asx_status proc_mem_spawn(const char *program, uint32_t polls_until_exit, int32_t exit_code,
                                 int auto_exit, const asx_process_stdio modes[3],
                                 asx_process_handle *out) {
    uint32_t i;
    size_t len = strlen(program);

    for (i = 0; i < ASX_MAX_PROCESSES; i++) {
        if (!g_processes[i].alive) {
            asx_process_slot *slot = &g_processes[i];
            uint32_t generation = asx_process_next_generation(slot->generation);
            memset(slot, 0, sizeof(*slot));
            memcpy(slot->program, program, len + 1u);
            slot->generation = generation;
            slot->polls_remaining = polls_until_exit;
            slot->exit_code = exit_code;
            slot->state = ASX_PROCESS_RUNNING;
            slot->auto_exit = auto_exit;
            slot->modes[0] = modes[0];
            slot->modes[1] = modes[1];
            slot->modes[2] = modes[2];
            slot->alive = 1;
            out->slot = i;
            out->generation = slot->generation;
            return ASX_OK;
        }
    }

    return ASX_E_RESOURCE_EXHAUSTED;
}

/* Advance the simulation one wait-poll; ASX_OK once it has finished. */
static asx_status proc_mem_poll(asx_process_slot *slot) {
    if (slot->state == ASX_PROCESS_RUNNING) {
        if (slot->auto_exit == 0) return ASX_E_PENDING;
        if (slot->polls_remaining > 0u) {
            slot->polls_remaining--;
            return ASX_E_PENDING;
        }
        slot->state = ASX_PROCESS_EXITED;
    }
    return ASX_OK;
}

static void proc_mem_status(const asx_process_slot *slot, asx_process_exit_status *out) {
    out->state = slot->state;
    out->code = slot->exit_code;
    out->signal = slot->state == ASX_PROCESS_TERMINATED ? slot->term_signal : 0;
}

/* ------------------------------------------------------------------ */
/* Backend selection                                                   */
/* ------------------------------------------------------------------ */

asx_status asx_process_set_backend(asx_process_backend backend) {
    if (backend != ASX_PROCESS_BACKEND_MEMORY && backend != ASX_PROCESS_BACKEND_NATIVE) {
        return ASX_E_INVALID_ARGUMENT;
    }
#if !ASX_PROCESS_HAS_NATIVE
    if (backend == ASX_PROCESS_BACKEND_NATIVE) return ASX_E_PERMISSION_DENIED;
#endif
    g_process_backend = backend;
    return ASX_OK;
}

asx_process_backend asx_process_get_backend(void) { return g_process_backend; }

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

asx_status asx_process_spawn(asx_process_handle *out, const asx_process_spawn_options *options) {
    static const asx_process_stdio inherit[3] = {ASX_PROCESS_STDIO_INHERIT,
                                                 ASX_PROCESS_STDIO_INHERIT,
                                                 ASX_PROCESS_STDIO_INHERIT};
    asx_status st;

    if (out == NULL || options == NULL) return ASX_E_INVALID_ARGUMENT;
    st = proc_check_program(options->program);
    if (st != ASX_OK) return st;

#if ASX_PROCESS_HAS_NATIVE
    if (PROC_USE_NATIVE()) {
        asx_process_command cmd;
        asx_process_command_init(&cmd, options->program);
        return asx_native_process_spawn(&cmd, out);
    }
#endif
    return proc_mem_spawn(options->program, options->polls_until_exit, options->exit_code,
                          options->auto_exit, inherit, out);
}

asx_status asx_process_poll_status(asx_process_handle process, asx_process_exit_status *out) {
    asx_process_slot *slot;
    asx_status st;

    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
#if ASX_PROCESS_HAS_NATIVE
    if (PROC_NATIVE(process)) return asx_native_process_poll_status(process, out, 1);
#endif
    slot = asx_process_lookup(process);
    if (slot == NULL) return ASX_E_NOT_FOUND;
    st = proc_mem_poll(slot);
    if (st != ASX_OK) return st;
    proc_mem_status(slot, out);
    return ASX_OK;
}

asx_status asx_process_poll_wait(asx_process_handle process, int32_t *out_exit_code) {
    asx_process_exit_status status;
    asx_status st;

    if (out_exit_code == NULL) return ASX_E_INVALID_ARGUMENT;
    st = asx_process_poll_status(process, &status);
    if (st != ASX_OK) return st;
    *out_exit_code = status.code;
    return ASX_OK;
}

asx_status asx_process_request_shutdown(asx_process_handle process) {
    asx_process_slot *slot;
#if ASX_PROCESS_HAS_NATIVE
    if (PROC_NATIVE(process)) return asx_native_process_terminate(process);
#endif
    slot = asx_process_lookup(process);
    if (slot == NULL) return ASX_E_NOT_FOUND;
    if (slot->state == ASX_PROCESS_RUNNING) {
        slot->state = ASX_PROCESS_EXITED;
        slot->exit_code = 0;
    }
    return ASX_OK;
}

asx_status asx_process_kill(asx_process_handle process, int32_t exit_code) {
    asx_process_slot *slot;
#if ASX_PROCESS_HAS_NATIVE
    if (PROC_NATIVE(process)) return asx_native_process_kill(process);
#endif
    slot = asx_process_lookup(process);
    if (slot == NULL) return ASX_E_NOT_FOUND;
    slot->state = ASX_PROCESS_TERMINATED;
    slot->exit_code = exit_code;
    slot->term_signal = ASX_PROCESS_SIGKILL;
    return ASX_OK;
}

asx_status asx_process_program(asx_process_handle process, const char **out_program) {
    asx_process_slot *slot;
    if (out_program == NULL) return ASX_E_INVALID_ARGUMENT;
#if ASX_PROCESS_HAS_NATIVE
    if (PROC_NATIVE(process)) return asx_native_process_program(process, out_program);
#endif
    slot = asx_process_lookup(process);
    if (slot == NULL) return ASX_E_NOT_FOUND;
    *out_program = slot->program;
    return ASX_OK;
}

asx_status asx_process_state_query(asx_process_handle process, asx_process_state *out_state) {
    asx_process_slot *slot;
    if (out_state == NULL) return ASX_E_INVALID_ARGUMENT;
#if ASX_PROCESS_HAS_NATIVE
    if (PROC_NATIVE(process)) return asx_native_process_state(process, out_state);
#endif
    slot = asx_process_lookup(process);
    if (slot == NULL) return ASX_E_NOT_FOUND;
    *out_state = slot->state;
    return ASX_OK;
}

int asx_process_is_alive(asx_process_handle process) {
#if ASX_PROCESS_HAS_NATIVE
    if (PROC_NATIVE(process)) return asx_native_process_is_alive(process);
#endif
    return asx_process_lookup(process) != NULL;
}

asx_status asx_process_release(asx_process_handle process) {
    asx_process_slot *slot;
#if ASX_PROCESS_HAS_NATIVE
    if (PROC_NATIVE(process)) return asx_native_process_release(process);
#endif
    slot = asx_process_lookup(process);
    if (slot == NULL) return ASX_E_NOT_FOUND;
    slot->alive = 0;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Command builder                                                     */
/* ------------------------------------------------------------------ */

static size_t bounded_len(const char *s, size_t max) {
    size_t len = 0;
    if (s == NULL) return 0;
    while (len < max && s[len] != '\0') len++;
    return len;
}

/* Drop every pair (set or removal) for `key`. */
static void proc_env_drop(asx_process_command *cmd, const char *key) {
    uint32_t i, write;
    write = 0u;
    for (i = 0u; i < cmd->env_count; i++) {
        if (strcmp(cmd->env_vars[i].key, key) != 0) {
            if (write != i) cmd->env_vars[write] = cmd->env_vars[i];
            write++;
        }
    }
    cmd->env_count = write;
}

static asx_status proc_env_key_check(const char *key) {
    size_t klen = bounded_len(key, ASX_PROCESS_ARG_MAX);
    if (klen == 0u) return ASX_E_INVALID_ARGUMENT;
    if (klen >= ASX_PROCESS_ARG_MAX) return ASX_E_BUFFER_TOO_SMALL;
    if (memchr(key, '=', klen) != NULL) return ASX_E_INVALID_ARGUMENT;
    return ASX_OK;
}

void asx_process_command_init(asx_process_command *cmd, const char *program) {
    if (cmd == NULL) return;
    memset(cmd, 0, sizeof(*cmd));
    cmd->program = program;
    cmd->stdin_mode = ASX_PROCESS_STDIO_INHERIT;
    cmd->stdout_mode = ASX_PROCESS_STDIO_INHERIT;
    cmd->stderr_mode = ASX_PROCESS_STDIO_INHERIT;
}

asx_status asx_process_command_arg(asx_process_command *cmd, const char *arg) {
    size_t len;
    if (cmd == NULL || arg == NULL) return ASX_E_INVALID_ARGUMENT;
    if (cmd->arg_count >= ASX_PROCESS_MAX_ARGS) return ASX_E_RESOURCE_EXHAUSTED;
    len = bounded_len(arg, ASX_PROCESS_ARG_MAX);
    if (len >= ASX_PROCESS_ARG_MAX) return ASX_E_BUFFER_TOO_SMALL;
    memcpy(cmd->args[cmd->arg_count], arg, len + 1u);
    cmd->arg_count++;
    return ASX_OK;
}

asx_status asx_process_command_env(asx_process_command *cmd, const char *key, const char *value) {
    size_t klen, vlen;
    asx_status st;
    uint32_t i;
    if (cmd == NULL || key == NULL || value == NULL) return ASX_E_INVALID_ARGUMENT;
    st = proc_env_key_check(key);
    if (st != ASX_OK) return st;
    klen = bounded_len(key, ASX_PROCESS_ARG_MAX);
    vlen = bounded_len(value, ASX_PROCESS_ARG_MAX);
    if (vlen >= ASX_PROCESS_ARG_MAX) return ASX_E_BUFFER_TOO_SMALL;
    /* Setting a key replaces any earlier set/removal of it in place. */
    for (i = 0u; i < cmd->env_count; i++) {
        if (strcmp(cmd->env_vars[i].key, key) == 0) {
            memcpy(cmd->env_vars[i].value, value, vlen + 1u);
            cmd->env_vars[i].remove = 0u;
            return ASX_OK;
        }
    }
    if (cmd->env_count >= ASX_PROCESS_MAX_ENV) return ASX_E_RESOURCE_EXHAUSTED;
    memcpy(cmd->env_vars[cmd->env_count].key, key, klen + 1u);
    memcpy(cmd->env_vars[cmd->env_count].value, value, vlen + 1u);
    cmd->env_vars[cmd->env_count].remove = 0u;
    cmd->env_count++;
    return ASX_OK;
}

void asx_process_command_env_clear(asx_process_command *cmd) {
    if (cmd == NULL) return;
    cmd->env_clear = 1u;
    cmd->env_count = 0u;
}

asx_status asx_process_command_env_remove(asx_process_command *cmd, const char *key) {
    asx_status st;
    size_t klen;
    if (cmd == NULL || key == NULL) return ASX_E_INVALID_ARGUMENT;
    st = proc_env_key_check(key);
    if (st != ASX_OK) return st;
    proc_env_drop(cmd, key);
    /* A cleared environment has nothing inherited left to hide. */
    if (cmd->env_clear) return ASX_OK;
    if (cmd->env_count >= ASX_PROCESS_MAX_ENV) return ASX_E_RESOURCE_EXHAUSTED;
    klen = bounded_len(key, ASX_PROCESS_ARG_MAX);
    memcpy(cmd->env_vars[cmd->env_count].key, key, klen + 1u);
    cmd->env_vars[cmd->env_count].value[0] = '\0';
    cmd->env_vars[cmd->env_count].remove = 1u;
    cmd->env_count++;
    return ASX_OK;
}

asx_status asx_process_command_current_dir(asx_process_command *cmd, const char *dir) {
    size_t len;
    if (cmd == NULL || dir == NULL) return ASX_E_INVALID_ARGUMENT;
    len = bounded_len(dir, ASX_PROCESS_ARG_MAX);
    if (len >= ASX_PROCESS_ARG_MAX) return ASX_E_BUFFER_TOO_SMALL;
    memcpy(cmd->current_dir, dir, len + 1u);
    return ASX_OK;
}

void asx_process_command_stdin(asx_process_command *cmd, asx_process_stdio mode) {
    if (cmd != NULL) cmd->stdin_mode = mode;
}

void asx_process_command_stdout(asx_process_command *cmd, asx_process_stdio mode) {
    if (cmd != NULL) cmd->stdout_mode = mode;
}

void asx_process_command_stderr(asx_process_command *cmd, asx_process_stdio mode) {
    if (cmd != NULL) cmd->stderr_mode = mode;
}

void asx_process_command_kill_on_drop(asx_process_command *cmd, uint8_t enabled) {
    if (cmd != NULL) cmd->kill_on_drop = enabled;
}

static int proc_stdio_valid(asx_process_stdio mode) {
    switch (mode) {
    case ASX_PROCESS_STDIO_INHERIT:
    case ASX_PROCESS_STDIO_PIPED:
    case ASX_PROCESS_STDIO_NULL: return 1;
    }
    return 0;
}

asx_status asx_process_command_spawn(const asx_process_command *cmd, asx_process_handle *out) {
    asx_process_stdio modes[3];
    asx_status st;

    if (cmd == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    st = proc_check_program(cmd->program);
    if (st != ASX_OK) return st;
    if (cmd->arg_count > ASX_PROCESS_MAX_ARGS || cmd->env_count > ASX_PROCESS_MAX_ENV) {
        return ASX_E_INVALID_ARGUMENT;
    }
    if (!proc_stdio_valid(cmd->stdin_mode) || !proc_stdio_valid(cmd->stdout_mode) ||
        !proc_stdio_valid(cmd->stderr_mode)) {
        return ASX_E_INVALID_ARGUMENT;
    }
#if ASX_PROCESS_HAS_NATIVE
    if (PROC_USE_NATIVE()) return asx_native_process_spawn(cmd, out);
#endif
    modes[0] = cmd->stdin_mode;
    modes[1] = cmd->stdout_mode;
    modes[2] = cmd->stderr_mode;
    return proc_mem_spawn(cmd->program, cmd->polls_until_exit, cmd->exit_code, cmd->auto_exit,
                          modes, out);
}

/* ------------------------------------------------------------------ */
/* Piped stdio                                                         */
/* ------------------------------------------------------------------ */

asx_status asx_process_stdin_write(asx_process_handle process, const uint8_t *src, uint32_t len,
                                   uint32_t *out_written) {
    asx_process_slot *slot;
    if (out_written == NULL || (src == NULL && len > 0u)) return ASX_E_INVALID_ARGUMENT;
    *out_written = 0u;
#if ASX_PROCESS_HAS_NATIVE
    if (PROC_NATIVE(process)) {
        return asx_native_process_stdin_write(process, src, len, out_written);
    }
#endif
    slot = asx_process_lookup(process);
    if (slot == NULL) return ASX_E_NOT_FOUND;
    if (slot->modes[0] != ASX_PROCESS_STDIO_PIPED || slot->stdin_closed) {
        return ASX_E_INVALID_STATE;
    }
    if (slot->state != ASX_PROCESS_RUNNING) return ASX_E_DISCONNECTED;
    /* The simulated child consumes (and discards) everything. */
    *out_written = len;
    return ASX_OK;
}

asx_status asx_process_stdin_close(asx_process_handle process) {
    asx_process_slot *slot;
#if ASX_PROCESS_HAS_NATIVE
    if (PROC_NATIVE(process)) return asx_native_process_stdin_close(process);
#endif
    slot = asx_process_lookup(process);
    if (slot == NULL) return ASX_E_NOT_FOUND;
    if (slot->modes[0] != ASX_PROCESS_STDIO_PIPED) return ASX_E_INVALID_STATE;
    slot->stdin_closed = 1;
    return ASX_OK;
}

static asx_status proc_output_read(asx_process_handle process, int which, uint8_t *dst,
                                   uint32_t cap, uint32_t *out_read) {
    asx_process_slot *slot;
    if (out_read == NULL || (dst == NULL && cap > 0u)) return ASX_E_INVALID_ARGUMENT;
    *out_read = 0u;
#if ASX_PROCESS_HAS_NATIVE
    if (PROC_NATIVE(process)) {
        if (cap == 0u) return ASX_E_BUFFER_TOO_SMALL;
        return asx_native_process_read(process, which, dst, cap, out_read);
    }
#endif
    slot = asx_process_lookup(process);
    if (slot == NULL) return ASX_E_NOT_FOUND;
    if (slot->modes[which] != ASX_PROCESS_STDIO_PIPED) return ASX_E_INVALID_STATE;
    if (cap == 0u) return ASX_E_BUFFER_TOO_SMALL;
    /* Empty pipe: EOF once the simulated child has exited. */
    return slot->state == ASX_PROCESS_RUNNING ? ASX_E_PENDING : ASX_OK;
}

asx_status asx_process_stdout_read(asx_process_handle process, uint8_t *dst, uint32_t cap,
                                   uint32_t *out_read) {
    return proc_output_read(process, 1, dst, cap, out_read);
}

asx_status asx_process_stderr_read(asx_process_handle process, uint8_t *dst, uint32_t cap,
                                   uint32_t *out_read) {
    return proc_output_read(process, 2, dst, cap, out_read);
}

/* ------------------------------------------------------------------ */
/* Output capture and queries                                          */
/* ------------------------------------------------------------------ */

void asx_process_output_init(asx_process_output *out) {
    if (out != NULL) memset(out, 0, sizeof(*out));
}

static void proc_output_finish(asx_process_output *out, const asx_process_exit_status *status) {
    out->exit_code = status->code;
    out->term_signal = status->signal;
    out->success = (status->state == ASX_PROCESS_EXITED && status->code == 0) ? 1 : 0;
}

asx_status asx_process_poll_output(asx_process_handle process, asx_process_output *out) {
    asx_process_exit_status status;
    asx_status st;

    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
#if ASX_PROCESS_HAS_NATIVE
    if (PROC_NATIVE(process)) return asx_native_process_poll_output(process, out, 1);
#endif
    st = asx_process_poll_status(process, &status);
    if (st != ASX_OK) return st;
    proc_output_finish(out, &status);
    return ASX_OK;
}

asx_status asx_process_wait_with_output(asx_process_handle process, asx_process_output *out) {
    asx_process_exit_status status;
    asx_status st;
    uint32_t max_polls = ASX_PROCESS_WAIT_MAX_POLLS;

    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
#if ASX_PROCESS_HAS_NATIVE
    if (PROC_NATIVE(process)) return asx_native_process_wait_with_output(process, out);
#endif

    /* Poll until exit (bounded to prevent infinite loop) */
    while (max_polls > 0u) {
        st = asx_process_poll_status(process, &status);
        if (st == ASX_OK) break;
        if (st != ASX_E_PENDING) return st;
        max_polls--;
    }
    if (max_polls == 0u) return ASX_E_POLL_BUDGET_EXHAUSTED;

    /* The simulation produces no output bytes. */
    proc_output_finish(out, &status);
    return ASX_OK;
}

uint32_t asx_process_id(asx_process_handle process) {
    asx_process_slot *slot;
#if ASX_PROCESS_HAS_NATIVE
    if (PROC_NATIVE(process)) return asx_native_process_id(process);
#endif
    slot = asx_process_lookup(process);
    if (slot == NULL) return 0u;
    return process.slot;
}

int asx_process_exited_successfully(asx_process_handle process) {
    asx_process_state state;
    asx_process_slot *slot;
#if ASX_PROCESS_HAS_NATIVE
    if (PROC_NATIVE(process)) {
        asx_process_exit_status status;
        if (asx_native_process_state(process, &state) != ASX_OK) return 0;
        if (state != ASX_PROCESS_EXITED) return 0;
        if (asx_native_process_poll_status(process, &status, 0) != ASX_OK) return 0;
        return status.code == 0;
    }
#endif
    slot = asx_process_lookup(process);
    if (slot == NULL) return 0;
    state = slot->state;
    if (state != ASX_PROCESS_EXITED) return 0;
    return slot->exit_code == 0;
}

/* ------------------------------------------------------------------ */
/* Reset                                                               */
/* ------------------------------------------------------------------ */

void asx_process_reset(void) {
    uint32_t i;

#if ASX_PROCESS_HAS_NATIVE
    asx_native_process_reset();
#endif
    g_process_backend = ASX_PROCESS_DEFAULT_BACKEND;
    for (i = 0; i < ASX_MAX_PROCESSES; i++) {
        uint32_t generation = g_processes[i].generation;
        memset(&g_processes[i], 0, sizeof(g_processes[i]));
        g_processes[i].generation = generation;
    }
}

#endif /* ASX_HAS_NATIVE_RUNTIME_SURFACES */
