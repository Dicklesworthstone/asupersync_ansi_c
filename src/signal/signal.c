/*
 * signal.c — signal and shutdown surface
 *
 * One subscription table serves both backends:
 *   MEMORY — deliveries come only from asx_signal_raise(); polling never
 *            parks (cooperative re-poll).
 *   NATIVE — subscriptions also receive real OS signals caught by the
 *            handlers in src/platform/posix/signal_posix.c. Polling first
 *            pumps the self-pipe into the table; with nothing pending it
 *            records the polled task as the subscription's waiter and
 *            parks it on self-pipe readiness. Any delivery (pumped or
 *            raised) wakes the waiters of the subscriptions it reaches.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/runtime/runtime.h>
#include <asx/signal/signal.h>
#include <string.h>

#if ASX_HAS_NATIVE_RUNTIME_SURFACES

#if defined(ASX_PROFILE_POSIX)
#include "signal_native.h"
#define ASX_SIGNAL_HAS_NATIVE 1
#else
#define ASX_SIGNAL_HAS_NATIVE 0
#endif

/* asx_signal_reset() always restores in-process delivery; asx_runtime_init()
 * switches to NATIVE when it installs a live reactor (see rt.c). */
#define ASX_SIGNAL_DEFAULT_BACKEND ASX_SIGNAL_BACKEND_MEMORY

typedef struct {
    asx_signal_kind kind;
    uint32_t generation;
    uint32_t pending_count;
    asx_task_id waiter; /* parked task to wake on delivery (NATIVE) */
    int native;
    int alive;
} asx_signal_slot;

static asx_signal_slot g_subscriptions[ASX_MAX_SIGNAL_SUBSCRIPTIONS];
static int g_shutdown_requested;
static asx_signal_backend g_signal_backend = ASX_SIGNAL_DEFAULT_BACKEND;

static uint32_t asx_signal_next_generation(uint32_t generation) {
    generation++;
    return generation == 0u ? 1u : generation;
}

static asx_signal_slot *asx_signal_lookup(asx_signal_subscription subscription) {
    asx_signal_slot *slot;
    if (subscription.slot >= ASX_MAX_SIGNAL_SUBSCRIPTIONS) return NULL;
    slot = &g_subscriptions[subscription.slot];
    if (!slot->alive) return NULL;
    if (slot->generation != subscription.generation) return NULL;
    return slot;
}

/* Credit `count` deliveries of `kind` to matching subscriptions (only
 * NATIVE ones for OS-caught signals) and wake their parked waiters. */
static void signal_deliver(asx_signal_kind kind, uint32_t count, int native_only) {
    uint32_t i;
    if (count == 0u) return;
    for (i = 0; i < ASX_MAX_SIGNAL_SUBSCRIPTIONS; i++) {
        asx_signal_slot *s = &g_subscriptions[i];
        if (!s->alive || s->kind != kind) continue;
        if (native_only && !s->native) continue;
        s->pending_count += count;
        if (s->waiter != ASX_INVALID_ID) {
            asx_status st = asx_task_wake(s->waiter);
            (void)st;
            s->waiter = ASX_INVALID_ID;
        }
    }
    if (kind == ASX_SIGNAL_INT || kind == ASX_SIGNAL_TERM) g_shutdown_requested = 1;
}

#if ASX_SIGNAL_HAS_NATIVE
/* Move signals caught by the OS handlers into the subscription table. */
static void signal_pump(void) {
    uint32_t counts[ASX_SIGNAL_KIND_COUNT];
    uint32_t i;
    memset(counts, 0, sizeof(counts));
    asx_native_signal_drain(counts);
    for (i = 0; i < ASX_SIGNAL_KIND_COUNT; i++) {
        signal_deliver(asx_signal_kind_at(i), counts[i], 1);
    }
}
#endif

asx_status asx_signal_set_backend(asx_signal_backend backend) {
    if (backend != ASX_SIGNAL_BACKEND_MEMORY && backend != ASX_SIGNAL_BACKEND_NATIVE) {
        return ASX_E_INVALID_ARGUMENT;
    }
#if !ASX_SIGNAL_HAS_NATIVE
    if (backend == ASX_SIGNAL_BACKEND_NATIVE) return ASX_E_PERMISSION_DENIED;
#endif
    g_signal_backend = backend;
    return ASX_OK;
}

asx_signal_backend asx_signal_get_backend(void) { return g_signal_backend; }

