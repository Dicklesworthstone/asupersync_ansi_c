/*
 * asx/runtime/io_driver.h — IO driver and reactor integration
 *
 * The IO driver owns the reactor lifecycle and provides the bridge
 * between external IO events and task wakeups. It integrates with
 * the reactor hooks (real or ghost) and the waker system.
 *
 * Walking skeleton: single-threaded, uses ghost reactor for
 * deterministic testing. Real reactor integration deferred to
 * platform hook implementation.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_RUNTIME_IO_DRIVER_H
#define ASX_RUNTIME_IO_DRIVER_H

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <asx/asx_ids.h>
#include <asx/runtime/waker.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ASX_MAX_IO_TOKENS 32u

/* -------------------------------------------------------------------
 * IO interest
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_IO_READABLE  = 0x01,
    ASX_IO_WRITABLE  = 0x02,
    ASX_IO_ERROR     = 0x04
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
    asx_io_token    token;
    asx_io_interest ready;    /* bitmask of ready interests */
} asx_io_event;

/* -------------------------------------------------------------------
 * API: IO driver lifecycle
 * ------------------------------------------------------------------- */

/* Initialize the IO driver. Must be called after runtime hooks
 * are installed. Uses the reactor hooks from runtime config. */
ASX_API ASX_MUST_USE asx_status asx_io_driver_init(void);

/* Shut down the IO driver, deregistering all tokens. */
ASX_API void asx_io_driver_shutdown(void);

/* -------------------------------------------------------------------
 * API: IO registration
 * ------------------------------------------------------------------- */

/* Register interest in IO events for a resource.
 * fd is an opaque file descriptor (platform-dependent).
 * interest is a bitmask of ASX_IO_READABLE|WRITABLE|ERROR.
 * waker will be signaled when the interest is ready.
 * Returns ASX_OK on success. */
ASX_API ASX_MUST_USE asx_status asx_io_register(
    int fd,
    asx_io_interest interest,
    const asx_waker *waker,
    asx_io_token *out_token);

/* Deregister an IO token. */
ASX_API void asx_io_deregister(asx_io_token *token);

/* Update the interest mask for an existing registration. */
ASX_API ASX_MUST_USE asx_status asx_io_set_interest(
    asx_io_token *token,
    asx_io_interest interest);

/* -------------------------------------------------------------------
 * API: IO driver poll
 * ------------------------------------------------------------------- */

/* Poll the reactor for IO events. Non-blocking in walking skeleton.
 * Collects ready events and signals associated wakers.
 * Returns the number of events collected. */
ASX_API uint32_t asx_io_driver_poll(
    asx_io_event *out_events,
    uint32_t max_events,
    uint32_t timeout_ms);

/* Get the count of active IO registrations. */
ASX_API uint32_t asx_io_active_count(void);

/* -------------------------------------------------------------------
 * Reset (test support)
 * ------------------------------------------------------------------- */

ASX_API void asx_io_driver_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_RUNTIME_IO_DRIVER_H */
