/*
 * process.c — deterministic child-process host surface
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/process/process.h>
#include <string.h>

#if ASX_HAS_NATIVE_RUNTIME_SURFACES

typedef struct {
    char program[ASX_PROCESS_PROGRAM_MAX];
    uint32_t generation;
    uint32_t polls_remaining;
    int32_t exit_code;
    asx_process_state state;
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

asx_status asx_process_spawn(asx_process_handle *out, const asx_process_spawn_options *options) {
    uint32_t i;
    asx_process_slot *slot;
    uint32_t generation;
    size_t len;

    if (out == NULL || options == NULL || options->program == NULL) {
        return ASX_E_INVALID_ARGUMENT;
    }

    len = strlen(options->program);
    if (len == 0u || len >= ASX_PROCESS_PROGRAM_MAX) { return ASX_E_INVALID_ARGUMENT; }

    for (i = 0; i < ASX_MAX_PROCESSES; i++) {
        if (!g_processes[i].alive) {
            slot = &g_processes[i];
            generation = asx_process_next_generation(slot->generation);
            memset(slot, 0, sizeof(*slot));
            memcpy(slot->program, options->program, len + 1u);
            slot->generation = generation;
            slot->polls_remaining = options->polls_until_exit;
            slot->exit_code = options->exit_code;
            slot->state = ASX_PROCESS_RUNNING;
            slot->auto_exit = options->auto_exit;
            slot->alive = 1;
            out->slot = i;
            out->generation = slot->generation;
            return ASX_OK;
        }
    }

    return ASX_E_RESOURCE_EXHAUSTED;
}

asx_status asx_process_poll_wait(asx_process_handle process, int32_t *out_exit_code) {
    asx_process_slot *slot;

    if (out_exit_code == NULL) return ASX_E_INVALID_ARGUMENT;
    slot = asx_process_lookup(process);
    if (slot == NULL) return ASX_E_NOT_FOUND;

    if (slot->state == ASX_PROCESS_RUNNING) {
        if (slot->auto_exit == 0) return ASX_E_PENDING;
        if (slot->polls_remaining > 0u) {
            slot->polls_remaining--;
            return ASX_E_PENDING;
        }
        slot->state = ASX_PROCESS_EXITED;
    }

    *out_exit_code = slot->exit_code;
    return ASX_OK;
}

asx_status asx_process_request_shutdown(asx_process_handle process) {
    asx_process_slot *slot = asx_process_lookup(process);
    if (slot == NULL) return ASX_E_NOT_FOUND;
    if (slot->state == ASX_PROCESS_RUNNING) {
        slot->state = ASX_PROCESS_EXITED;
        slot->exit_code = 0;
    }
    return ASX_OK;
}

asx_status asx_process_kill(asx_process_handle process, int32_t exit_code) {
    asx_process_slot *slot = asx_process_lookup(process);
    if (slot == NULL) return ASX_E_NOT_FOUND;
    slot->state = ASX_PROCESS_TERMINATED;
    slot->exit_code = exit_code;
    return ASX_OK;
}

asx_status asx_process_program(asx_process_handle process, const char **out_program) {
    asx_process_slot *slot;
    if (out_program == NULL) return ASX_E_INVALID_ARGUMENT;
    slot = asx_process_lookup(process);
    if (slot == NULL) return ASX_E_NOT_FOUND;
    *out_program = slot->program;
    return ASX_OK;
}

asx_status asx_process_state_query(asx_process_handle process, asx_process_state *out_state) {
    asx_process_slot *slot;
    if (out_state == NULL) return ASX_E_INVALID_ARGUMENT;
    slot = asx_process_lookup(process);
    if (slot == NULL) return ASX_E_NOT_FOUND;
    *out_state = slot->state;
    return ASX_OK;
}

int asx_process_is_alive(asx_process_handle process) { return asx_process_lookup(process) != NULL; }

/* ------------------------------------------------------------------ */
/* Command builder                                                     */
/* ------------------------------------------------------------------ */

