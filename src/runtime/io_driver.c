/*
 * io_driver.c — IO driver and reactor integration
 *
 * Walking skeleton: ghost reactor only, no real IO.
 * Provides the API surface for future platform-specific reactors.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/runtime/io_driver.h>
#include <asx/asx_config.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Internal registration slot                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    int               fd;
    asx_io_interest   interest;
    asx_waker         waker;
    uint16_t          generation;
    int               alive;
} asx_io_reg;

/* ------------------------------------------------------------------ */
/* Arena                                                               */
/* ------------------------------------------------------------------ */

static asx_io_reg g_regs[ASX_MAX_IO_TOKENS];
static uint32_t g_reg_count = 0;
static uint32_t g_active_count = 0;
static int g_initialized = 0;

static uint16_t next_gen(uint16_t g)
{
    g++;
    if (g == 0) g = 1;
    return g;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

asx_status asx_io_driver_init(void)
{
    g_initialized = 1;
    return ASX_OK;
}

void asx_io_driver_shutdown(void)
{
    asx_io_driver_reset();
    g_initialized = 0;
}

void asx_io_driver_reset(void)
{
    uint32_t i;
    for (i = 0; i < ASX_MAX_IO_TOKENS; i++) {
        g_regs[i].generation = next_gen(g_regs[i].generation);
        g_regs[i].alive = 0;
        g_regs[i].fd = -1;
        g_regs[i].interest = 0;
    }
    g_reg_count = 0;
    g_active_count = 0;
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

asx_status asx_io_register(int fd,
                            asx_io_interest interest,
                            const asx_waker *waker,
                            asx_io_token *out_token)
{
    uint32_t idx;

    if (out_token == NULL || waker == NULL)
        return ASX_E_INVALID_ARGUMENT;
    if (!g_initialized) return ASX_E_INVALID_STATE;

    /* Find free slot */
    idx = ASX_MAX_IO_TOKENS;
    {
        uint32_t i;
        for (i = 0; i < g_reg_count; i++) {
            if (!g_regs[i].alive) {
                idx = i;
                break;
            }
        }
    }

    if (idx == ASX_MAX_IO_TOKENS) {
        if (g_reg_count >= ASX_MAX_IO_TOKENS)
            return ASX_E_RESOURCE_EXHAUSTED;
        idx = g_reg_count++;
    }

    g_regs[idx].fd = fd;
    g_regs[idx].interest = interest;
    g_regs[idx].waker = *waker;
    g_regs[idx].generation = next_gen(g_regs[idx].generation);
    g_regs[idx].alive = 1;
    g_active_count++;

    out_token->slot = idx;
    out_token->generation = g_regs[idx].generation;

    return ASX_OK;
}

void asx_io_deregister(asx_io_token *token)
{
    if (token == NULL) return;
    if (token->slot >= g_reg_count) return;
    if (g_regs[token->slot].generation != token->generation) return;
    if (!g_regs[token->slot].alive) return;

    g_regs[token->slot].alive = 0;
    if (g_active_count > 0) g_active_count--;
}

asx_status asx_io_set_interest(asx_io_token *token, asx_io_interest interest)
{
    if (token == NULL) return ASX_E_INVALID_ARGUMENT;
    if (token->slot >= g_reg_count) return ASX_E_NOT_FOUND;
    if (g_regs[token->slot].generation != token->generation) return ASX_E_NOT_FOUND;
    if (!g_regs[token->slot].alive) return ASX_E_NOT_FOUND;

    g_regs[token->slot].interest = interest;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* IO driver poll                                                      */
/* ------------------------------------------------------------------ */

uint32_t asx_io_driver_poll(asx_io_event *out_events,
                             uint32_t max_events,
                             uint32_t timeout_ms)
{
    uint32_t ready_count = 0;

    (void)out_events;
    (void)max_events;
    (void)timeout_ms;

    /* Walking skeleton: delegate to ghost reactor via runtime hook.
     * Ghost reactor returns 0 ready events, so no wakers are signaled.
     * Real reactor integration will poll the platform reactor and
     * match ready FDs to registered tokens. */
    {
        asx_status st;
        st = asx_runtime_reactor_wait(timeout_ms, &ready_count, 0);
        (void)st;
    }

    /* In walking skeleton mode, ghost reactor returns 0 events.
     * Future: match reactor results to registered tokens and fill
     * out_events, signal associated wakers. */

    return 0;
}

/* ------------------------------------------------------------------ */
/* Query                                                               */
/* ------------------------------------------------------------------ */

uint32_t asx_io_active_count(void)
{
    return g_active_count;
}
