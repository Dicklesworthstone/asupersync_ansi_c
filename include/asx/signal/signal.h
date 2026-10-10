/*
 * asx/signal/signal.h — signal and shutdown surface
 *
 * Two backends behind one API:
 *   MEMORY — deterministic in-process delivery (lab runtime, replay,
 *            portable core): only asx_signal_raise() delivers.
 *   NATIVE — real POSIX signals (POSIX builds). Subscribing to a kind
 *            installs a sigaction handler for it (the previous disposition
 *            is saved and restored when the last subscription for the kind
 *            goes away, and on reset). The handler only performs
 *            async-signal-safe work: it records the kind by writing one
 *            byte to a non-blocking self-pipe that the IO driver watches,
 *            so a task awaiting a subscription parks and is woken by
 *            reactor readiness.
 *
 * Both backends share the subscription table: asx_signal_raise() injects
 * a delivery in-process (no OS signal is sent; use raise()/kill() for
 * that) and wakes parked subscribers. Delivery counts accumulate per
 * subscription until polled; the OS may coalesce identical signals that
 * arrive before the handler runs.
 *
 * Shutdown: an INT or TERM delivery (raised, or caught by a NATIVE
 * handler) sets the shutdown-requested flag.
 *
 * Backend selection: asx_signal_reset() restores MEMORY (and every saved
 * OS disposition); asx_runtime_init() switches to NATIVE when it installs
 * a live readiness reactor; asx_signal_set_backend() selects one
 * explicitly. Subscriptions keep the backend they were created with.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_SIGNAL_SIGNAL_H
#define ASX_SIGNAL_SIGNAL_H

#include <asx/asx_config.h>
#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if ASX_HAS_NATIVE_RUNTIME_SURFACES

#ifndef ASX_MAX_SIGNAL_SUBSCRIPTIONS
#define ASX_MAX_SIGNAL_SUBSCRIPTIONS 16u
#endif
#if (ASX_MAX_SIGNAL_SUBSCRIPTIONS) < 1
#error "ASX_MAX_SIGNAL_SUBSCRIPTIONS must be at least 1"
#endif

/* Signal kinds (values follow the Linux numbering; NATIVE maps each kind
 * to the platform's own signal number). */
typedef enum {
    ASX_SIGNAL_HUP = 1,
    ASX_SIGNAL_INT = 2,
    ASX_SIGNAL_USR1 = 10,
    ASX_SIGNAL_USR2 = 12,
    ASX_SIGNAL_TERM = 15
} asx_signal_kind;

typedef struct {
    uint32_t slot;
    uint32_t generation;
} asx_signal_subscription;

typedef enum { ASX_SIGNAL_BACKEND_MEMORY = 0, ASX_SIGNAL_BACKEND_NATIVE = 1 } asx_signal_backend;

/* Select the backend for subscriptions created from now on.
 * Returns ASX_OK, ASX_E_INVALID_ARGUMENT for unknown values, or
 * ASX_E_PERMISSION_DENIED when NATIVE is unavailable in this build. */
ASX_API ASX_MUST_USE asx_status asx_signal_set_backend(asx_signal_backend backend);

/* Report the backend used for newly created subscriptions. */
ASX_API asx_signal_backend asx_signal_get_backend(void);

/* Subscribe to a signal kind, returning a subscription handle. NATIVE
 * installs the OS handler for the kind; ASX_E_INVALID_ARGUMENT for kinds
 * the platform cannot catch. */
ASX_API ASX_MUST_USE asx_status asx_signal_subscribe(asx_signal_subscription *out,
                                                     asx_signal_kind kind);
/* Poll a subscription: ASX_OK with the number of deliveries since the
 * last successful poll, or ASX_E_PENDING when there are none (NATIVE: the
 * polled task is parked until a signal of any subscribed kind arrives). */
ASX_API ASX_MUST_USE asx_status asx_signal_poll(asx_signal_subscription subscription,
                                                uint32_t *out_count);
/* Unsubscribe; NATIVE restores the previous OS disposition once no
 * subscription for the kind remains. */
ASX_API ASX_MUST_USE asx_status asx_signal_unsubscribe(asx_signal_subscription subscription);
/* Inject a delivery of `kind` to all matching subscriptions (both
 * backends; no OS signal is sent) and wake parked subscribers. */
ASX_API ASX_MUST_USE asx_status asx_signal_raise(asx_signal_kind kind);
/* Check if a shutdown signal (INT/TERM) has been received. */
ASX_API int asx_signal_shutdown_requested(void);
/* Clear the shutdown-requested flag. */
ASX_API void asx_signal_clear_shutdown(void);

/* -------------------------------------------------------------------
 * Ctrl+C helper (upstream signal::ctrl_c)
 * ------------------------------------------------------------------- */

typedef struct {
    asx_signal_subscription subscription;
    int active;
} asx_signal_ctrl_c;

/* Start listening for Ctrl+C (SIGINT). With the NATIVE backend the
 * process is no longer terminated by SIGINT while the listener is
 * active. */
ASX_API ASX_MUST_USE asx_status asx_signal_ctrl_c_init(asx_signal_ctrl_c *ctrl_c);

/* Poll for Ctrl+C: ASX_OK once it was pressed (the listener then
 * unsubscribes), ASX_E_PENDING otherwise (NATIVE: task parked),
 * ASX_E_INVALID_STATE if the listener is not active. */
ASX_API ASX_MUST_USE asx_status asx_signal_ctrl_c_poll(asx_signal_ctrl_c *ctrl_c);

/* Stop listening early (restores the previous SIGINT disposition).
 * Idempotent. */
ASX_API void asx_signal_ctrl_c_cancel(asx_signal_ctrl_c *ctrl_c);

/* Reset all signal state (test support): drops every subscription,
 * restores every saved OS disposition, clears the shutdown flag and
 * restores the MEMORY backend. */
ASX_API void asx_signal_reset(void);

#endif /* ASX_HAS_NATIVE_RUNTIME_SURFACES */

#ifdef __cplusplus
}
#endif

#endif /* ASX_SIGNAL_SIGNAL_H */