static size_t bounded_len(const char *s, size_t max) {
    size_t len = 0;
    if (s == NULL) return 0;
    while (len < max && s[len] != '\0') len++;
    return len;
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
    if (cmd == NULL || key == NULL || value == NULL) return ASX_E_INVALID_ARGUMENT;
    if (cmd->env_count >= ASX_PROCESS_MAX_ENV) return ASX_E_RESOURCE_EXHAUSTED;
    klen = bounded_len(key, ASX_PROCESS_ARG_MAX);
    vlen = bounded_len(value, ASX_PROCESS_ARG_MAX);
    if (klen >= ASX_PROCESS_ARG_MAX || vlen >= ASX_PROCESS_ARG_MAX) return ASX_E_BUFFER_TOO_SMALL;
    memcpy(cmd->env_vars[cmd->env_count].key, key, klen + 1u);
    memcpy(cmd->env_vars[cmd->env_count].value, value, vlen + 1u);
    cmd->env_count++;
    return ASX_OK;
}

void asx_process_command_env_clear(asx_process_command *cmd) {
    if (cmd == NULL) return;
    cmd->env_clear = 1u;
    cmd->env_count = 0u;
}

void asx_process_command_env_remove(asx_process_command *cmd, const char *key) {
    uint32_t i, write;
    if (cmd == NULL || key == NULL) return;
    write = 0u;
    for (i = 0u; i < cmd->env_count; i++) {
        if (strcmp(cmd->env_vars[i].key, key) != 0) {
            if (write != i) cmd->env_vars[write] = cmd->env_vars[i];
            write++;
        }
    }
    cmd->env_count = write;
}

void asx_process_command_current_dir(asx_process_command *cmd, const char *dir) {
    size_t len;
    if (cmd == NULL || dir == NULL) return;
    len = bounded_len(dir, ASX_PROCESS_ARG_MAX);
    if (len < ASX_PROCESS_ARG_MAX) memcpy(cmd->current_dir, dir, len + 1u);
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

asx_status asx_process_command_spawn(const asx_process_command *cmd, asx_process_handle *out) {
    asx_process_spawn_options opts;

    if (cmd == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    memset(&opts, 0, sizeof(opts));
    opts.program = cmd->program;
    opts.polls_until_exit = cmd->polls_until_exit;
    opts.exit_code = cmd->exit_code;
    opts.auto_exit = cmd->auto_exit;
    return asx_process_spawn(out, &opts);
}

/* ------------------------------------------------------------------ */
/* Output capture and queries                                          */
/* ------------------------------------------------------------------ */

asx_status asx_process_wait_with_output(asx_process_handle process, asx_process_output *out) {
    int32_t exit_code = 0;
    asx_status st;
    uint32_t max_polls = ASX_PROCESS_WAIT_MAX_POLLS;

    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    /* Poll until exit (bounded to prevent infinite loop) */
    while (max_polls > 0u) {
        st = asx_process_poll_wait(process, &exit_code);
        if (st == ASX_OK) break;
        if (st != ASX_E_PENDING) return st;
        max_polls--;
    }
    if (max_polls == 0u) return ASX_E_POLL_BUDGET_EXHAUSTED;

    out->exit_code = exit_code;
    out->success = (exit_code == 0) ? 1 : 0;
    /* In deterministic mode, no real stdout/stderr to capture */
    return ASX_OK;
}

uint32_t asx_process_id(asx_process_handle process) {
    asx_process_slot *slot = asx_process_lookup(process);
    if (slot == NULL) return 0u;
    return process.slot;
}

int asx_process_exited_successfully(asx_process_handle process) {
    asx_process_slot *slot = asx_process_lookup(process);
    if (slot == NULL) return 0;
    if (slot->state != ASX_PROCESS_EXITED) return 0;
    return slot->exit_code == 0;
}

/* ------------------------------------------------------------------ */
/* Reset                                                               */
/* ------------------------------------------------------------------ */

void asx_process_reset(void) {
    uint32_t i;

    for (i = 0; i < ASX_MAX_PROCESSES; i++) {
        uint32_t generation = g_processes[i].generation;
        memset(&g_processes[i], 0, sizeof(g_processes[i]));
        g_processes[i].generation = generation;
    }
}

#endif /* ASX_HAS_NATIVE_RUNTIME_SURFACES */
