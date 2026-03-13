/*
 * signal.c — deterministic signal and shutdown surface
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/signal/signal.h>
#include <string.h>

typedef struct {
    asx_signal_kind kind;
    uint32_t generation;
    uint32_t pending_count;
    int alive;
} asx_signal_slot;

static asx_signal_slot g_subscriptions[ASX_MAX_SIGNAL_SUBSCRIPTIONS];
static int g_shutdown_requested;

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

asx_status asx_signal_subscribe(asx_signal_subscription *out, asx_signal_kind kind) {
    uint32_t i;
    asx_signal_slot *slot;

    if (out == NULL) return ASX_E_INVALID_ARGUMENT;

    for (i = 0; i < ASX_MAX_SIGNAL_SUBSCRIPTIONS; i++) {
        if (!g_subscriptions[i].alive) {
            slot = &g_subscriptions[i];
            memset(slot, 0, sizeof(*slot));
            slot->alive = 1;
            slot->kind = kind;
            slot->generation = asx_signal_next_generation(slot->generation);
            out->slot = i;
            out->generation = slot->generation;
            return ASX_OK;
        }
    }

    return ASX_E_RESOURCE_EXHAUSTED;
}

asx_status asx_signal_poll(asx_signal_subscription subscription, uint32_t *out_count) {
    asx_signal_slot *slot;
    if (out_count == NULL) return ASX_E_INVALID_ARGUMENT;
    slot = asx_signal_lookup(subscription);
    if (slot == NULL) return ASX_E_NOT_FOUND;
    if (slot->pending_count == 0u) return ASX_E_PENDING;
    *out_count = slot->pending_count;
    slot->pending_count = 0u;
    return ASX_OK;
}

asx_status asx_signal_unsubscribe(asx_signal_subscription subscription) {
    asx_signal_slot *slot = asx_signal_lookup(subscription);
    if (slot == NULL) return ASX_E_NOT_FOUND;
    slot->alive = 0;
    return ASX_OK;
}

asx_status asx_signal_raise(asx_signal_kind kind) {
    uint32_t i;
    for (i = 0; i < ASX_MAX_SIGNAL_SUBSCRIPTIONS; i++) {
        if (g_subscriptions[i].alive && g_subscriptions[i].kind == kind) {
            g_subscriptions[i].pending_count++;
        }
    }
    if (kind == ASX_SIGNAL_INT || kind == ASX_SIGNAL_TERM) { g_shutdown_requested = 1; }
    return ASX_OK;
}

int asx_signal_shutdown_requested(void) { return g_shutdown_requested; }

void asx_signal_clear_shutdown(void) { g_shutdown_requested = 0; }

void asx_signal_reset(void) {
    memset(g_subscriptions, 0, sizeof(g_subscriptions));
    g_shutdown_requested = 0;
}