asx_status asx_signal_subscribe(asx_signal_subscription *out, asx_signal_kind kind) {
    uint32_t i;
    asx_signal_slot *slot;
    int native = 0;

    if (out == NULL) return ASX_E_INVALID_ARGUMENT;

    for (i = 0; i < ASX_MAX_SIGNAL_SUBSCRIPTIONS; i++) {
        if (!g_subscriptions[i].alive) break;
    }
    if (i >= ASX_MAX_SIGNAL_SUBSCRIPTIONS) return ASX_E_RESOURCE_EXHAUSTED;

#if ASX_SIGNAL_HAS_NATIVE
    if (g_signal_backend == ASX_SIGNAL_BACKEND_NATIVE) {
        asx_status st = asx_native_signal_acquire(kind);
        if (st != ASX_OK) return st;
        native = 1;
    }
#endif

    slot = &g_subscriptions[i];
    {
        uint32_t generation = asx_signal_next_generation(slot->generation);
        memset(slot, 0, sizeof(*slot));
        slot->generation = generation;
    }
    slot->alive = 1;
    slot->kind = kind;
    slot->native = native;
    slot->waiter = ASX_INVALID_ID;
    out->slot = i;
    out->generation = slot->generation;
    return ASX_OK;
}

asx_status asx_signal_poll(asx_signal_subscription subscription, uint32_t *out_count) {
    asx_signal_slot *slot;
    if (out_count == NULL) return ASX_E_INVALID_ARGUMENT;
    slot = asx_signal_lookup(subscription);
    if (slot == NULL) return ASX_E_NOT_FOUND;
#if ASX_SIGNAL_HAS_NATIVE
    if (slot->native) signal_pump();
#endif
    if (slot->pending_count > 0u) {
        *out_count = slot->pending_count;
        slot->pending_count = 0u;
        slot->waiter = ASX_INVALID_ID;
        return ASX_OK;
    }
#if ASX_SIGNAL_HAS_NATIVE
    if (slot->native) {
        slot->waiter = asx_task_current();
        return asx_native_signal_wait(subscription.slot);
    }
#endif
    return ASX_E_PENDING;
}

asx_status asx_signal_unsubscribe(asx_signal_subscription subscription) {
    asx_signal_slot *slot = asx_signal_lookup(subscription);
    if (slot == NULL) return ASX_E_NOT_FOUND;
#if ASX_SIGNAL_HAS_NATIVE
    if (slot->native) {
        asx_native_signal_forget(subscription.slot);
        asx_native_signal_release(slot->kind);
    }
#endif
    slot->alive = 0;
    return ASX_OK;
}

asx_status asx_signal_raise(asx_signal_kind kind) {
    /* Also requests shutdown for INT/TERM when nobody is subscribed. */
    signal_deliver(kind, 1u, 0);
    return ASX_OK;
}

int asx_signal_shutdown_requested(void) {
#if ASX_SIGNAL_HAS_NATIVE
    if (asx_native_signal_shutdown_flag()) return 1;
#endif
    return g_shutdown_requested;
}

void asx_signal_clear_shutdown(void) {
#if ASX_SIGNAL_HAS_NATIVE
    asx_native_signal_clear_shutdown();
#endif
    g_shutdown_requested = 0;
}

/* ------------------------------------------------------------------ */
/* Ctrl+C                                                              */
/* ------------------------------------------------------------------ */

asx_status asx_signal_ctrl_c_init(asx_signal_ctrl_c *ctrl_c) {
    asx_status st;
    if (ctrl_c == NULL) return ASX_E_INVALID_ARGUMENT;
    ctrl_c->active = 0;
    st = asx_signal_subscribe(&ctrl_c->subscription, ASX_SIGNAL_INT);
    if (st == ASX_OK) ctrl_c->active = 1;
    return st;
}

asx_status asx_signal_ctrl_c_poll(asx_signal_ctrl_c *ctrl_c) {
    uint32_t count = 0u;
    asx_status st;
    if (ctrl_c == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!ctrl_c->active) return ASX_E_INVALID_STATE;
    st = asx_signal_poll(ctrl_c->subscription, &count);
    if (st == ASX_OK) asx_signal_ctrl_c_cancel(ctrl_c);
    return st;
}

void asx_signal_ctrl_c_cancel(asx_signal_ctrl_c *ctrl_c) {
    if (ctrl_c == NULL || !ctrl_c->active) return;
    ctrl_c->active = 0;
    {
        asx_status st = asx_signal_unsubscribe(ctrl_c->subscription);
        (void)st;
    }
}

/* ------------------------------------------------------------------ */
/* Reset                                                               */
/* ------------------------------------------------------------------ */

void asx_signal_reset(void) {
    uint32_t i;
#if ASX_SIGNAL_HAS_NATIVE
    asx_native_signal_reset();
#endif
    for (i = 0; i < ASX_MAX_SIGNAL_SUBSCRIPTIONS; i++) {
        uint32_t generation = g_subscriptions[i].generation;
        memset(&g_subscriptions[i], 0, sizeof(g_subscriptions[i]));
        g_subscriptions[i].generation = generation;
    }
    g_shutdown_requested = 0;
    g_signal_backend = ASX_SIGNAL_DEFAULT_BACKEND;
}

#endif /* ASX_HAS_NATIVE_RUNTIME_SURFACES */
