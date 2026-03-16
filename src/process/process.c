/*
 * process.c — deterministic child-process host surface
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/process/process.h>
#include <string.h>

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

void asx_process_reset(void) {
    uint32_t i;

    for (i = 0; i < ASX_MAX_PROCESSES; i++) {
        uint32_t generation = g_processes[i].generation;
        memset(&g_processes[i], 0, sizeof(g_processes[i]));
        g_processes[i].generation = generation;
    }
}
