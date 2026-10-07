/*
 * process_native.h — internal bridge between the process API and real
 * child processes
 *
 * NOT part of the public API. process.c validates arguments and
 * dispatches here for handles whose slot carries
 * ASX_PROCESS_NATIVE_SLOT_BIT (and for spawns while NATIVE is selected).
 * The POSIX implementation lives in src/platform/posix/process_posix.c
 * and is only linked in POSIX builds.
 *
 * `park` arguments: nonzero lets an operation that must wait arm a wake
 * source for the current task and park it (poll API); zero only reports
 * ASX_E_PENDING (used by the blocking wait_with_output loop).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_PROCESS_NATIVE_H
#define ASX_PROCESS_NATIVE_H

#include <asx/asx_status.h>
#include <asx/process/process.h>
#include <stdint.h>

/* Handles with this bit set in `slot` refer to native child processes. */
#define ASX_PROCESS_NATIVE_SLOT_BIT 0x80000000u

static inline int asx_process_slot_is_native(uint32_t slot) {
    return (slot & ASX_PROCESS_NATIVE_SLOT_BIT) != 0u;
}

/* Stream selectors for asx_native_process_read. */
#define ASX_NATIVE_PROCESS_STDOUT 1
#define ASX_NATIVE_PROCESS_STDERR 2

/* Reset: release every native child (kill_on_drop ones are killed). */
void asx_native_process_reset(void);

/* Spawn (command pre-validated: program non-empty and short enough). */
asx_status asx_native_process_spawn(const asx_process_command *cmd, asx_process_handle *out);

/* Exit waiting and status. */
asx_status asx_native_process_poll_status(asx_process_handle h, asx_process_exit_status *out,
                                          int park);
asx_status asx_native_process_state(asx_process_handle h, asx_process_state *out);

/* Signals: SIGKILL / SIGTERM (no-op once reaped). */
asx_status asx_native_process_kill(asx_process_handle h);
asx_status asx_native_process_terminate(asx_process_handle h);

/* Queries. */
asx_status asx_native_process_program(asx_process_handle h, const char **out);
uint32_t asx_native_process_id(asx_process_handle h);
int asx_native_process_is_alive(asx_process_handle h);

/* Piped stdio. */
asx_status asx_native_process_stdin_write(asx_process_handle h, const uint8_t *src, uint32_t len,
                                          uint32_t *out_written);
asx_status asx_native_process_stdin_close(asx_process_handle h);
asx_status asx_native_process_read(asx_process_handle h, int which, uint8_t *dst, uint32_t cap,
                                   uint32_t *out_read);

/* Output capture: poll form (appends into *out) and blocking form. */
asx_status asx_native_process_poll_output(asx_process_handle h, asx_process_output *out, int park);
asx_status asx_native_process_wait_with_output(asx_process_handle h, asx_process_output *out);

/* Drop the handle (kill_on_drop or detach-to-reaper). */
asx_status asx_native_process_release(asx_process_handle h);

#endif /* ASX_PROCESS_NATIVE_H */
