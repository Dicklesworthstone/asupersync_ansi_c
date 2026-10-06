/*
 * io_driver.c — IO driver and reactor integration
 *
 * Two backends behind one registration arena:
 *
 *   live  — non-deterministic builds whose reactor hooks report per-fd
 *           readiness (POSIX: epoll / poll(2)). Registrations are armed
 *           one-shot with a (generation, slot) token; readiness wakes the
 *           exact task or waker that armed the registration.
 *   ghost — deterministic builds (and reactors without readiness hooks).
 *           Registration and quiescence semantics are preserved; the
 *           count-only reactor wait is mapped deterministically onto the
 *           oldest registrations so replay never depends on OS timing.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx_config.h>
#include <asx/runtime/browser_boundary.h>
#include <asx/runtime/io_driver.h>
#include <asx/runtime/runtime.h>
#include <string.h>

#if ASX_HAS_NATIVE_IO_DRIVER
extern asx_io_backend asx_runtime_active_io_backend_selected(void);

/* ------------------------------------------------------------------ */
/* Internal registration slot                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    int fd;
    asx_io_interest interest;
    asx_waker waker;  /* legacy wake target (when has_waker) */
    asx_task_id task; /* preferred wake target (ASX_INVALID_ID if none) */
    uint16_t generation;
    int alive;
    int has_waker;
    int armed; /* live backend: one-shot interest outstanding */
} asx_io_reg;

/* ------------------------------------------------------------------ */
/* Arena                                                               */
/* ------------------------------------------------------------------ */

static asx_io_reg g_regs[ASX_MAX_IO_TOKENS];
static uint32_t g_reg_count = 0;
static uint32_t g_active_count = 0;
static int g_initialized = 0;
static int g_live = 0;
static asx_io_backend g_backend = ASX_IO_BACKEND_GHOST;

static uint16_t next_gen(uint16_t g) {
    g++;
    if (g == 0) g = 1;
    return g;
}

static int io_interest_valid(asx_io_interest interest) {
    uint32_t valid_mask =
        (uint32_t)ASX_IO_READABLE | (uint32_t)ASX_IO_WRITABLE | (uint32_t)ASX_IO_ERROR;
    uint32_t value = (uint32_t)interest;

    if (value == 0u) return 0;
    return (value & ~valid_mask) == 0u;
}

static uint64_t io_token_value(uint32_t slot) {
    return ((uint64_t)g_regs[slot].generation << 32) | (uint64_t)slot;
}

static asx_io_reg *io_lookup(const asx_io_token *token) {
    if (token == NULL) return NULL;
    if (token->slot >= g_reg_count) return NULL;
    if (g_regs[token->slot].generation != token->generation) return NULL;
    if (!g_regs[token->slot].alive) return NULL;
    return &g_regs[token->slot];
}

/* Arm one-shot readiness in the live backend. */
static asx_status io_arm_live(uint32_t slot) {
    asx_io_reg *r = &g_regs[slot];
    asx_status st;
    if (!g_live) return ASX_OK;
    st = asx_runtime_reactor_register(r->fd, (uint32_t)r->interest, io_token_value(slot));
    if (st == ASX_OK) r->armed = 1;
    return st;
}

