/*
 * asx/signal/signal.h — deterministic signal and shutdown surface
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

typedef enum {
    ASX_SIGNAL_HUP = 1,
    ASX_SIGNAL_INT = 2,
    ASX_SIGNAL_USR1 = 10,
    ASX_SIGNAL_TERM = 15
} asx_signal_kind;

typedef struct {
    uint32_t slot;
    uint32_t generation;
} asx_signal_subscription;

/* Subscribe to a signal kind, returning a subscription handle. */
ASX_API ASX_MUST_USE asx_status asx_signal_subscribe(asx_signal_subscription *out,
                                                     asx_signal_kind kind);
/* Poll a subscription for pending signal deliveries. */
ASX_API ASX_MUST_USE asx_status asx_signal_poll(asx_signal_subscription subscription,
                                                uint32_t *out_count);
/* Unsubscribe from a signal subscription. */
ASX_API ASX_MUST_USE asx_status asx_signal_unsubscribe(asx_signal_subscription subscription);
/* Raise a signal, delivering it to all matching subscriptions. */
ASX_API ASX_MUST_USE asx_status asx_signal_raise(asx_signal_kind kind);
/* Check if a shutdown signal (INT/TERM) has been received. */
ASX_API int asx_signal_shutdown_requested(void);
/* Clear the shutdown-requested flag. */
ASX_API void asx_signal_clear_shutdown(void);

/* Reset all signal state (test support). */
ASX_API void asx_signal_reset(void);

#endif /* ASX_HAS_NATIVE_RUNTIME_SURFACES */

#ifdef __cplusplus
}
#endif

#endif /* ASX_SIGNAL_SIGNAL_H */
