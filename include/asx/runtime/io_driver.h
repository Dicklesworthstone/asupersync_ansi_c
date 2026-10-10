/*
 * asx/runtime/io_driver.h — IO driver and reactor integration
 *
 * The IO driver owns the reactor lifecycle and provides the bridge
 * between external IO events and task wakeups.
 *
 * Backends:
 *   live  — non-deterministic builds whose reactor hooks report per-fd
 *           readiness (POSIX epoll / poll(2)). Interest is armed one-shot
 *           and readiness wakes exactly the task (or waker) that armed it.
 *   ghost — deterministic builds: registration/quiescence semantics only;
 *           readiness is synthesized deterministically for replay.
 *
 * Typical use from a poll function (non-blocking fd):
 *   n = read(fd, ...);
 *   if (n < 0 && errno == EAGAIN) return asx_io_wait(&token, ASX_IO_READABLE);
 * asx_io_wait arms one-shot interest for the current task and parks it;
 * the scheduler's idle loop blocks in the reactor and wakes it on
 * readiness.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_RUNTIME_IO_DRIVER_H
#define ASX_RUNTIME_IO_DRIVER_H

#include <asx/asx_config.h>
#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>
#include <asx/runtime/waker.h>

#ifdef __cplusplus
extern "C" {
#endif

#if ASX_HAS_NATIVE_IO_DRIVER
#ifndef ASX_MAX_IO_TOKENS
#define ASX_MAX_IO_TOKENS 64u
#endif
#if (ASX_MAX_IO_TOKENS) < 1
#error "ASX_MAX_IO_TOKENS must be at least 1"
#endif

/* -------------------------------------------------------------------
 * IO interest
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_IO_READABLE = 0x01,
    ASX_IO_WRITABLE = 0x02,
    ASX_IO_ERROR = 0x04
} asx_io_interest;

/* -------------------------------------------------------------------
 * IO token — registration handle for IO interest
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t slot;
    uint16_t generation;
} asx_io_token;

/* -------------------------------------------------------------------
 * IO event — returned from driver poll
 * ------------------------------------------------------------------- */

typedef struct {
    asx_io_token token;
    asx_io_interest ready; /* bitmask of ready interests */
} asx_io_event;

/* -------------------------------------------------------------------
 * API: IO driver lifecycle
 * ------------------------------------------------------------------- */

/* Initialize the IO driver. Must be called after runtime hooks
 * are installed. Uses the reactor hooks from runtime config.
 * Re-initialization resets all prior registrations and invalidates old tokens. */
ASX_API ASX_MUST_USE asx_status asx_io_driver_init(void);

/* Shut down the IO driver, deregistering all tokens. */
ASX_API void asx_io_driver_shutdown(void);

/* Query whether the IO driver is initialized and accepting registrations. */
ASX_API int asx_io_driver_is_initialized(void);

/* -------------------------------------------------------------------
 * API: IO registration
 * ------------------------------------------------------------------- */

/* Register interest in IO events for a resource.
 * fd is an opaque file descriptor (platform-dependent).
 * interest must be a nonzero bitmask composed only of
 * ASX_IO_READABLE|WRITABLE|ERROR.
 * waker will be signaled when the interest is ready and must refer to a
 * currently live waker registration.
 * Returns ASX_OK on success.
 * Returns ASX_E_INVALID_ARGUMENT if any pointer is NULL, interest is invalid,
 * or waker is stale/deregistered. */
ASX_API ASX_MUST_USE asx_status asx_io_register(int fd, asx_io_interest interest,
                                                const asx_waker *waker, asx_io_token *out_token);

/* Register a file descriptor without arming any interest or wake target.
 * Use asx_io_arm() / asx_io_wait() to request readiness notifications.
 * Returns ASX_OK, ASX_E_INVALID_ARGUMENT for NULL/negative input,
 * ASX_E_INVALID_STATE if the driver is not initialized, or
 * ASX_E_RESOURCE_EXHAUSTED when the token arena is full. */
ASX_API ASX_MUST_USE asx_status asx_io_register_fd(int fd, asx_io_token *out_token);

/* Arm one-shot interest and set the task woken on readiness (the live
 * backend reports the next readiness once, then the registration must be
 * armed again). Returns ASX_OK, ASX_E_INVALID_ARGUMENT for bad interest,
 * ASX_E_NOT_FOUND for stale tokens, or a reactor error. */
ASX_API ASX_MUST_USE asx_status asx_io_arm(asx_io_token *token, asx_io_interest interest,
                                           asx_task_id task);

/* Arm interest for the currently polled task and park it (live backend).
 * Returns ASX_E_PENDING on success — return it from the poll function —
 * or the arming error. Outside a scheduler poll nothing is parked. */
ASX_API ASX_MUST_USE asx_status asx_io_wait(asx_io_token *token, asx_io_interest interest);

/* Deregister an IO token. */
ASX_API void asx_io_deregister(asx_io_token *token);

/* Update the interest mask for an existing registration.
 * interest must be a nonzero mask composed only of
 * ASX_IO_READABLE|WRITABLE|ERROR. */
ASX_API ASX_MUST_USE asx_status asx_io_set_interest(asx_io_token *token, asx_io_interest interest);

/* Query the stored registration state for an existing token.
 * Returns ASX_OK on success and fills any non-NULL outputs.
 * Returns ASX_E_INVALID_ARGUMENT if token is NULL or all outputs are NULL.
 * Returns ASX_E_PERMISSION_DENIED when the IO-driver surface is unavailable
 * in the active profile, or ASX_E_INVALID_STATE when the driver has not been
 * initialized for the current runtime/bootstrap sequence.
 * Returns ASX_E_NOT_FOUND for stale or unknown tokens. */
ASX_API ASX_MUST_USE asx_status asx_io_get_registration(const asx_io_token *token, int *out_fd,
                                                        asx_io_interest *out_interest);

/* -------------------------------------------------------------------
 * API: IO driver poll
 * ------------------------------------------------------------------- */

/* Poll the reactor for IO events, waiting up to timeout_ms (live backend).
 * Collects ready events and wakes the associated tasks/wakers.
 * Returns the number of events collected. */
ASX_API uint32_t asx_io_driver_poll(asx_io_event *out_events, uint32_t max_events,
                                    uint32_t timeout_ms);

/* Get the count of active IO registrations. */
ASX_API uint32_t asx_io_active_count(void);

/* Get the count of registrations that can still produce a wakeup (live
 * backend: armed one-shot interests; ghost backend: all active). */
ASX_API uint32_t asx_io_armed_count(void);

/* Returns nonzero when the driver is initialized with the live backend. */
ASX_API int asx_io_driver_is_live(void);

/* Query the currently selected IO backend for the initialized driver.
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT for NULL, and
 * ASX_E_INVALID_STATE when the driver has not been initialized. */
ASX_API ASX_MUST_USE asx_status asx_io_driver_get_backend(asx_io_backend *out_backend);

/* -------------------------------------------------------------------
 * Reset (test support)
 * ------------------------------------------------------------------- */

/* Reset the IO driver to uninitialized state (test support). */
ASX_API void asx_io_driver_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ASX_RUNTIME_IO_DRIVER_H */