static void io_wake(asx_io_reg *r) {
    if (r->task != ASX_INVALID_ID) {
        asx_status st = asx_task_wake(r->task);
        (void)st;
    }
    if (r->has_waker) {
        asx_status st = asx_waker_wake(&r->waker);
        (void)st;
    }
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

asx_status asx_io_driver_init(void) {
    asx_status st = asx_surface_gate(ASX_SURFACE_IO_DRIVER);
    if (st != ASX_OK) return st;

    g_backend = asx_runtime_active_io_backend_selected();
    if (g_backend == ASX_IO_BACKEND_IO_URING) return ASX_E_PERMISSION_DENIED;

    /* Re-initialization must start from a clean registration arena so
     * old tokens cannot survive across runtime/bootstrap boundaries. */
    asx_io_driver_reset();
#if ASX_DETERMINISTIC
    g_live = 0;
#else
    g_live = asx_runtime_reactor_has_readiness();
#endif
    g_initialized = 1;
    return ASX_OK;
}

void asx_io_driver_shutdown(void) {
    asx_io_driver_reset();
    g_initialized = 0;
    g_live = 0;
    g_backend = ASX_IO_BACKEND_GHOST;
}

int asx_io_driver_is_initialized(void) { return g_initialized; }

int asx_io_driver_is_live(void) { return g_initialized && g_live; }

void asx_io_driver_reset(void) {
    uint32_t i;
    for (i = 0; i < ASX_MAX_IO_TOKENS; i++) {
        if (g_live && g_regs[i].alive && g_regs[i].fd >= 0) {
            asx_runtime_reactor_deregister(g_regs[i].fd);
        }
        g_regs[i].generation = next_gen(g_regs[i].generation);
        g_regs[i].alive = 0;
        g_regs[i].armed = 0;
        g_regs[i].fd = -1;
        g_regs[i].interest = 0;
        g_regs[i].task = ASX_INVALID_ID;
        g_regs[i].has_waker = 0;
    }
    g_reg_count = 0;
    g_active_count = 0;
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

static asx_status io_alloc(int fd, asx_io_interest interest, asx_io_token *out_token,
                           uint32_t *out_idx) {
    uint32_t idx = ASX_MAX_IO_TOKENS;
    uint32_t i;

    for (i = 0; i < g_reg_count; i++) {
        /* ASX_CHECKPOINT_WAIVER("bounded slot search") */
        if (!g_regs[i].alive) {
            idx = i;
            break;
        }
    }
    if (idx == ASX_MAX_IO_TOKENS) {
        if (g_reg_count >= ASX_MAX_IO_TOKENS) return ASX_E_RESOURCE_EXHAUSTED;
        idx = g_reg_count++;
    }

    g_regs[idx].fd = fd;
    g_regs[idx].interest = interest;
    g_regs[idx].task = ASX_INVALID_ID;
    g_regs[idx].has_waker = 0;
    g_regs[idx].armed = 0;
    g_regs[idx].generation = next_gen(g_regs[idx].generation);
    g_regs[idx].alive = 1;
    g_active_count++;

    out_token->slot = idx;
    out_token->generation = g_regs[idx].generation;
    *out_idx = idx;
    return ASX_OK;
}

static void io_free(uint32_t idx) {
    if (g_live && g_regs[idx].fd >= 0) asx_runtime_reactor_deregister(g_regs[idx].fd);
    g_regs[idx].alive = 0;
    g_regs[idx].armed = 0;
    g_regs[idx].task = ASX_INVALID_ID;
    g_regs[idx].has_waker = 0;
    if (g_active_count > 0) g_active_count--;
}

asx_status asx_io_register(int fd, asx_io_interest interest, const asx_waker *waker,
                           asx_io_token *out_token) {
    uint32_t idx;
    asx_status st;

    if (out_token == NULL || waker == NULL) return ASX_E_INVALID_ARGUMENT;
    st = asx_surface_gate(ASX_SURFACE_IO_DRIVER);
    if (st != ASX_OK) return st;
    if (!g_initialized) return ASX_E_INVALID_STATE;
    if (!io_interest_valid(interest)) return ASX_E_INVALID_ARGUMENT;
    if (asx_waker_task(waker) == ASX_INVALID_ID) return ASX_E_INVALID_ARGUMENT;

    st = io_alloc(fd, interest, out_token, &idx);
    if (st != ASX_OK) return st;
    g_regs[idx].waker = *waker;
    g_regs[idx].has_waker = 1;

    /* Live backend: arm immediately; failure is atomic (no registration). */
    st = io_arm_live(idx);
    if (st != ASX_OK) {
        io_free(idx);
        return st;
    }
    return ASX_OK;
}

asx_status asx_io_register_fd(int fd, asx_io_token *out_token) {
    uint32_t idx;
    asx_status st;

    if (out_token == NULL || fd < 0) return ASX_E_INVALID_ARGUMENT;
    st = asx_surface_gate(ASX_SURFACE_IO_DRIVER);
    if (st != ASX_OK) return st;
    if (!g_initialized) return ASX_E_INVALID_STATE;
    return io_alloc(fd, ASX_IO_READABLE, out_token, &idx);
}

void asx_io_deregister(asx_io_token *token) {
    asx_io_reg *r = io_lookup(token);
    if (r == NULL) return;
    io_free(token->slot);
}

asx_status asx_io_set_interest(asx_io_token *token, asx_io_interest interest) {
    asx_io_reg *r;
    asx_status st = asx_surface_gate(ASX_SURFACE_IO_DRIVER);
    if (st != ASX_OK) return st;
    if (token == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!io_interest_valid(interest)) return ASX_E_INVALID_ARGUMENT;
    r = io_lookup(token);
    if (r == NULL) return ASX_E_NOT_FOUND;

    r->interest = interest;
    return io_arm_live(token->slot);
}

asx_status asx_io_arm(asx_io_token *token, asx_io_interest interest, asx_task_id task) {
    asx_io_reg *r;
    asx_status st = asx_surface_gate(ASX_SURFACE_IO_DRIVER);
    if (st != ASX_OK) return st;
    if (token == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!io_interest_valid(interest)) return ASX_E_INVALID_ARGUMENT;
    r = io_lookup(token);
    if (r == NULL) return ASX_E_NOT_FOUND;

    r->interest = interest;
    r->task = task;
    return io_arm_live(token->slot);
}

asx_status asx_io_wait(asx_io_token *token, asx_io_interest interest) {
    asx_task_id self = asx_task_current();
    asx_status st = asx_io_arm(token, interest, self);
    if (st != ASX_OK) return st;
    /* Park only when the readiness can actually wake us; the ghost backend
     * keeps cooperative re-polling. */
    if (g_live && self != ASX_INVALID_ID) {
        st = asx_task_park(self);
        (void)st;
    }
    return ASX_E_PENDING;
}

asx_status asx_io_get_registration(const asx_io_token *token, int *out_fd,
                                   asx_io_interest *out_interest) {
    asx_io_reg *r;
    asx_status st;

    st = asx_surface_gate(ASX_SURFACE_IO_DRIVER);
    if (st != ASX_OK) return st;
    if (token == NULL) return ASX_E_INVALID_ARGUMENT;
    if (out_fd == NULL && out_interest == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!g_initialized) return ASX_E_INVALID_STATE;
    r = io_lookup(token);
    if (r == NULL) return ASX_E_NOT_FOUND;

    if (out_fd != NULL) *out_fd = r->fd;
    if (out_interest != NULL) *out_interest = r->interest;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* IO driver poll                                                      */
/* ------------------------------------------------------------------ */

static uint32_t io_poll_live(asx_io_event *out_events, uint32_t max_events, uint32_t timeout_ms) {
    asx_reactor_event events[32];
    uint32_t n = 0;
    uint32_t collected = 0;
    uint32_t i;
    uint32_t cap = max_events < 32u ? max_events : 32u;

    if (asx_runtime_reactor_poll(timeout_ms, events, cap, &n) != ASX_OK) return 0u;
    for (i = 0; i < n; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: n <= 32 reactor events");
        uint32_t slot = (uint32_t)(events[i].token & 0xFFFFFFFFu);
        uint16_t gen = (uint16_t)(events[i].token >> 32);
        asx_io_reg *r;

        if (slot >= g_reg_count) continue;
        r = &g_regs[slot];
        if (!r->alive || r->generation != gen) continue;
        r->armed = 0;
        io_wake(r);

        out_events[collected].token.slot = slot;
        out_events[collected].token.generation = gen;
        out_events[collected].ready = (asx_io_interest)events[i].ready;
        collected++;
    }
    return collected;
}

uint32_t asx_io_driver_poll(asx_io_event *out_events, uint32_t max_events, uint32_t timeout_ms) {
    uint32_t ready_count = 0;
    uint32_t collected = 0;
    uint32_t i;

    if (!g_initialized || out_events == NULL || max_events == 0u) return 0u;
    if (g_live) return io_poll_live(out_events, max_events, timeout_ms);

    /* Ghost backend: the hook only reports a readiness count, so ready
     * slots are mapped deterministically onto the oldest active
     * registrations. Replay never depends on OS readiness order. */
    {
        asx_status st;
        st = asx_runtime_reactor_wait(timeout_ms, &ready_count, 0);
        if (st != ASX_OK || ready_count == 0u) return 0u;
    }

    for (i = 0; i < g_reg_count && collected < ready_count && collected < max_events; i++) {
        /* ASX_CHECKPOINT_WAIVER("bounded arena scan") */
        if (!g_regs[i].alive) continue;

        out_events[collected].token.slot = i;
        out_events[collected].token.generation = g_regs[i].generation;
        out_events[collected].ready = g_regs[i].interest;

        io_wake(&g_regs[i]);
        collected++;
    }

    return collected;
}

/* ------------------------------------------------------------------ */
/* Query                                                               */
/* ------------------------------------------------------------------ */

uint32_t asx_io_active_count(void) { return g_active_count; }

uint32_t asx_io_armed_count(void) {
    uint32_t i;
    uint32_t n = 0;
    if (!g_live) return g_active_count;
    for (i = 0; i < g_reg_count; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: g_reg_count <= ASX_MAX_IO_TOKENS");
        if (g_regs[i].alive && g_regs[i].armed) n++;
    }
    return n;
}

asx_status asx_io_driver_get_backend(asx_io_backend *out_backend) {
    if (out_backend == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!g_initialized) return ASX_E_INVALID_STATE;
    *out_backend = g_backend;
    return ASX_OK;
}
#endif
