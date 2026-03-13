/*
 * asx/process/process.h — deterministic child-process host surface
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_PROCESS_PROCESS_H
#define ASX_PROCESS_PROCESS_H

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ASX_MAX_PROCESSES
#define ASX_MAX_PROCESSES 8u
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

ASX_API void asx_process_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_PROCESS_PROCESS_H */
