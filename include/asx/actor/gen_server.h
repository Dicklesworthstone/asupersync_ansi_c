/*
 * asx/actor/gen_server.h — OTP-style generic request/reply server
 *
 * Provides a structured server pattern where a callback module handles
 * typed requests and produces replies. The gen_server manages the
 * mailbox, dispatch, state threading, and lifecycle.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_ACTOR_GEN_SERVER_H
#define ASX_ACTOR_GEN_SERVER_H

#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ASX_GEN_SERVER_MAILBOX_SIZE
#define ASX_GEN_SERVER_MAILBOX_SIZE 16u
#endif

#ifndef ASX_GEN_SERVER_MAX_INSTANCES
#define ASX_GEN_SERVER_MAX_INSTANCES 8u
#endif

/* -------------------------------------------------------------------
 * Gen server callback interface
 * ------------------------------------------------------------------- */

/* Init callback: returns initial state. */
typedef asx_status (*asx_gen_server_init_fn)(void *args, void **out_state);

/* Handle call (synchronous request/reply). */
typedef asx_status (*asx_gen_server_handle_call_fn)(void *state, uint64_t request,
                                                     uint64_t *reply);

/* Handle cast (asynchronous one-way message). */
typedef asx_status (*asx_gen_server_handle_cast_fn)(void *state, uint64_t message);

/* Terminate callback: cleanup state. */
typedef void (*asx_gen_server_terminate_fn)(void *state, asx_status reason);

typedef struct {
    asx_gen_server_init_fn init;
    asx_gen_server_handle_call_fn handle_call;
    asx_gen_server_handle_cast_fn handle_cast;
    asx_gen_server_terminate_fn terminate;
} asx_gen_server_callbacks;

/* -------------------------------------------------------------------
 * Gen server instance
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_GEN_SERVER_IDLE    = 0,
    ASX_GEN_SERVER_RUNNING = 1,
    ASX_GEN_SERVER_STOPPED = 2
} asx_gen_server_state;

typedef enum {
    ASX_GEN_SERVER_MSG_CALL = 0,
    ASX_GEN_SERVER_MSG_CAST = 1,
    ASX_GEN_SERVER_MSG_STOP = 2
} asx_gen_server_msg_kind;

typedef struct {
    asx_gen_server_msg_kind kind;
    uint64_t payload;
    uint64_t reply;
    uint8_t replied;
} asx_gen_server_msg;

typedef struct {
    uint32_t slot;
    uint32_t generation;
} asx_gen_server_ref;

/* -------------------------------------------------------------------
 * Gen server API
 * ------------------------------------------------------------------- */

/* Start a gen_server with the given callback module and init args. */
ASX_API ASX_MUST_USE asx_status asx_gen_server_start(asx_gen_server_ref *out,
                                                       const asx_gen_server_callbacks *callbacks,
                                                       void *init_args);

/* Send a synchronous call. Blocks (polls) until reply is available. */
ASX_API ASX_MUST_USE asx_status asx_gen_server_call(asx_gen_server_ref ref, uint64_t request,
                                                      uint64_t *reply);

/* Send an asynchronous cast (fire-and-forget). */
ASX_API ASX_MUST_USE asx_status asx_gen_server_cast(asx_gen_server_ref ref, uint64_t message);

/* Stop the gen_server gracefully. */
ASX_API asx_status asx_gen_server_stop(asx_gen_server_ref ref);

/* Query gen_server state. */
ASX_API asx_gen_server_state asx_gen_server_get_state(asx_gen_server_ref ref);

/* Process one message from the mailbox. Returns ASX_E_PENDING if empty. */
ASX_API ASX_MUST_USE asx_status asx_gen_server_poll(asx_gen_server_ref ref);

/* Try to send a cast without blocking. Returns ASX_E_WOULD_BLOCK if mailbox full. */
ASX_API ASX_MUST_USE asx_status asx_gen_server_try_cast(asx_gen_server_ref ref, uint64_t message);

/* Send an info message (like cast but uses a separate handler). */
ASX_API ASX_MUST_USE asx_status asx_gen_server_info(asx_gen_server_ref ref, uint64_t message);

/* Check if the gen_server has finished (stopped or failed). */
ASX_API int asx_gen_server_is_finished(asx_gen_server_ref ref);

/* Check if the gen_server mailbox is closed. */
ASX_API int asx_gen_server_is_closed(asx_gen_server_ref ref);

/* Get the gen_server's name (empty string if unnamed). */
ASX_API const char *asx_gen_server_name(asx_gen_server_ref ref);

/* Get the number of calls handled. */
ASX_API uint32_t asx_gen_server_calls_handled(asx_gen_server_ref ref);

/* Get the number of casts handled. */
ASX_API uint32_t asx_gen_server_casts_handled(asx_gen_server_ref ref);

/* Start a named gen_server. */
ASX_API ASX_MUST_USE asx_status asx_gen_server_start_named(asx_gen_server_ref *out, const char *name,
                                                              const asx_gen_server_callbacks *callbacks,
                                                              void *init_args);

/* Reset all gen_server state (test support). */
ASX_API void asx_gen_server_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_ACTOR_GEN_SERVER_H */
