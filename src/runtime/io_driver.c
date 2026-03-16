/*
 * io_driver.c — IO driver and reactor integration
 *
 * Walking skeleton: ghost reactor only, no real IO.
 * Provides the API surface for future platform-specific reactors.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx_config.h>
#include <asx/runtime/browser_boundary.h>
#include <asx/runtime/io_driver.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Internal registration slot                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    int fd;
    asx_io_interest interest;
    asx_waker waker;
    uint16_t generation;
    int alive;
} asx_io_reg;

/* ------------------------------------------------------------------ */
/* Arena                                                               */
/* ------------------------------------------------------------------ */

static asx_io_reg g_regs[ASX_MAX_IO_TOKENS];
static uint32_t g_reg_count = 0;
static uint32_t g_active_count = 0;
static int g_initialized = 0;

static uint16_t next_gen(uint16_t g) {
    g++;
    if (g == 0) g = 1;
    return g;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

asx_status asx_io_driver_init(void) {
    asx_status st = asx_surface_gate(ASX_SURFACE_IO_DRIVER);
    if (st != ASX_OK) return st;

    /* Re-initialization must start from a clean registration arena so
     * old tokens cannot survive across runtime/bootstrap boundaries. */
    asx_io_driver_reset();
    g_initialized = 1;
    return ASX_OK;
}

void asx_io_driver_shutdown(void) {
    asx_io_driver_reset();
    g_initialized = 0;
}

int asx_io_driver_is_initialized(void) { return g_initialized; }

void asx_io_driver_reset(void) {
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

asx_status asx_io_register(int fd, asx_io_interest interest, const asx_waker *waker,
                           asx_io_token *out_token) {
    uint32_t idx;
    asx_status st;

    if (out_token == NULL || waker == NULL) return ASX_E_INVALID_ARGUMENT;
    st = asx_surface_gate(ASX_SURFACE_IO_DRIVER);
    if (st != ASX_OK) return st;
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
        if (g_reg_count >= ASX_MAX_IO_TOKENS) return ASX_E_RESOURCE_EXHAUSTED;
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

void asx_io_deregister(asx_io_token *token) {
    if (token == NULL) return;
    if (token->slot >= g_reg_count) return;
    if (g_regs[token->slot].generation != token->generation) return;
    if (!g_regs[token->slot].alive) return;

    g_regs[token->slot].alive = 0;
    if (g_active_count > 0) g_active_count--;
}

asx_status asx_io_set_interest(asx_io_token *token, asx_io_interest interest) {
    asx_status st = asx_surface_gate(ASX_SURFACE_IO_DRIVER);
    if (st != ASX_OK) return st;
    if (token == NULL) return ASX_E_INVALID_ARGUMENT;
    if (token->slot >= g_reg_count) return ASX_E_NOT_FOUND;
    if (g_regs[token->slot].generation != token->generation) return ASX_E_NOT_FOUND;
    if (!g_regs[token->slot].alive) return ASX_E_NOT_FOUND;

    g_regs[token->slot].interest = interest;
    return ASX_OK;
}

asx_status asx_io_get_registration(const asx_io_token *token, int *out_fd,
                                   asx_io_interest *out_interest) {
    if (token == NULL) return ASX_E_INVALID_ARGUMENT;
    if (out_fd == NULL && out_interest == NULL) return ASX_E_INVALID_ARGUMENT;
    if (token->slot >= g_reg_count) return ASX_E_NOT_FOUND;
    if (g_regs[token->slot].generation != token->generation) return ASX_E_NOT_FOUND;
    if (!g_regs[token->slot].alive) return ASX_E_NOT_FOUND;

    if (out_fd != NULL) *out_fd = g_regs[token->slot].fd;
    if (out_interest != NULL) *out_interest = g_regs[token->slot].interest;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* IO driver poll                                                      */
/* ------------------------------------------------------------------ */

uint32_t asx_io_driver_poll(asx_io_event *out_events, uint32_t max_events, uint32_t timeout_ms) {
    uint32_t ready_count = 0;
    uint32_t collected = 0;
    uint32_t i;

    if (!g_initialized || out_events == NULL || max_events == 0u) return 0u;

    /* Walking skeleton: delegate to ghost reactor via runtime hook.
     * The hook only reports a readiness count, so we deterministically map
     * those ready slots onto the oldest active registrations. This keeps the
     * registration/poll/waker contract usable without pretending to have a
     * full fd-backed reactor yet. */
    {
        asx_status st;
        st = asx_runtime_reactor_wait(timeout_ms, &ready_count, 0);
        if (st != ASX_OK || ready_count == 0u) return 0u;
    }

    for (i = 0; i < g_reg_count && collected < ready_count && collected < max_events; i++) {
        asx_status wake_st;

        if (!g_regs[i].alive) continue;

        out_events[collected].token.slot = i;
        out_events[collected].token.generation = g_regs[i].generation;
        out_events[collected].ready = g_regs[i].interest;

        wake_st = asx_waker_wake(&g_regs[i].waker);
        (void)wake_st;

        collected++;
    }

    return collected;
}

/* ------------------------------------------------------------------ */
/* Query                                                               */
/* ------------------------------------------------------------------ */

uint32_t asx_io_active_count(void) { return g_active_count; }
