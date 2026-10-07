/*
 * signal_native.h — internal bridge between the signal API and real POSIX
 * signals
 *
 * NOT part of the public API. signal.c owns the subscription table and
 * delivery bookkeeping for both backends; this bridge supplies the OS
 * side for NATIVE subscriptions: refcounted sigaction installation per
 * kind, the async-signal-safe self-pipe, and per-subscription reactor
 * registrations used to park waiting tasks. The POSIX implementation
 * lives in src/platform/posix/signal_posix.c (POSIX builds only).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_SIGNAL_NATIVE_H
#define ASX_SIGNAL_NATIVE_H

#include <asx/asx_status.h>
#include <asx/signal/signal.h>
#include <stdint.h>

/* Dense index over the supported kinds (HUP, INT, USR1, USR2, TERM). */
#define ASX_SIGNAL_KIND_COUNT 5u

static inline int asx_signal_kind_index(asx_signal_kind kind) {
    switch (kind) {
    case ASX_SIGNAL_HUP: return 0;
    case ASX_SIGNAL_INT: return 1;
    case ASX_SIGNAL_USR1: return 2;
    case ASX_SIGNAL_USR2: return 3;
    case ASX_SIGNAL_TERM: return 4;
    }
    return -1;
}

static inline asx_signal_kind asx_signal_kind_at(uint32_t index) {
    static const asx_signal_kind kinds[ASX_SIGNAL_KIND_COUNT] = {ASX_SIGNAL_HUP, ASX_SIGNAL_INT,
                                                                 ASX_SIGNAL_USR1, ASX_SIGNAL_USR2,
                                                                 ASX_SIGNAL_TERM};
    return kinds[index < ASX_SIGNAL_KIND_COUNT ? index : 0u];
}

/* Restore every saved disposition, drop every wait registration, drain
 * the self-pipe and clear the shutdown flag. */
void asx_native_signal_reset(void);

/* Take / drop a reference on the OS handler for `kind` (installed on the
 * first reference, previous disposition restored on the last).
 * acquire: ASX_OK, ASX_E_INVALID_ARGUMENT for unsupported kinds, or a
 * mapped errno (pipe/sigaction failure; nothing is left installed). */
asx_status asx_native_signal_acquire(asx_signal_kind kind);
void asx_native_signal_release(asx_signal_kind kind);

/* Drain the self-pipe, adding caught deliveries per kind index. */
void asx_native_signal_drain(uint32_t counts[ASX_SIGNAL_KIND_COUNT]);

/* Arm self-pipe readiness for subscription `slot` on behalf of the task
 * being polled and park it. Returns ASX_E_PENDING (also outside a task
 * poll, where nothing is armed) or an arming error. */
asx_status asx_native_signal_wait(uint32_t slot);

/* Drop the wait registration of subscription `slot`. */
void asx_native_signal_forget(uint32_t slot);

/* Shutdown flag set by caught INT/TERM. */
int asx_native_signal_shutdown_flag(void);
void asx_native_signal_clear_shutdown(void);

#endif /* ASX_SIGNAL_NATIVE_H */
