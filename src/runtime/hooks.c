/*
 * hooks.c — runtime hook management, validation, and dispatch
 *
 * Implements the hook contract from asx_config.h:
 *   - Hook initialization with safe defaults (malloc-based allocator, stderr log)
 *   - Deterministic mode validation (forbids ambient entropy, requires logical clock)
 *   - Allocator seal for hardened/no-allocation profiles
 *   - Hook-backed runtime helpers that dispatch through the active hook table
 *
 * ASX_CHECKPOINT_WAIVER_FILE("codec-and-hooks: all loops in this file are either "
 *   "codec parsing (JSON/binary input processing), hook dispatch, or fault-injection "
 *   "table scans. None are on the task poll hot path. Codec input lengths are bounded "
 *   "by the codec buffer capacity contract.")
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx_config.h>
#include <asx/portable.h>
#include <asx/runtime/hindsight.h>
#include <asx/codec/codec.h>
#include <asx/codec/equivalence.h>
#include "codec_internal.h"
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#if CHAR_BIT != 8
#error "asx binary codec requires 8-bit bytes"
#endif

/* ------------------------------------------------------------------ */
/* Default hook implementations                                       */
/* ------------------------------------------------------------------ */

static void *default_malloc(void *ctx, size_t size) {
    (void)ctx;
    return malloc(size);
}

static void *default_realloc(void *ctx, void *ptr, size_t size) {
    (void)ctx;
    return realloc(ptr, size);
}

static void default_free(void *ctx, void *ptr) {
    (void)ctx;
    free(ptr);
}

static asx_time default_logical_clock(void *ctx) {
    (void)ctx;
    return 0; /* logical clock starts at 0, advanced by runtime */
}

static asx_time default_wall_clock(void *ctx) {
    (void)ctx;
    return 0; /* stub: real platforms override this */
}

static uint64_t default_seeded_entropy(void *ctx) {
    /* Deterministic PRNG: simple counter-based default.
     * Real deployments should provide a proper seeded PRNG. */
    static uint64_t state = 0x5DEECE66DULL;
    (void)ctx;
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return state;
}

static asx_status default_ghost_reactor_wait(void *ctx, uint64_t logical_step,
                                              uint32_t *ready_count) {
    (void)ctx; (void)logical_step;
    *ready_count = 0; /* no events in stub ghost reactor */
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Global hook state                                                  */
/* ------------------------------------------------------------------ */

static asx_runtime_hooks g_hooks;
static int g_hooks_installed = 0;

/* ------------------------------------------------------------------ */
/* Safety profile queries                                             */
/* ------------------------------------------------------------------ */

asx_safety_profile asx_safety_profile_active(void) {
    return (asx_safety_profile)ASX_SAFETY_PROFILE_SELECTED;
}

const char *asx_safety_profile_str(asx_safety_profile profile) {
    switch (profile) {
        case ASX_SAFETY_DEBUG:    return "debug";
        case ASX_SAFETY_HARDENED: return "hardened";
        case ASX_SAFETY_RELEASE:  return "release";
    }
    return "unknown";
}

asx_containment_policy asx_containment_policy_for_profile(
    asx_safety_profile profile) {
    switch (profile) {
        case ASX_SAFETY_DEBUG:
            return ASX_CONTAIN_FAIL_FAST;
        case ASX_SAFETY_HARDENED:
            return ASX_CONTAIN_POISON_REGION;
        case ASX_SAFETY_RELEASE:
            return ASX_CONTAIN_ERROR_ONLY;
    }
    return ASX_CONTAIN_ERROR_ONLY;
}

asx_containment_policy asx_containment_policy_active(void) {
    return asx_containment_policy_for_profile(asx_safety_profile_active());
}

/* ------------------------------------------------------------------ */
/* Fault injection state (deterministic-mode only)                    */
/* ------------------------------------------------------------------ */

#define ASX_FAULT_MAX_ACTIVE 8u

static asx_fault_injection g_faults[ASX_FAULT_MAX_ACTIVE];
static uint32_t g_fault_count = 0;
static uint32_t g_fault_clock_calls = 0;
static uint32_t g_fault_entropy_calls = 0;
static uint32_t g_fault_alloc_calls = 0;

asx_status asx_fault_inject(const asx_fault_injection *fault) {
    if (!fault) return ASX_E_INVALID_ARGUMENT;
    if (fault->kind == ASX_FAULT_NONE) return ASX_E_INVALID_ARGUMENT;
    if (g_fault_count >= ASX_FAULT_MAX_ACTIVE)
        return ASX_E_RESOURCE_EXHAUSTED;
    g_faults[g_fault_count] = *fault;
    g_fault_count++;
    return ASX_OK;
}

asx_status asx_fault_clear(void) {
    memset(g_faults, 0, sizeof(g_faults));
    g_fault_count = 0;
    g_fault_clock_calls = 0;
    g_fault_entropy_calls = 0;
    g_fault_alloc_calls = 0;
    return ASX_OK;
}

uint32_t asx_fault_injection_count(void) {
    return g_fault_count;
}

/* Check whether a fault should fire; returns 1 if fault is active.
 * Updates the fault's internal tracking via call_counter. */
static int fault_should_fire(asx_fault_injection *f, uint32_t call_number) {
    if (call_number < f->trigger_after) return 0;
    if (f->trigger_count > 0) {
        uint32_t injected = call_number - f->trigger_after;
        if (injected >= f->trigger_count) return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Hook initialization                                                */
/* ------------------------------------------------------------------ */

asx_status asx_runtime_hooks_init(asx_runtime_hooks *hooks) {
    if (!hooks) return ASX_E_INVALID_ARGUMENT;
    memset(hooks, 0, sizeof(*hooks));

    /* Default allocator: stdlib malloc/realloc/free */
    hooks->allocator.malloc_fn  = default_malloc;
    hooks->allocator.realloc_fn = default_realloc;
    hooks->allocator.free_fn    = default_free;

    /* Log sink is opt-in (NULL by default) */

    /* Deterministic-safe defaults: logical clock, seeded PRNG, ghost reactor */
    hooks->clock.now_ns_fn         = default_wall_clock;
    hooks->clock.logical_now_ns_fn = default_logical_clock;
    hooks->entropy.random_u64_fn   = default_seeded_entropy;
    hooks->reactor.ghost_wait_fn   = default_ghost_reactor_wait;

    hooks->deterministic_seeded_prng = 1;
    hooks->allocator_sealed = 0;

    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Hook validation                                                    */
/* ------------------------------------------------------------------ */

asx_status asx_runtime_hooks_validate(const asx_runtime_hooks *hooks,
                                       int deterministic_mode) {
    if (!hooks) return ASX_E_INVALID_ARGUMENT;

    /* Allocator is mandatory */
    if (!hooks->allocator.malloc_fn || !hooks->allocator.free_fn)
        return ASX_E_INVALID_ARGUMENT;

    if (deterministic_mode) {
        /* Deterministic mode requires a logical clock */
        if (!hooks->clock.logical_now_ns_fn)
            return ASX_E_DETERMINISM_VIOLATION;

        /* Deterministic mode forbids ambient entropy unless seeded PRNG is configured */
        if (hooks->entropy.random_u64_fn && !hooks->deterministic_seeded_prng)
            return ASX_E_DETERMINISM_VIOLATION;

        /* Ghost reactor required if reactor is configured in deterministic mode */
        if (hooks->reactor.wait_fn && !hooks->reactor.ghost_wait_fn)
            return ASX_E_DETERMINISM_VIOLATION;
    } else {
        /* Live mode needs a real clock */
        if (!hooks->clock.now_ns_fn)
            return ASX_E_INVALID_ARGUMENT;
    }

    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Hook installation and retrieval                                    */
/* ------------------------------------------------------------------ */

asx_status asx_runtime_set_hooks(const asx_runtime_hooks *hooks) {
    if (!hooks) return ASX_E_INVALID_ARGUMENT;
    g_hooks = *hooks;
    g_hooks_installed = 1;
    return ASX_OK;
}

const asx_runtime_hooks *asx_runtime_get_hooks(void) {
    if (!g_hooks_installed) return NULL;
    return &g_hooks;
}

/* ------------------------------------------------------------------ */
/* Allocator seal                                                     */
/* ------------------------------------------------------------------ */

asx_status asx_runtime_seal_allocator(void) {
    if (!g_hooks_installed) return ASX_E_INVALID_STATE;
    if (g_hooks.allocator_sealed) return ASX_E_INVALID_STATE; /* already sealed */
    g_hooks.allocator_sealed = 1;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Hook-backed runtime helpers                                        */
/* ------------------------------------------------------------------ */

asx_status asx_runtime_alloc(size_t size, void **out_ptr) {
    void *p;
    uint32_t i;

    if (!out_ptr) return ASX_E_INVALID_ARGUMENT;
    if (!g_hooks_installed) return ASX_E_INVALID_STATE;
    if (g_hooks.allocator_sealed) return ASX_E_ALLOCATOR_SEALED;
    if (!g_hooks.allocator.malloc_fn) return ASX_E_INVALID_STATE;

    /* Check for allocation fault injection */
    for (i = 0; i < g_fault_count; i++) {
        if (g_faults[i].kind == ASX_FAULT_ALLOC_FAIL &&
            fault_should_fire(&g_faults[i], g_fault_alloc_calls)) {
            g_fault_alloc_calls++;
            return ASX_E_RESOURCE_EXHAUSTED;
        }
    }
    g_fault_alloc_calls++;

    p = g_hooks.allocator.malloc_fn(g_hooks.allocator.ctx, size);
    if (!p) return ASX_E_RESOURCE_EXHAUSTED;
    *out_ptr = p;
    return ASX_OK;
}

asx_status asx_runtime_realloc(void *ptr, size_t size, void **out_ptr) {
    void *p;
    if (!out_ptr) return ASX_E_INVALID_ARGUMENT;
    if (!g_hooks_installed) return ASX_E_INVALID_STATE;
    if (g_hooks.allocator_sealed) return ASX_E_ALLOCATOR_SEALED;
    if (!g_hooks.allocator.realloc_fn) return ASX_E_INVALID_STATE;

    p = g_hooks.allocator.realloc_fn(g_hooks.allocator.ctx, ptr, size);
    if (!p && size > 0) return ASX_E_RESOURCE_EXHAUSTED;
    *out_ptr = p;
    return ASX_OK;
}

asx_status asx_runtime_free(void *ptr) {
    if (!g_hooks_installed) return ASX_E_INVALID_STATE;
    if (!g_hooks.allocator.free_fn) return ASX_E_INVALID_STATE;
    g_hooks.allocator.free_fn(g_hooks.allocator.ctx, ptr);
    return ASX_OK;
}

asx_status asx_runtime_now_ns(asx_time *out_now) {
    asx_time raw;
    uint32_t i;

    if (!out_now) return ASX_E_INVALID_ARGUMENT;
    if (!g_hooks_installed) return ASX_E_INVALID_STATE;

#if ASX_DETERMINISTIC
    if (g_hooks.clock.logical_now_ns_fn) {
        raw = g_hooks.clock.logical_now_ns_fn(g_hooks.clock.ctx);
    } else
#endif
    if (g_hooks.clock.now_ns_fn) {
        raw = g_hooks.clock.now_ns_fn(g_hooks.clock.ctx);
    } else {
        return ASX_E_INVALID_STATE;
    }

    /* Apply clock fault injection */
    for (i = 0; i < g_fault_count; i++) {
        if (g_faults[i].kind == ASX_FAULT_CLOCK_SKEW &&
            fault_should_fire(&g_faults[i], g_fault_clock_calls)) {
            raw += (asx_time)g_faults[i].param;
        } else if (g_faults[i].kind == ASX_FAULT_CLOCK_REVERSE &&
                   fault_should_fire(&g_faults[i], g_fault_clock_calls)) {
            if (raw >= (asx_time)g_faults[i].param) {
                raw -= (asx_time)g_faults[i].param;
            } else {
                raw = 0;
            }
        }
    }
    g_fault_clock_calls++;

    /* Log nondeterministic clock boundary event */
    asx_hindsight_log(ASX_ND_CLOCK_READ, 0, (uint64_t)raw);

    *out_now = raw;
    return ASX_OK;
}

asx_status asx_runtime_random_u64(uint64_t *out_value) {
    uint32_t i;

    if (!out_value) return ASX_E_INVALID_ARGUMENT;
    if (!g_hooks_installed) return ASX_E_INVALID_STATE;

#if ASX_DETERMINISTIC
    /* In deterministic mode, entropy is forbidden unless seeded PRNG */
    if (!g_hooks.deterministic_seeded_prng)
        return ASX_E_INVALID_STATE;
#endif
    if (!g_hooks.entropy.random_u64_fn) return ASX_E_INVALID_STATE;
    *out_value = g_hooks.entropy.random_u64_fn(g_hooks.entropy.ctx);

    /* Apply entropy fault injection */
    for (i = 0; i < g_fault_count; i++) {
        if (g_faults[i].kind == ASX_FAULT_ENTROPY_CONST &&
            fault_should_fire(&g_faults[i], g_fault_entropy_calls)) {
            *out_value = g_faults[i].param;
        }
    }
    g_fault_entropy_calls++;

    /* Log nondeterministic entropy boundary event */
    asx_hindsight_log(ASX_ND_ENTROPY_READ, 0, *out_value);

    return ASX_OK;
}

asx_status asx_runtime_reactor_wait(uint32_t timeout_ms,
                                     uint32_t *out_ready_count,
                                     uint64_t logical_step) {
    if (!out_ready_count) return ASX_E_INVALID_ARGUMENT;
    if (!g_hooks_installed) return ASX_E_INVALID_STATE;

#if ASX_DETERMINISTIC
    if (g_hooks.reactor.ghost_wait_fn) {
        asx_status rs_ = g_hooks.reactor.ghost_wait_fn(
            g_hooks.reactor.ctx, logical_step, out_ready_count);
        if (rs_ == ASX_OK) {
            asx_hindsight_log(
                *out_ready_count > 0 ? ASX_ND_IO_READY : ASX_ND_IO_TIMEOUT,
                0, (uint64_t)*out_ready_count);
        }
        return rs_;
    }
#endif
    if (g_hooks.reactor.wait_fn) {
        asx_status rs_ = g_hooks.reactor.wait_fn(
            g_hooks.reactor.ctx, timeout_ms, out_ready_count);
        if (rs_ == ASX_OK) {
            asx_hindsight_log(
                *out_ready_count > 0 ? ASX_ND_IO_READY : ASX_ND_IO_TIMEOUT,
                0, (uint64_t)*out_ready_count);
        }
        return rs_;
    }
    return ASX_E_INVALID_STATE;
}

asx_status asx_runtime_log_write(int level, const char *message) {
    if (!g_hooks_installed) return ASX_E_INVALID_STATE;
    if (!g_hooks.log.write_fn) return ASX_OK; /* silent if no log hook */
    g_hooks.log.write_fn(g_hooks.log.ctx, level, message);
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Config initialization                                              */
/* ------------------------------------------------------------------ */

void asx_runtime_config_init(asx_runtime_config *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->size = (uint32_t)sizeof(*cfg);
#if defined(ASX_PROFILE_EMBEDDED_ROUTER) || defined(ASX_PROFILE_HFT)
    cfg->wait_policy = ASX_WAIT_BUSY_SPIN;
#elif defined(ASX_PROFILE_AUTOMOTIVE)
    cfg->wait_policy = ASX_WAIT_SLEEP;
#else
    cfg->wait_policy = ASX_WAIT_YIELD;
#endif
    cfg->leak_response = ASX_LEAK_LOG;
    cfg->finalizer_poll_budget = 100;
    cfg->finalizer_time_budget_ns = (uint64_t)5000000000ULL; /* 5 seconds */
    cfg->finalizer_escalation = ASX_FINALIZER_BOUNDED_LOG;
    cfg->max_cancel_chain_depth = 16;
    cfg->max_cancel_chain_memory = 4096;
}

/* ------------------------------------------------------------------ */
/* Codec schema + JSON baseline implementation (bd-2n0.1)             */
/* ------------------------------------------------------------------ */

static int asx_codec_text_nonempty(const char *text)
{
    return text != NULL && text[0] != '\0';
}

static int asx_codec_is_lower_hex(char ch)
{
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
}

static const char *asx_codec_json_skip_ws(const char *cursor)
{
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\n' || *cursor == '\r') {
        cursor++;
    }
    return cursor;
}

static asx_status asx_codec_json_scan_string(const char *cursor, const char **out_next)
{
    const char *scan;

    if (cursor == NULL || out_next == NULL || *cursor != '"') {
        return ASX_E_INVALID_ARGUMENT;
    }

    scan = cursor + 1;
    while (*scan != '\0') {
        if (*scan == '"') {
            *out_next = scan + 1;
            return ASX_OK;
        }
        if (*scan == '\\') {
            scan++;
            if (*scan == '\0') {
                return ASX_E_INVALID_ARGUMENT;
            }
            if (*scan == 'u') {
                int i;
                for (i = 0; i < 4; i++) {
                    scan++;
                    if (!asx_codec_is_lower_hex(*scan) &&
                        !(*scan >= 'A' && *scan <= 'F')) {
                        return ASX_E_INVALID_ARGUMENT;
                    }
                }
            }
        }
        scan++;
    }

    return ASX_E_INVALID_ARGUMENT;
}

static asx_status asx_codec_json_scan_number(const char *cursor, const char **out_next)
{
    const char *scan;
    int has_digits = 0;

    if (cursor == NULL || out_next == NULL) {
        return ASX_E_INVALID_ARGUMENT;
    }

    scan = cursor;
    if (*scan == '-') {
        scan++;
    }
    while (*scan >= '0' && *scan <= '9') {
        has_digits = 1;
        scan++;
    }
    if (!has_digits) {
        return ASX_E_INVALID_ARGUMENT;
    }

    if (*scan == '.') {
        int frac_digits = 0;
        scan++;
        while (*scan >= '0' && *scan <= '9') {
            frac_digits = 1;
            scan++;
        }
        if (!frac_digits) {
            return ASX_E_INVALID_ARGUMENT;
        }
    }

    if (*scan == 'e' || *scan == 'E') {
        int exp_digits = 0;
        scan++;
        if (*scan == '+' || *scan == '-') {
            scan++;
        }
        while (*scan >= '0' && *scan <= '9') {
            exp_digits = 1;
            scan++;
        }
        if (!exp_digits) {
            return ASX_E_INVALID_ARGUMENT;
        }
    }

    *out_next = scan;
    return ASX_OK;
}

static asx_status asx_codec_json_scan_value(const char *cursor, const char **out_next, uint32_t depth);

static asx_status asx_codec_json_scan_array(const char *cursor, const char **out_next, uint32_t depth)
{
    const char *scan;
    asx_status st;

    if (*cursor != '[') {
        return ASX_E_INVALID_ARGUMENT;
    }
    if (depth > 64u) {
        return ASX_E_INVALID_ARGUMENT;
    }

    scan = asx_codec_json_skip_ws(cursor + 1);
    if (*scan == ']') {
        *out_next = scan + 1;
        return ASX_OK;
    }

    for (;;) {
        st = asx_codec_json_scan_value(scan, &scan, depth + 1u);
        if (st != ASX_OK) {
            return st;
        }
        scan = asx_codec_json_skip_ws(scan);
        if (*scan == ',') {
            scan = asx_codec_json_skip_ws(scan + 1);
            continue;
        }
        if (*scan == ']') {
            *out_next = scan + 1;
            return ASX_OK;
        }
        return ASX_E_INVALID_ARGUMENT;
    }
}

static asx_status asx_codec_json_scan_object(const char *cursor, const char **out_next, uint32_t depth)
{
    const char *scan;
    asx_status st;

    if (*cursor != '{') {
        return ASX_E_INVALID_ARGUMENT;
    }
    if (depth > 64u) {
        return ASX_E_INVALID_ARGUMENT;
    }

    scan = asx_codec_json_skip_ws(cursor + 1);
    if (*scan == '}') {
        *out_next = scan + 1;
        return ASX_OK;
    }

    for (;;) {
        st = asx_codec_json_scan_string(scan, &scan);
        if (st != ASX_OK) {
            return st;
        }
        scan = asx_codec_json_skip_ws(scan);
        if (*scan != ':') {
            return ASX_E_INVALID_ARGUMENT;
        }
        scan = asx_codec_json_skip_ws(scan + 1);
        st = asx_codec_json_scan_value(scan, &scan, depth + 1u);
        if (st != ASX_OK) {
            return st;
        }
        scan = asx_codec_json_skip_ws(scan);
        if (*scan == ',') {
            scan = asx_codec_json_skip_ws(scan + 1);
            continue;
        }
        if (*scan == '}') {
            *out_next = scan + 1;
            return ASX_OK;
        }
        return ASX_E_INVALID_ARGUMENT;
    }
}

static asx_status asx_codec_json_scan_value(const char *cursor, const char **out_next, uint32_t depth)
{
    const char *scan;

    if (cursor == NULL || out_next == NULL) {
        return ASX_E_INVALID_ARGUMENT;
    }

    scan = asx_codec_json_skip_ws(cursor);
    switch (*scan) {
    case '"':
        return asx_codec_json_scan_string(scan, out_next);
    case '{':
        return asx_codec_json_scan_object(scan, out_next, depth);
    case '[':
        return asx_codec_json_scan_array(scan, out_next, depth);
    case 't':
        if (strncmp(scan, "true", 4) == 0) {
            *out_next = scan + 4;
            return ASX_OK;
        }
        return ASX_E_INVALID_ARGUMENT;
    case 'f':
        if (strncmp(scan, "false", 5) == 0) {
            *out_next = scan + 5;
            return ASX_OK;
        }
        return ASX_E_INVALID_ARGUMENT;
    case 'n':
        if (strncmp(scan, "null", 4) == 0) {
            *out_next = scan + 4;
            return ASX_OK;
        }
        return ASX_E_INVALID_ARGUMENT;
    default:
        if ((*scan >= '0' && *scan <= '9') || *scan == '-') {
            return asx_codec_json_scan_number(scan, out_next);
        }
        return ASX_E_INVALID_ARGUMENT;
    }
}

static int asx_codec_json_value_matches(const char *json_text, char required_first_char)
{
    const char *cursor;
    const char *next;

    if (json_text == NULL) {
        return 0;
    }

    cursor = asx_codec_json_skip_ws(json_text);
    if (*cursor != required_first_char) {
        return 0;
    }
    if (asx_codec_json_scan_value(cursor, &next, 0u) != ASX_OK) {
        return 0;
    }
    next = asx_codec_json_skip_ws(next);
    return *next == '\0';
}

static char *asx_codec_strdup_range(const char *begin, size_t len)
{
    char *copy;

    copy = (char *)malloc(len + 1u);
    if (copy == NULL) {
        return NULL;
    }
    if (len > 0u) {
        memcpy(copy, begin, len);
    }
    copy[len] = '\0';
    return copy;
}

static int asx_codec_key_equals(const char *lhs, const char *rhs)
{
    return lhs != NULL && rhs != NULL && strcmp(lhs, rhs) == 0;
}

static asx_status asx_codec_json_decode_string(const char *cursor,
                                               const char **out_next,
                                               char **out_text)
{
    const char *scan;
    const char *readp;
    char *decoded;
    size_t max_len;
    size_t write_index;

    if (cursor == NULL || out_next == NULL || out_text == NULL || *cursor != '"') {
        return ASX_E_INVALID_ARGUMENT;
    }

    if (asx_codec_json_scan_string(cursor, &scan) != ASX_OK) {
        return ASX_E_INVALID_ARGUMENT;
    }

    max_len = (size_t)(scan - cursor);
    decoded = (char *)malloc(max_len);
    if (decoded == NULL) {
        return ASX_E_RESOURCE_EXHAUSTED;
    }

    readp = cursor + 1;
    write_index = 0u;
    while (readp < scan - 1) {
        if (*readp == '\\') {
            readp++;
            switch (*readp) {
            case '"':  decoded[write_index++] = '"';  break;
            case '\\': decoded[write_index++] = '\\'; break;
            case '/':  decoded[write_index++] = '/';  break;
            case 'b':  decoded[write_index++] = '\b'; break;
            case 'f':  decoded[write_index++] = '\f'; break;
            case 'n':  decoded[write_index++] = '\n'; break;
            case 'r':  decoded[write_index++] = '\r'; break;
            case 't':  decoded[write_index++] = '\t'; break;
            case 'u':
                decoded[write_index++] = '?';
                readp += 4;
                break;
            default:
                free(decoded);
                return ASX_E_INVALID_ARGUMENT;
            }
            readp++;
            continue;
        }
        decoded[write_index++] = *readp;
        readp++;
    }

    decoded[write_index] = '\0';
    *out_text = decoded;
    *out_next = scan;
    return ASX_OK;
}

static asx_status asx_codec_json_decode_u64(const char *cursor,
                                            const char **out_next,
                                            uint64_t *out_value)
{
    const char *scan;
    uint64_t value;

    if (cursor == NULL || out_next == NULL || out_value == NULL) {
        return ASX_E_INVALID_ARGUMENT;
    }

    scan = cursor;
    if (*scan < '0' || *scan > '9') {
        return ASX_E_INVALID_ARGUMENT;
    }

    value = 0u;
    while (*scan >= '0' && *scan <= '9') {
        uint64_t digit = (uint64_t)(*scan - '0');
        if (value > (UINT64_MAX - digit) / 10u) {
            return ASX_E_INVALID_ARGUMENT;
        }
        value = value * 10u + digit;
        scan++;
    }

    *out_value = value;
    *out_next = scan;
    return ASX_OK;
}

static asx_status asx_codec_capture_raw_json(const char *cursor,
                                             const char **out_next,
                                             char **out_json)
{
    const char *start;
    const char *next;
    char *copy;

    if (cursor == NULL || out_next == NULL || out_json == NULL) {
        return ASX_E_INVALID_ARGUMENT;
    }

    start = asx_codec_json_skip_ws(cursor);
    if (asx_codec_json_scan_value(start, &next, 0u) != ASX_OK) {
        return ASX_E_INVALID_ARGUMENT;
    }

    copy = asx_codec_strdup_range(start, (size_t)(next - start));
    if (copy == NULL) {
        return ASX_E_RESOURCE_EXHAUSTED;
    }
    *out_json = copy;
    *out_next = next;
    return ASX_OK;
}

static asx_status asx_codec_decode_provenance_json(const char *cursor,
                                                   const char **out_next,
                                                   asx_fixture_provenance *prov)
{
    const char *scan;
    uint32_t seen = 0u;

    enum {
        ASX_PROV_RUST_BASELINE = 1u << 0,
        ASX_PROV_TOOLCHAIN_HASH = 1u << 1,
        ASX_PROV_TOOLCHAIN_RELEASE = 1u << 2,
        ASX_PROV_TOOLCHAIN_HOST = 1u << 3,
        ASX_PROV_CARGO_LOCK = 1u << 4,
        ASX_PROV_CAPTURE_RUN = 1u << 5,
        ASX_PROV_ALL = (1u << 6) - 1u
    };

    if (cursor == NULL || out_next == NULL || prov == NULL) {
        return ASX_E_INVALID_ARGUMENT;
    }

    scan = asx_codec_json_skip_ws(cursor);
    if (*scan != '{') {
        return ASX_E_INVALID_ARGUMENT;
    }
    scan = asx_codec_json_skip_ws(scan + 1);
    if (*scan == '}') {
        return ASX_E_INVALID_ARGUMENT;
    }

    for (;;) {
        char *key = NULL;
        char *value = NULL;
        asx_status st;

        st = asx_codec_json_decode_string(scan, &scan, &key);
        if (st != ASX_OK) {
            return st;
        }
        scan = asx_codec_json_skip_ws(scan);
        if (*scan != ':') {
            free(key);
            return ASX_E_INVALID_ARGUMENT;
        }
        scan = asx_codec_json_skip_ws(scan + 1);
        st = asx_codec_json_decode_string(scan, &scan, &value);
        if (st != ASX_OK) {
            free(key);
            return st;
        }

        if (asx_codec_key_equals(key, "rust_baseline_commit")) {
            prov->rust_baseline_commit = value;
            seen |= ASX_PROV_RUST_BASELINE;
        } else if (asx_codec_key_equals(key, "rust_toolchain_commit_hash")) {
            prov->rust_toolchain_commit_hash = value;
            seen |= ASX_PROV_TOOLCHAIN_HASH;
        } else if (asx_codec_key_equals(key, "rust_toolchain_release")) {
            prov->rust_toolchain_release = value;
            seen |= ASX_PROV_TOOLCHAIN_RELEASE;
        } else if (asx_codec_key_equals(key, "rust_toolchain_host")) {
            prov->rust_toolchain_host = value;
            seen |= ASX_PROV_TOOLCHAIN_HOST;
        } else if (asx_codec_key_equals(key, "cargo_lock_sha256")) {
            prov->cargo_lock_sha256 = value;
            seen |= ASX_PROV_CARGO_LOCK;
        } else if (asx_codec_key_equals(key, "capture_run_id")) {
            prov->capture_run_id = value;
            seen |= ASX_PROV_CAPTURE_RUN;
        } else {
            free(key);
            free(value);
            return ASX_E_INVALID_ARGUMENT;
        }

        free(key);
        scan = asx_codec_json_skip_ws(scan);
        if (*scan == ',') {
            scan = asx_codec_json_skip_ws(scan + 1);
            continue;
        }
        if (*scan == '}') {
            scan++;
            break;
        }
        return ASX_E_INVALID_ARGUMENT;
    }

    if (seen != ASX_PROV_ALL) {
        return ASX_E_INVALID_ARGUMENT;
    }
    *out_next = scan;
    return ASX_OK;
}

const char *asx_codec_kind_str(asx_codec_kind codec)
{
    switch (codec) {
    case ASX_CODEC_KIND_JSON:
        return "json";
    case ASX_CODEC_KIND_BIN:
        return "bin";
    default:
        return "unknown";
    }
}

asx_status asx_codec_kind_parse(const char *text, asx_codec_kind *out_codec)
{
    if (text == NULL || out_codec == NULL) {
        return ASX_E_INVALID_ARGUMENT;
    }
    if (strcmp(text, "json") == 0) {
        *out_codec = ASX_CODEC_KIND_JSON;
        return ASX_OK;
    }
    if (strcmp(text, "bin") == 0) {
        *out_codec = ASX_CODEC_KIND_BIN;
        return ASX_OK;
    }
    return ASX_E_INVALID_ARGUMENT;
}

void asx_canonical_fixture_init(asx_canonical_fixture *fixture)
{
    if (fixture == NULL) {
        return;
    }
    memset(fixture, 0, sizeof(*fixture));
    fixture->codec = ASX_CODEC_KIND_JSON;
}

void asx_canonical_fixture_reset(asx_canonical_fixture *fixture)
{
    if (fixture == NULL) {
        return;
    }

    free(fixture->scenario_id);
    free(fixture->fixture_schema_version);
    free(fixture->scenario_dsl_version);
    free(fixture->profile);
    free(fixture->input_json);
    free(fixture->expected_events_json);
    free(fixture->expected_final_snapshot_json);
    free(fixture->expected_error_codes_json);
    free(fixture->semantic_digest);
    free(fixture->provenance.rust_baseline_commit);
    free(fixture->provenance.rust_toolchain_commit_hash);
    free(fixture->provenance.rust_toolchain_release);
    free(fixture->provenance.rust_toolchain_host);
    free(fixture->provenance.cargo_lock_sha256);
    free(fixture->provenance.capture_run_id);

    asx_canonical_fixture_init(fixture);
}

asx_status asx_canonical_fixture_validate(const asx_canonical_fixture *fixture)
{
    const char *digest;
    size_t i;

    if (fixture == NULL) {
        return ASX_E_INVALID_ARGUMENT;
    }
    if (!asx_codec_text_nonempty(fixture->scenario_id) ||
        !asx_codec_text_nonempty(fixture->fixture_schema_version) ||
        !asx_codec_text_nonempty(fixture->scenario_dsl_version) ||
        !asx_codec_text_nonempty(fixture->profile) ||
        !asx_codec_text_nonempty(fixture->input_json) ||
        !asx_codec_text_nonempty(fixture->expected_events_json) ||
        !asx_codec_text_nonempty(fixture->expected_final_snapshot_json) ||
        !asx_codec_text_nonempty(fixture->expected_error_codes_json) ||
        !asx_codec_text_nonempty(fixture->semantic_digest) ||
        !asx_codec_text_nonempty(fixture->provenance.rust_baseline_commit) ||
        !asx_codec_text_nonempty(fixture->provenance.rust_toolchain_commit_hash) ||
        !asx_codec_text_nonempty(fixture->provenance.rust_toolchain_release) ||
        !asx_codec_text_nonempty(fixture->provenance.rust_toolchain_host) ||
        !asx_codec_text_nonempty(fixture->provenance.cargo_lock_sha256) ||
        !asx_codec_text_nonempty(fixture->provenance.capture_run_id)) {
        return ASX_E_INVALID_ARGUMENT;
    }

    if (fixture->codec != ASX_CODEC_KIND_JSON && fixture->codec != ASX_CODEC_KIND_BIN) {
        return ASX_E_INVALID_ARGUMENT;
    }

    if (!asx_codec_json_value_matches(fixture->input_json, '{') ||
        strstr(fixture->input_json, "\"ops\"") == NULL) {
        return ASX_E_INVALID_ARGUMENT;
    }
    if (!asx_codec_json_value_matches(fixture->expected_events_json, '[') ||
        !asx_codec_json_value_matches(fixture->expected_final_snapshot_json, '{') ||
        !asx_codec_json_value_matches(fixture->expected_error_codes_json, '[')) {
        return ASX_E_INVALID_ARGUMENT;
    }

    digest = fixture->semantic_digest;
    if (strncmp(digest, "sha256:", 7) != 0) {
        return ASX_E_INVALID_ARGUMENT;
    }
    digest += 7;
    for (i = 0; i < 64u; i++) {
        if (!asx_codec_is_lower_hex(digest[i])) {
            return ASX_E_INVALID_ARGUMENT;
        }
    }
    if (digest[64] != '\0') {
        return ASX_E_INVALID_ARGUMENT;
    }

    return ASX_OK;
}

void asx_codec_buffer_init(asx_codec_buffer *buf)
{
    if (buf == NULL) {
        return;
    }
    buf->data = NULL;
    buf->len = 0u;
    buf->cap = 0u;
}

void asx_codec_buffer_reset(asx_codec_buffer *buf)
{
    if (buf == NULL) {
        return;
    }
    free(buf->data);
    buf->data = NULL;
    buf->len = 0u;
    buf->cap = 0u;
}

static asx_status asx_codec_buffer_reserve(asx_codec_buffer *buf, size_t additional)
{
    size_t required;
    size_t next_cap;
    char *grown;

    if (buf == NULL) {
        return ASX_E_INVALID_ARGUMENT;
    }
    required = buf->len + additional + 1u;
    if (required <= buf->cap) {
        return ASX_OK;
    }

    next_cap = (buf->cap == 0u) ? 128u : buf->cap;
    while (next_cap < required) {
        if (next_cap > ((size_t)-1) / 2u) {
            return ASX_E_RESOURCE_EXHAUSTED;
        }
        next_cap *= 2u;
    }

    grown = (char *)realloc(buf->data, next_cap);
    if (grown == NULL) {
        return ASX_E_RESOURCE_EXHAUSTED;
    }

    buf->data = grown;
    buf->cap = next_cap;
    if (buf->len == 0u) {
        buf->data[0] = '\0';
    }
    return ASX_OK;
}

asx_status asx_codec_buffer_append_bytes(asx_codec_buffer *buf,
                                                const char *bytes,
                                                size_t len)
{
    asx_status st;

    if (buf == NULL || bytes == NULL) {
        return ASX_E_INVALID_ARGUMENT;
    }
    st = asx_codec_buffer_reserve(buf, len);
    if (st != ASX_OK) {
        return st;
    }
    memcpy(buf->data + buf->len, bytes, len);
    buf->len += len;
    buf->data[buf->len] = '\0';
    return ASX_OK;
}

asx_status asx_codec_buffer_append_cstr(asx_codec_buffer *buf, const char *text)
{
    if (text == NULL) {
        return ASX_E_INVALID_ARGUMENT;
    }
    return asx_codec_buffer_append_bytes(buf, text, strlen(text));
}

asx_status asx_codec_buffer_append_char(asx_codec_buffer *buf, char ch)
{
    return asx_codec_buffer_append_bytes(buf, &ch, 1u);
}

asx_status asx_codec_buffer_append_u64(asx_codec_buffer *buf, uint64_t value)
{
    char tmp[32];
    int n;

    n = snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)value);
    if (n < 0) {
        return ASX_E_INVALID_STATE;
    }
    return asx_codec_buffer_append_bytes(buf, tmp, (size_t)n);
}

asx_status asx_codec_buffer_append_json_string(asx_codec_buffer *buf, const char *text)
{
    const unsigned char *p;
    asx_status st;

    if (text == NULL) {
        return ASX_E_INVALID_ARGUMENT;
    }

    st = asx_codec_buffer_append_char(buf, '"');
    if (st != ASX_OK) {
        return st;
    }

    p = (const unsigned char *)text;
    while (*p != '\0') {
        switch (*p) {
        case '"':
            st = asx_codec_buffer_append_cstr(buf, "\\\"");
            break;
        case '\\':
            st = asx_codec_buffer_append_cstr(buf, "\\\\");
            break;
        case '\b':
            st = asx_codec_buffer_append_cstr(buf, "\\b");
            break;
        case '\f':
            st = asx_codec_buffer_append_cstr(buf, "\\f");
            break;
        case '\n':
            st = asx_codec_buffer_append_cstr(buf, "\\n");
            break;
        case '\r':
            st = asx_codec_buffer_append_cstr(buf, "\\r");
            break;
        case '\t':
            st = asx_codec_buffer_append_cstr(buf, "\\t");
            break;
        default:
            if (*p < 0x20u) {
                char esc[7];
                int n = snprintf(esc, sizeof(esc), "\\u%04x", (unsigned int)*p);
                if (n < 0) {
                    return ASX_E_INVALID_STATE;
                }
                st = asx_codec_buffer_append_bytes(buf, esc, (size_t)n);
            } else {
                st = asx_codec_buffer_append_bytes(buf, (const char *)p, 1u);
            }
            break;
        }
        if (st != ASX_OK) {
            return st;
        }
        p++;
    }

    return asx_codec_buffer_append_char(buf, '"');
}

asx_status asx_codec_buffer_append_field_prefix(asx_codec_buffer *buf, int *is_first)
{
    asx_status st;

    if (*is_first) {
        *is_first = 0;
        return ASX_OK;
    }
    st = asx_codec_buffer_append_char(buf, ',');
    return st;
}

asx_status asx_codec_buffer_append_string_field(asx_codec_buffer *buf,
                                                       int *is_first,
                                                       const char *key,
                                                       const char *value)
{
    asx_status st;

    st = asx_codec_buffer_append_field_prefix(buf, is_first);
    if (st != ASX_OK) {
        return st;
    }
    st = asx_codec_buffer_append_json_string(buf, key);
    if (st != ASX_OK) {
        return st;
    }
    st = asx_codec_buffer_append_char(buf, ':');
    if (st != ASX_OK) {
        return st;
    }
    return asx_codec_buffer_append_json_string(buf, value);
}

asx_status asx_codec_buffer_append_u64_field(asx_codec_buffer *buf,
                                                    int *is_first,
                                                    const char *key,
                                                    uint64_t value)
{
    asx_status st;

    st = asx_codec_buffer_append_field_prefix(buf, is_first);
    if (st != ASX_OK) {
        return st;
    }
    st = asx_codec_buffer_append_json_string(buf, key);
    if (st != ASX_OK) {
        return st;
    }
    st = asx_codec_buffer_append_char(buf, ':');
    if (st != ASX_OK) {
        return st;
    }
    return asx_codec_buffer_append_u64(buf, value);
}

static asx_status asx_codec_buffer_append_raw_json_field(asx_codec_buffer *buf,
                                                         int *is_first,
                                                         const char *key,
                                                         const char *raw_json,
                                                         char expected_prefix)
{
    const char *start;
    const char *end;
    asx_status st;

    if (raw_json == NULL) {
        return ASX_E_INVALID_ARGUMENT;
    }

    start = asx_codec_json_skip_ws(raw_json);
    if (*start != expected_prefix) {
        return ASX_E_INVALID_ARGUMENT;
    }
    st = asx_codec_json_scan_value(start, &end, 0u);
    if (st != ASX_OK) {
        return st;
    }
    if (*asx_codec_json_skip_ws(end) != '\0') {
        return ASX_E_INVALID_ARGUMENT;
    }

    st = asx_codec_buffer_append_field_prefix(buf, is_first);
    if (st != ASX_OK) {
        return st;
    }
    st = asx_codec_buffer_append_json_string(buf, key);
    if (st != ASX_OK) {
        return st;
    }
    st = asx_codec_buffer_append_char(buf, ':');
    if (st != ASX_OK) {
        return st;
    }
    return asx_codec_buffer_append_bytes(buf, start, (size_t)(end - start));
}

static asx_status asx_codec_buffer_append_provenance(asx_codec_buffer *buf,
                                                     int *is_first,
                                                     const asx_fixture_provenance *prov)
{
    asx_status st;
    int inner_first = 1;

    st = asx_codec_buffer_append_field_prefix(buf, is_first);
    if (st != ASX_OK) {
        return st;
    }
    st = asx_codec_buffer_append_json_string(buf, "provenance");
    if (st != ASX_OK) {
        return st;
    }
    st = asx_codec_buffer_append_char(buf, ':');
    if (st != ASX_OK) {
        return st;
    }
    st = asx_codec_buffer_append_char(buf, '{');
    if (st != ASX_OK) {
        return st;
    }

    st = asx_codec_buffer_append_string_field(buf, &inner_first, "cargo_lock_sha256",
                                              prov->cargo_lock_sha256);
    if (st != ASX_OK) {
        return st;
    }
    st = asx_codec_buffer_append_string_field(buf, &inner_first, "capture_run_id",
                                              prov->capture_run_id);
    if (st != ASX_OK) {
        return st;
    }
    st = asx_codec_buffer_append_string_field(buf, &inner_first, "rust_baseline_commit",
                                              prov->rust_baseline_commit);
    if (st != ASX_OK) {
        return st;
    }
    st = asx_codec_buffer_append_string_field(buf, &inner_first, "rust_toolchain_commit_hash",
                                              prov->rust_toolchain_commit_hash);
    if (st != ASX_OK) {
        return st;
    }
    st = asx_codec_buffer_append_string_field(buf, &inner_first, "rust_toolchain_host",
                                              prov->rust_toolchain_host);
    if (st != ASX_OK) {
        return st;
    }
    st = asx_codec_buffer_append_string_field(buf, &inner_first, "rust_toolchain_release",
                                              prov->rust_toolchain_release);
    if (st != ASX_OK) {
        return st;
    }

    return asx_codec_buffer_append_char(buf, '}');
}

asx_status asx_codec_encode_fixture_json(const asx_canonical_fixture *fixture,
                                         asx_codec_buffer *out_json)
{
    asx_status st;
    int is_first = 1;

    if (fixture == NULL || out_json == NULL) {
        return ASX_E_INVALID_ARGUMENT;
    }

    st = asx_canonical_fixture_validate(fixture);
    if (st != ASX_OK) {
        return st;
    }

    out_json->len = 0u;
    if (out_json->data != NULL) {
        out_json->data[0] = '\0';
    }

    st = asx_codec_buffer_append_char(out_json, '{');
    if (st != ASX_OK) {
        return st;
    }

    st = asx_codec_buffer_append_string_field(out_json, &is_first, "codec",
                                              asx_codec_kind_str(fixture->codec));
    if (st != ASX_OK) {
        return st;
    }
    st = asx_codec_buffer_append_raw_json_field(out_json, &is_first, "expected_error_codes",
                                                fixture->expected_error_codes_json, '[');
    if (st != ASX_OK) {
        return st;
    }
    st = asx_codec_buffer_append_raw_json_field(out_json, &is_first, "expected_events",
                                                fixture->expected_events_json, '[');
    if (st != ASX_OK) {
        return st;
    }
    st = asx_codec_buffer_append_raw_json_field(out_json, &is_first, "expected_final_snapshot",
                                                fixture->expected_final_snapshot_json, '{');
    if (st != ASX_OK) {
        return st;
    }
    st = asx_codec_buffer_append_string_field(out_json, &is_first, "fixture_schema_version",
                                              fixture->fixture_schema_version);
    if (st != ASX_OK) {
        return st;
    }
    st = asx_codec_buffer_append_raw_json_field(out_json, &is_first, "input",
                                                fixture->input_json, '{');
    if (st != ASX_OK) {
        return st;
    }
    st = asx_codec_buffer_append_string_field(out_json, &is_first, "profile",
                                              fixture->profile);
    if (st != ASX_OK) {
        return st;
    }
    st = asx_codec_buffer_append_provenance(out_json, &is_first, &fixture->provenance);
    if (st != ASX_OK) {
        return st;
    }
    st = asx_codec_buffer_append_string_field(out_json, &is_first, "scenario_dsl_version",
                                              fixture->scenario_dsl_version);
    if (st != ASX_OK) {
        return st;
    }
    st = asx_codec_buffer_append_string_field(out_json, &is_first, "scenario_id",
                                              fixture->scenario_id);
    if (st != ASX_OK) {
        return st;
    }
    st = asx_codec_buffer_append_u64_field(out_json, &is_first, "seed", fixture->seed);
    if (st != ASX_OK) {
        return st;
    }
    st = asx_codec_buffer_append_string_field(out_json, &is_first, "semantic_digest",
                                              fixture->semantic_digest);
    if (st != ASX_OK) {
        return st;
    }

    st = asx_codec_buffer_append_char(out_json, '}');
    if (st != ASX_OK) {
        return st;
    }

    return ASX_OK;
}

asx_status asx_codec_decode_fixture_json(const char *json, asx_canonical_fixture *out_fixture)
{
    const char *scan;
    asx_canonical_fixture parsed;
    uint32_t seen = 0u;

    enum {
        ASX_F_CODEC = 1u << 0,
        ASX_F_EXPECTED_ERROR_CODES = 1u << 1,
        ASX_F_EXPECTED_EVENTS = 1u << 2,
        ASX_F_EXPECTED_FINAL_SNAPSHOT = 1u << 3,
        ASX_F_FIXTURE_SCHEMA_VERSION = 1u << 4,
        ASX_F_INPUT = 1u << 5,
        ASX_F_PROFILE = 1u << 6,
        ASX_F_PROVENANCE = 1u << 7,
        ASX_F_SCENARIO_DSL_VERSION = 1u << 8,
        ASX_F_SCENARIO_ID = 1u << 9,
        ASX_F_SEED = 1u << 10,
        ASX_F_SEMANTIC_DIGEST = 1u << 11,
        ASX_F_REQUIRED = (1u << 12) - 1u
    };

    if (json == NULL || out_fixture == NULL) {
        return ASX_E_INVALID_ARGUMENT;
    }

    asx_canonical_fixture_init(&parsed);

    scan = asx_codec_json_skip_ws(json);
    if (*scan != '{') {
        return ASX_E_INVALID_ARGUMENT;
    }
    scan = asx_codec_json_skip_ws(scan + 1);
    if (*scan == '}') {
        return ASX_E_INVALID_ARGUMENT;
    }

    for (;;) {
        char *key = NULL;
        asx_status st;

        st = asx_codec_json_decode_string(scan, &scan, &key);
        if (st != ASX_OK) {
            asx_canonical_fixture_reset(&parsed);
            return st;
        }
        scan = asx_codec_json_skip_ws(scan);
        if (*scan != ':') {
            free(key);
            asx_canonical_fixture_reset(&parsed);
            return ASX_E_INVALID_ARGUMENT;
        }
        scan = asx_codec_json_skip_ws(scan + 1);

        if (asx_codec_key_equals(key, "codec")) {
            char *codec_text = NULL;
            st = asx_codec_json_decode_string(scan, &scan, &codec_text);
            if (st == ASX_OK) {
                st = asx_codec_kind_parse(codec_text, &parsed.codec);
            }
            free(codec_text);
            seen |= ASX_F_CODEC;
        } else if (asx_codec_key_equals(key, "expected_error_codes")) {
            st = asx_codec_capture_raw_json(scan, &scan, &parsed.expected_error_codes_json);
            seen |= ASX_F_EXPECTED_ERROR_CODES;
        } else if (asx_codec_key_equals(key, "expected_events")) {
            st = asx_codec_capture_raw_json(scan, &scan, &parsed.expected_events_json);
            seen |= ASX_F_EXPECTED_EVENTS;
        } else if (asx_codec_key_equals(key, "expected_final_snapshot")) {
            st = asx_codec_capture_raw_json(scan, &scan, &parsed.expected_final_snapshot_json);
            seen |= ASX_F_EXPECTED_FINAL_SNAPSHOT;
        } else if (asx_codec_key_equals(key, "fixture_schema_version")) {
            st = asx_codec_json_decode_string(scan, &scan, &parsed.fixture_schema_version);
            seen |= ASX_F_FIXTURE_SCHEMA_VERSION;
        } else if (asx_codec_key_equals(key, "input")) {
            st = asx_codec_capture_raw_json(scan, &scan, &parsed.input_json);
            seen |= ASX_F_INPUT;
        } else if (asx_codec_key_equals(key, "profile")) {
            st = asx_codec_json_decode_string(scan, &scan, &parsed.profile);
            seen |= ASX_F_PROFILE;
        } else if (asx_codec_key_equals(key, "provenance")) {
            st = asx_codec_decode_provenance_json(scan, &scan, &parsed.provenance);
            seen |= ASX_F_PROVENANCE;
        } else if (asx_codec_key_equals(key, "scenario_dsl_version")) {
            st = asx_codec_json_decode_string(scan, &scan, &parsed.scenario_dsl_version);
            seen |= ASX_F_SCENARIO_DSL_VERSION;
        } else if (asx_codec_key_equals(key, "scenario_id")) {
            st = asx_codec_json_decode_string(scan, &scan, &parsed.scenario_id);
            seen |= ASX_F_SCENARIO_ID;
        } else if (asx_codec_key_equals(key, "seed")) {
            st = asx_codec_json_decode_u64(scan, &scan, &parsed.seed);
            seen |= ASX_F_SEED;
        } else if (asx_codec_key_equals(key, "semantic_digest")) {
            st = asx_codec_json_decode_string(scan, &scan, &parsed.semantic_digest);
            seen |= ASX_F_SEMANTIC_DIGEST;
        } else {
            st = ASX_E_INVALID_ARGUMENT;
        }

        free(key);
        if (st != ASX_OK) {
            asx_canonical_fixture_reset(&parsed);
            return st;
        }

        scan = asx_codec_json_skip_ws(scan);
        if (*scan == ',') {
            scan = asx_codec_json_skip_ws(scan + 1);
            continue;
        }
        if (*scan == '}') {
            scan++;
            break;
        }

        asx_canonical_fixture_reset(&parsed);
        return ASX_E_INVALID_ARGUMENT;
    }

    scan = asx_codec_json_skip_ws(scan);
    if (*scan != '\0' || seen != ASX_F_REQUIRED) {
        asx_canonical_fixture_reset(&parsed);
        return ASX_E_INVALID_ARGUMENT;
    }

    if (asx_canonical_fixture_validate(&parsed) != ASX_OK) {
        asx_canonical_fixture_reset(&parsed);
        return ASX_E_INVALID_ARGUMENT;
    }

    *out_fixture = parsed;
    return ASX_OK;
}

asx_status asx_codec_fixture_replay_key(const asx_canonical_fixture *fixture,
                                        asx_codec_buffer *out_key)
{
    asx_status st;
    int is_first = 1;

    if (fixture == NULL || out_key == NULL) {
        return ASX_E_INVALID_ARGUMENT;
    }

    st = asx_canonical_fixture_validate(fixture);
    if (st != ASX_OK) {
        return st;
    }

    out_key->len = 0u;
    if (out_key->data != NULL) {
        out_key->data[0] = '\0';
    }

    st = asx_codec_buffer_append_char(out_key, '{');
    if (st != ASX_OK) {
        return st;
    }
    st = asx_codec_buffer_append_string_field(out_key, &is_first, "codec",
                                              asx_codec_kind_str(fixture->codec));
    if (st != ASX_OK) {
        return st;
    }
    st = asx_codec_buffer_append_string_field(out_key, &is_first, "profile", fixture->profile);
    if (st != ASX_OK) {
        return st;
    }
    st = asx_codec_buffer_append_string_field(out_key, &is_first, "scenario_id",
                                              fixture->scenario_id);
    if (st != ASX_OK) {
        return st;
    }
    st = asx_codec_buffer_append_u64_field(out_key, &is_first, "seed", fixture->seed);
    if (st != ASX_OK) {
        return st;
    }
    st = asx_codec_buffer_append_string_field(out_key, &is_first, "semantic_digest",
                                              fixture->semantic_digest);
    if (st != ASX_OK) {
        return st;
    }
    return asx_codec_buffer_append_char(out_key, '}');
}

static asx_status asx_codec_decode_fixture_json_payload(const void *payload,
                                                        size_t payload_len,
                                                        asx_canonical_fixture *out_fixture)
{
    char *copy;
    asx_status st;

    if (payload == NULL || out_fixture == NULL || payload_len == 0u) {
        return ASX_E_INVALID_ARGUMENT;
    }
    if (payload_len + 1u < payload_len) {
        return ASX_E_RESOURCE_EXHAUSTED;
    }

    copy = (char *)malloc(payload_len + 1u);
    if (copy == NULL) {
        return ASX_E_RESOURCE_EXHAUSTED;
    }
    memcpy(copy, payload, payload_len);
    copy[payload_len] = '\0';

    st = asx_codec_decode_fixture_json(copy, out_fixture);
    free(copy);
    return st;
}

enum {
    ASX_CODEC_BIN_WIRE_VARINT = 0u,
    ASX_CODEC_BIN_WIRE_BYTES  = 2u,
    ASX_CODEC_BIN_WIRE_MASK   = 0x3u
};

enum {
    ASX_CODEC_BIN_TAG_SCENARIO_ID                 = 1u,
    ASX_CODEC_BIN_TAG_FIXTURE_SCHEMA_VERSION      = 2u,
    ASX_CODEC_BIN_TAG_SCENARIO_DSL_VERSION        = 3u,
    ASX_CODEC_BIN_TAG_PROFILE                     = 4u,
    ASX_CODEC_BIN_TAG_CODEC                       = 5u,
    ASX_CODEC_BIN_TAG_SEED                        = 6u,
    ASX_CODEC_BIN_TAG_INPUT_JSON                  = 7u,
    ASX_CODEC_BIN_TAG_EXPECTED_EVENTS_JSON        = 8u,
    ASX_CODEC_BIN_TAG_EXPECTED_FINAL_SNAPSHOT_JSON = 9u,
    ASX_CODEC_BIN_TAG_EXPECTED_ERROR_CODES_JSON   = 10u,
    ASX_CODEC_BIN_TAG_SEMANTIC_DIGEST             = 11u,
    ASX_CODEC_BIN_TAG_RUST_BASELINE_COMMIT        = 12u,
    ASX_CODEC_BIN_TAG_RUST_TOOLCHAIN_COMMIT_HASH  = 13u,
    ASX_CODEC_BIN_TAG_RUST_TOOLCHAIN_RELEASE      = 14u,
    ASX_CODEC_BIN_TAG_RUST_TOOLCHAIN_HOST         = 15u,
    ASX_CODEC_BIN_TAG_CARGO_LOCK_SHA256           = 16u,
    ASX_CODEC_BIN_TAG_CAPTURE_RUN_ID              = 17u
};

enum {
    ASX_CODEC_BIN_HEADER_SIZE = 11u,
    ASX_CODEC_BIN_CHECKSUM_SIZE = 4u
};

static const unsigned char g_asx_codec_bin_magic[4] = {'A', 'S', 'X', 'B'};

typedef struct {
    const unsigned char *payload;
    size_t payload_len;
    uint8_t frame_schema_version;
    uint8_t message_type;
    uint8_t flags;
} asx_codec_bin_frame_view;

static int asx_codec_slice_nonempty(asx_codec_slice slice)
{
    return slice.ptr != NULL && slice.len > 0u;
}

static int asx_codec_slice_json_prefix(asx_codec_slice slice, char expected_first_char)
{
    size_t i;
    const unsigned char *bytes;

    if (!asx_codec_slice_nonempty(slice)) {
        return 0;
    }

    bytes = (const unsigned char *)slice.ptr;
    i = 0u;
    while (i < slice.len) {
        if (bytes[i] != ' ' && bytes[i] != '\t' && bytes[i] != '\r' && bytes[i] != '\n') {
            return bytes[i] == (unsigned char)expected_first_char;
        }
        i++;
    }
    return 0;
}

static int asx_codec_slice_contains(asx_codec_slice haystack, const char *needle)
{
    size_t i;
    size_t needle_len;

    if (!asx_codec_slice_nonempty(haystack) || needle == NULL) {
        return 0;
    }
    needle_len = strlen(needle);
    if (needle_len == 0u || haystack.len < needle_len) {
        return 0;
    }

    for (i = 0u; i + needle_len <= haystack.len; i++) {
        if (memcmp(haystack.ptr + i, needle, needle_len) == 0) {
            return 1;
        }
    }
    return 0;
}

static int asx_codec_slice_matches(asx_codec_slice slice, const char *expected)
{
    size_t expected_len;

    if (!asx_codec_slice_nonempty(slice) || expected == NULL) {
        return 0;
    }
    expected_len = strlen(expected);
    if (slice.len != expected_len) {
        return 0;
    }
    return memcmp(slice.ptr, expected, expected_len) == 0;
}

static int asx_codec_slice_is_sha256_digest(asx_codec_slice digest)
{
    size_t i;

    if (!asx_codec_slice_nonempty(digest) || digest.len != 71u) {
        return 0;
    }
    if (memcmp(digest.ptr, "sha256:", 7u) != 0) {
        return 0;
    }
    for (i = 7u; i < digest.len; i++) {
        if (!asx_codec_is_lower_hex(digest.ptr[i])) {
            return 0;
        }
    }
    return 1;
}

static uint32_t asx_codec_bin_checksum32(const unsigned char *bytes, size_t len)
{
    uint32_t hash;
    size_t i;

    hash = 2166136261u;
    for (i = 0u; i < len; i++) {
        hash ^= (uint32_t)bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

/* Big-endian helpers delegated to asx/portable.h (P-WRAP-003) */
#define asx_codec_bin_store_u32_be(out, v) asx_store_be_u32((uint8_t *)(out), (v))
#define asx_codec_bin_load_u32_be(in) asx_load_be_u32((const uint8_t *)(in))

static asx_status asx_codec_bin_append_u8(asx_codec_buffer *buf, uint8_t value)
{
    char byte = (char)value;
    return asx_codec_buffer_append_bytes(buf, &byte, 1u);
}

static asx_status asx_codec_bin_append_u32_be(asx_codec_buffer *buf, uint32_t value)
{
    char bytes[4];
    bytes[0] = (char)((value >> 24) & 0xffu);
    bytes[1] = (char)((value >> 16) & 0xffu);
    bytes[2] = (char)((value >> 8) & 0xffu);
    bytes[3] = (char)(value & 0xffu);
    return asx_codec_buffer_append_bytes(buf, bytes, 4u);
}

static asx_status asx_codec_bin_decode_varint(const unsigned char **cursor,
                                              const unsigned char *end,
                                              uint64_t *out_value)
{
    uint64_t value;
    uint32_t i;
    const unsigned char *scan;

    if (cursor == NULL || *cursor == NULL || end == NULL || out_value == NULL || *cursor > end) {
        return ASX_E_INVALID_ARGUMENT;
    }

    value = 0u;
    scan = *cursor;
    for (i = 0u; i < 10u; i++) {
        unsigned char byte;

        if (scan >= end) {
            return ASX_E_INVALID_ARGUMENT;
        }
        byte = *scan++;
        if (i == 9u && (byte & 0xfeu) != 0u) {
            return ASX_E_INVALID_ARGUMENT;
        }
        value |= ((uint64_t)(byte & 0x7fu)) << (7u * i);
        if ((byte & 0x80u) == 0u) {
            *cursor = scan;
            *out_value = value;
            return ASX_OK;
        }
    }
    return ASX_E_INVALID_ARGUMENT;
}

static asx_status asx_codec_bin_append_varint(asx_codec_buffer *buf, uint64_t value)
{
    char bytes[10];
    size_t n;

    if (buf == NULL) {
        return ASX_E_INVALID_ARGUMENT;
    }

    n = 0u;
    do {
        uint8_t byte = (uint8_t)(value & 0x7fu);
        value >>= 7u;
        if (value != 0u) {
            byte |= 0x80u;
        }
        bytes[n++] = (char)byte;
    } while (value != 0u);

    return asx_codec_buffer_append_bytes(buf, bytes, n);
}

static asx_status asx_codec_bin_append_key(asx_codec_buffer *buf, uint32_t tag, uint8_t wire)
{
    uint64_t key;
    key = ((uint64_t)tag << 2u) | (uint64_t)(wire & ASX_CODEC_BIN_WIRE_MASK);
    return asx_codec_bin_append_varint(buf, key);
}

static asx_status asx_codec_bin_append_string_field(asx_codec_buffer *buf,
                                                    uint32_t tag,
                                                    const char *value)
{
    asx_status st;
    size_t len;

    if (buf == NULL || value == NULL) {
        return ASX_E_INVALID_ARGUMENT;
    }
    len = strlen(value);

    st = asx_codec_bin_append_key(buf, tag, ASX_CODEC_BIN_WIRE_BYTES);
    if (st != ASX_OK) {
        return st;
    }
    st = asx_codec_bin_append_varint(buf, (uint64_t)len);
    if (st != ASX_OK) {
        return st;
    }
    return asx_codec_buffer_append_bytes(buf, value, len);
}

static asx_status asx_codec_bin_append_varint_field(asx_codec_buffer *buf,
                                                    uint32_t tag,
                                                    uint64_t value)
{
    asx_status st;

    st = asx_codec_bin_append_key(buf, tag, ASX_CODEC_BIN_WIRE_VARINT);
    if (st != ASX_OK) {
        return st;
    }
    return asx_codec_bin_append_varint(buf, value);
}

static asx_status asx_codec_bin_parse_frame(const void *frame_payload,
                                            size_t frame_len,
                                            asx_codec_bin_frame_view *out_frame)
{
    const unsigned char *bytes;
    uint32_t payload_len32;
    size_t expected_len;

    if (frame_payload == NULL || frame_len < ASX_CODEC_BIN_HEADER_SIZE || out_frame == NULL) {
        return ASX_E_INVALID_ARGUMENT;
    }

    bytes = (const unsigned char *)frame_payload;
    if (memcmp(bytes, g_asx_codec_bin_magic, 4u) != 0) {
        return ASX_E_INVALID_ARGUMENT;
    }

    out_frame->frame_schema_version = bytes[4];
    out_frame->message_type = bytes[5];
    out_frame->flags = bytes[6];
    if (out_frame->frame_schema_version != ASX_CODEC_BIN_FRAME_SCHEMA_VERSION_V1 ||
        out_frame->message_type != ASX_CODEC_BIN_FRAME_MESSAGE_FIXTURE) {
        return ASX_E_INVALID_ARGUMENT;
    }
    if ((out_frame->flags & (uint8_t)(~ASX_CODEC_BIN_FLAG_CHECKSUM_FOOTER)) != 0u) {
        return ASX_E_INVALID_ARGUMENT;
    }

    payload_len32 = asx_codec_bin_load_u32_be(bytes + 7);
    /* Guard: only meaningful on 32-bit targets where size_t could overflow */
#if SIZE_MAX <= UINT32_MAX
    if ((size_t)payload_len32 > ((size_t)-1) - ASX_CODEC_BIN_HEADER_SIZE) {
        return ASX_E_INVALID_ARGUMENT;
    }
#endif
    expected_len = ASX_CODEC_BIN_HEADER_SIZE + (size_t)payload_len32;
    if ((out_frame->flags & ASX_CODEC_BIN_FLAG_CHECKSUM_FOOTER) != 0u) {
        if (expected_len > ((size_t)-1) - ASX_CODEC_BIN_CHECKSUM_SIZE) {
            return ASX_E_INVALID_ARGUMENT;
        }
        expected_len += ASX_CODEC_BIN_CHECKSUM_SIZE;
    }
    if (expected_len != frame_len) {
        return ASX_E_INVALID_ARGUMENT;
    }

    if ((out_frame->flags & ASX_CODEC_BIN_FLAG_CHECKSUM_FOOTER) != 0u) {
        uint32_t expected_checksum;
        uint32_t observed_checksum;
        expected_checksum = asx_codec_bin_load_u32_be(bytes + expected_len - ASX_CODEC_BIN_CHECKSUM_SIZE);
        observed_checksum = asx_codec_bin_checksum32(bytes, expected_len - ASX_CODEC_BIN_CHECKSUM_SIZE);
        if (expected_checksum != observed_checksum) {
            return ASX_E_INVALID_ARGUMENT;
        }
    }

    out_frame->payload = bytes + ASX_CODEC_BIN_HEADER_SIZE;
    out_frame->payload_len = (size_t)payload_len32;
    return ASX_OK;
}

void asx_codec_bin_fixture_view_init(asx_codec_bin_fixture_view *view)
{
    if (view == NULL) {
        return;
    }
    memset(view, 0, sizeof(*view));
}

static asx_status asx_codec_bin_validate_view(const asx_codec_bin_fixture_view *view)
{
    if (view == NULL) {
        return ASX_E_INVALID_ARGUMENT;
    }
    if (view->frame_schema_version != ASX_CODEC_BIN_FRAME_SCHEMA_VERSION_V1 ||
        view->message_type != ASX_CODEC_BIN_FRAME_MESSAGE_FIXTURE) {
        return ASX_E_INVALID_ARGUMENT;
    }
    if (view->codec != ASX_CODEC_KIND_JSON && view->codec != ASX_CODEC_KIND_BIN) {
        return ASX_E_INVALID_ARGUMENT;
    }
    if (!asx_codec_slice_nonempty(view->scenario_id) ||
        !asx_codec_slice_nonempty(view->fixture_schema_version) ||
        !asx_codec_slice_nonempty(view->scenario_dsl_version) ||
        !asx_codec_slice_nonempty(view->profile) ||
        !asx_codec_slice_nonempty(view->rust_baseline_commit) ||
        !asx_codec_slice_nonempty(view->rust_toolchain_commit_hash) ||
        !asx_codec_slice_nonempty(view->rust_toolchain_release) ||
        !asx_codec_slice_nonempty(view->rust_toolchain_host) ||
        !asx_codec_slice_nonempty(view->cargo_lock_sha256) ||
        !asx_codec_slice_nonempty(view->capture_run_id)) {
        return ASX_E_INVALID_ARGUMENT;
    }
    if (!asx_codec_slice_json_prefix(view->input_json, '{') ||
        !asx_codec_slice_contains(view->input_json, "\"ops\"")) {
        return ASX_E_INVALID_ARGUMENT;
    }
    if (!asx_codec_slice_json_prefix(view->expected_events_json, '[') ||
        !asx_codec_slice_json_prefix(view->expected_final_snapshot_json, '{') ||
        !asx_codec_slice_json_prefix(view->expected_error_codes_json, '[')) {
        return ASX_E_INVALID_ARGUMENT;
    }
    if (!asx_codec_slice_is_sha256_digest(view->semantic_digest)) {
        return ASX_E_INVALID_ARGUMENT;
    }
    return ASX_OK;
}

asx_status asx_codec_decode_fixture_bin_view(const void *payload,
                                             size_t payload_len,
                                             asx_codec_bin_fixture_view *out_view)
{
    asx_codec_bin_frame_view frame;
    asx_codec_bin_fixture_view view;
    const unsigned char *cursor;
    const unsigned char *end;
    uint32_t seen;
    asx_status st;

    enum {
        ASX_BIN_SEEN_REQUIRED = (1u << 17) - 1u
    };

    if (out_view == NULL) {
        return ASX_E_INVALID_ARGUMENT;
    }

    st = asx_codec_bin_parse_frame(payload, payload_len, &frame);
    if (st != ASX_OK) {
        return st;
    }

    asx_codec_bin_fixture_view_init(&view);
    view.frame_schema_version = frame.frame_schema_version;
    view.message_type = frame.message_type;
    view.flags = frame.flags;

    cursor = frame.payload;
    end = frame.payload + frame.payload_len;
    seen = 0u;

    while (cursor < end) {
        uint64_t key;
        uint32_t tag;
        uint8_t wire;
        uint32_t bit = 0u;

        st = asx_codec_bin_decode_varint(&cursor, end, &key);
        if (st != ASX_OK) {
            return st;
        }

        tag = (uint32_t)(key >> 2u);
        wire = (uint8_t)(key & ASX_CODEC_BIN_WIRE_MASK);
        if (tag == 0u || tag > 63u) {
            return ASX_E_INVALID_ARGUMENT;
        }
        if (tag <= 17u) {
            bit = 1u << (tag - 1u);
        }

        if (wire == ASX_CODEC_BIN_WIRE_VARINT) {
            uint64_t value;

            st = asx_codec_bin_decode_varint(&cursor, end, &value);
            if (st != ASX_OK) {
                return st;
            }

            if (tag == ASX_CODEC_BIN_TAG_CODEC) {
                if ((seen & bit) != 0u || value > (uint64_t)ASX_CODEC_KIND_BIN) {
                    return ASX_E_INVALID_ARGUMENT;
                }
                view.codec = (asx_codec_kind)value;
                seen |= bit;
            } else if (tag == ASX_CODEC_BIN_TAG_SEED) {
                if ((seen & bit) != 0u) {
                    return ASX_E_INVALID_ARGUMENT;
                }
                view.seed = value;
                seen |= bit;
            }
            continue;
        }

        if (wire == ASX_CODEC_BIN_WIRE_BYTES) {
            uint64_t raw_len;
            size_t field_len;
            asx_codec_slice slice;

            st = asx_codec_bin_decode_varint(&cursor, end, &raw_len);
            if (st != ASX_OK || raw_len > (uint64_t)(size_t)(end - cursor)) {
                return ASX_E_INVALID_ARGUMENT;
            }
            field_len = (size_t)raw_len;
            slice.ptr = (const char *)cursor;
            slice.len = field_len;
            cursor += field_len;

            if (tag == ASX_CODEC_BIN_TAG_SCENARIO_ID) {
                if ((seen & bit) != 0u) return ASX_E_INVALID_ARGUMENT;
                view.scenario_id = slice;
                seen |= bit;
            } else if (tag == ASX_CODEC_BIN_TAG_FIXTURE_SCHEMA_VERSION) {
                if ((seen & bit) != 0u) return ASX_E_INVALID_ARGUMENT;
                view.fixture_schema_version = slice;
                seen |= bit;
            } else if (tag == ASX_CODEC_BIN_TAG_SCENARIO_DSL_VERSION) {
                if ((seen & bit) != 0u) return ASX_E_INVALID_ARGUMENT;
                view.scenario_dsl_version = slice;
                seen |= bit;
            } else if (tag == ASX_CODEC_BIN_TAG_PROFILE) {
                if ((seen & bit) != 0u) return ASX_E_INVALID_ARGUMENT;
                view.profile = slice;
                seen |= bit;
            } else if (tag == ASX_CODEC_BIN_TAG_INPUT_JSON) {
                if ((seen & bit) != 0u) return ASX_E_INVALID_ARGUMENT;
                view.input_json = slice;
                seen |= bit;
            } else if (tag == ASX_CODEC_BIN_TAG_EXPECTED_EVENTS_JSON) {
                if ((seen & bit) != 0u) return ASX_E_INVALID_ARGUMENT;
                view.expected_events_json = slice;
                seen |= bit;
            } else if (tag == ASX_CODEC_BIN_TAG_EXPECTED_FINAL_SNAPSHOT_JSON) {
                if ((seen & bit) != 0u) return ASX_E_INVALID_ARGUMENT;
                view.expected_final_snapshot_json = slice;
                seen |= bit;
            } else if (tag == ASX_CODEC_BIN_TAG_EXPECTED_ERROR_CODES_JSON) {
                if ((seen & bit) != 0u) return ASX_E_INVALID_ARGUMENT;
                view.expected_error_codes_json = slice;
                seen |= bit;
            } else if (tag == ASX_CODEC_BIN_TAG_SEMANTIC_DIGEST) {
                if ((seen & bit) != 0u) return ASX_E_INVALID_ARGUMENT;
                view.semantic_digest = slice;
                seen |= bit;
            } else if (tag == ASX_CODEC_BIN_TAG_RUST_BASELINE_COMMIT) {
                if ((seen & bit) != 0u) return ASX_E_INVALID_ARGUMENT;
                view.rust_baseline_commit = slice;
                seen |= bit;
            } else if (tag == ASX_CODEC_BIN_TAG_RUST_TOOLCHAIN_COMMIT_HASH) {
                if ((seen & bit) != 0u) return ASX_E_INVALID_ARGUMENT;
                view.rust_toolchain_commit_hash = slice;
                seen |= bit;
            } else if (tag == ASX_CODEC_BIN_TAG_RUST_TOOLCHAIN_RELEASE) {
                if ((seen & bit) != 0u) return ASX_E_INVALID_ARGUMENT;
                view.rust_toolchain_release = slice;
                seen |= bit;
            } else if (tag == ASX_CODEC_BIN_TAG_RUST_TOOLCHAIN_HOST) {
                if ((seen & bit) != 0u) return ASX_E_INVALID_ARGUMENT;
                view.rust_toolchain_host = slice;
                seen |= bit;
            } else if (tag == ASX_CODEC_BIN_TAG_CARGO_LOCK_SHA256) {
                if ((seen & bit) != 0u) return ASX_E_INVALID_ARGUMENT;
                view.cargo_lock_sha256 = slice;
                seen |= bit;
            } else if (tag == ASX_CODEC_BIN_TAG_CAPTURE_RUN_ID) {
                if ((seen & bit) != 0u) return ASX_E_INVALID_ARGUMENT;
                view.capture_run_id = slice;
                seen |= bit;
            }
            continue;
        }

        return ASX_E_INVALID_ARGUMENT;
    }

    if (seen != ASX_BIN_SEEN_REQUIRED) {
        return ASX_E_INVALID_ARGUMENT;
    }
    if (!asx_codec_slice_matches(view.fixture_schema_version, "fixture-v1")) {
        return ASX_E_INVALID_ARGUMENT;
    }
    st = asx_codec_bin_validate_view(&view);
    if (st != ASX_OK) {
        return st;
    }

    *out_view = view;
    return ASX_OK;
}

static asx_status asx_codec_assign_dup_slice(char **out_text, asx_codec_slice slice)
{
    if (out_text == NULL || !asx_codec_slice_nonempty(slice)) {
        return ASX_E_INVALID_ARGUMENT;
    }
    *out_text = asx_codec_strdup_range(slice.ptr, slice.len);
    if (*out_text == NULL) {
        return ASX_E_RESOURCE_EXHAUSTED;
    }
    return ASX_OK;
}

static asx_status asx_codec_fixture_from_bin_view(const asx_codec_bin_fixture_view *view,
                                                  asx_canonical_fixture *out_fixture)
{
    asx_canonical_fixture parsed;
    asx_status st;

    if (view == NULL || out_fixture == NULL) {
        return ASX_E_INVALID_ARGUMENT;
    }

    asx_canonical_fixture_init(&parsed);
    parsed.codec = view->codec;
    parsed.seed = view->seed;

    st = asx_codec_assign_dup_slice(&parsed.scenario_id, view->scenario_id);
    if (st != ASX_OK) goto fail;
    st = asx_codec_assign_dup_slice(&parsed.fixture_schema_version, view->fixture_schema_version);
    if (st != ASX_OK) goto fail;
    st = asx_codec_assign_dup_slice(&parsed.scenario_dsl_version, view->scenario_dsl_version);
    if (st != ASX_OK) goto fail;
    st = asx_codec_assign_dup_slice(&parsed.profile, view->profile);
    if (st != ASX_OK) goto fail;
    st = asx_codec_assign_dup_slice(&parsed.input_json, view->input_json);
    if (st != ASX_OK) goto fail;
    st = asx_codec_assign_dup_slice(&parsed.expected_events_json, view->expected_events_json);
    if (st != ASX_OK) goto fail;
    st = asx_codec_assign_dup_slice(&parsed.expected_final_snapshot_json, view->expected_final_snapshot_json);
    if (st != ASX_OK) goto fail;
    st = asx_codec_assign_dup_slice(&parsed.expected_error_codes_json, view->expected_error_codes_json);
    if (st != ASX_OK) goto fail;
    st = asx_codec_assign_dup_slice(&parsed.semantic_digest, view->semantic_digest);
    if (st != ASX_OK) goto fail;
    st = asx_codec_assign_dup_slice(&parsed.provenance.rust_baseline_commit, view->rust_baseline_commit);
    if (st != ASX_OK) goto fail;
    st = asx_codec_assign_dup_slice(&parsed.provenance.rust_toolchain_commit_hash,
                                    view->rust_toolchain_commit_hash);
    if (st != ASX_OK) goto fail;
    st = asx_codec_assign_dup_slice(&parsed.provenance.rust_toolchain_release, view->rust_toolchain_release);
    if (st != ASX_OK) goto fail;
    st = asx_codec_assign_dup_slice(&parsed.provenance.rust_toolchain_host, view->rust_toolchain_host);
    if (st != ASX_OK) goto fail;
    st = asx_codec_assign_dup_slice(&parsed.provenance.cargo_lock_sha256, view->cargo_lock_sha256);
    if (st != ASX_OK) goto fail;
    st = asx_codec_assign_dup_slice(&parsed.provenance.capture_run_id, view->capture_run_id);
    if (st != ASX_OK) goto fail;

    st = asx_canonical_fixture_validate(&parsed);
    if (st != ASX_OK) {
        goto fail;
    }

    *out_fixture = parsed;
    return ASX_OK;

fail:
    asx_canonical_fixture_reset(&parsed);
    return st;
}

static asx_status asx_codec_encode_fixture_bin(const asx_canonical_fixture *fixture,
                                               asx_codec_buffer *out_payload)
{
    asx_status st;
    size_t payload_start;
    size_t payload_len;
    uint32_t checksum;
    unsigned char checksum_bytes[4];

    if (fixture == NULL || out_payload == NULL) {
        return ASX_E_INVALID_ARGUMENT;
    }

    st = asx_canonical_fixture_validate(fixture);
    if (st != ASX_OK) {
        return st;
    }
    if (strcmp(fixture->fixture_schema_version, "fixture-v1") != 0) {
        return ASX_E_INVALID_ARGUMENT;
    }

    out_payload->len = 0u;
    if (out_payload->data != NULL) {
        out_payload->data[0] = '\0';
    }

    st = asx_codec_buffer_append_bytes(out_payload, (const char *)g_asx_codec_bin_magic, 4u);
    if (st != ASX_OK) return st;
    st = asx_codec_bin_append_u8(out_payload, ASX_CODEC_BIN_FRAME_SCHEMA_VERSION_V1);
    if (st != ASX_OK) return st;
    st = asx_codec_bin_append_u8(out_payload, ASX_CODEC_BIN_FRAME_MESSAGE_FIXTURE);
    if (st != ASX_OK) return st;
    st = asx_codec_bin_append_u8(out_payload, ASX_CODEC_BIN_FLAG_CHECKSUM_FOOTER);
    if (st != ASX_OK) return st;
    st = asx_codec_bin_append_u32_be(out_payload, 0u);
    if (st != ASX_OK) return st;

    payload_start = out_payload->len;

    st = asx_codec_bin_append_string_field(out_payload, ASX_CODEC_BIN_TAG_SCENARIO_ID, fixture->scenario_id);
    if (st != ASX_OK) return st;
    st = asx_codec_bin_append_string_field(out_payload, ASX_CODEC_BIN_TAG_FIXTURE_SCHEMA_VERSION,
                                           fixture->fixture_schema_version);
    if (st != ASX_OK) return st;
    st = asx_codec_bin_append_string_field(out_payload, ASX_CODEC_BIN_TAG_SCENARIO_DSL_VERSION,
                                           fixture->scenario_dsl_version);
    if (st != ASX_OK) return st;
    st = asx_codec_bin_append_string_field(out_payload, ASX_CODEC_BIN_TAG_PROFILE, fixture->profile);
    if (st != ASX_OK) return st;
    st = asx_codec_bin_append_varint_field(out_payload, ASX_CODEC_BIN_TAG_CODEC, (uint64_t)fixture->codec);
    if (st != ASX_OK) return st;
    st = asx_codec_bin_append_varint_field(out_payload, ASX_CODEC_BIN_TAG_SEED, fixture->seed);
    if (st != ASX_OK) return st;
    st = asx_codec_bin_append_string_field(out_payload, ASX_CODEC_BIN_TAG_INPUT_JSON, fixture->input_json);
    if (st != ASX_OK) return st;
    st = asx_codec_bin_append_string_field(out_payload, ASX_CODEC_BIN_TAG_EXPECTED_EVENTS_JSON,
                                           fixture->expected_events_json);
    if (st != ASX_OK) return st;
    st = asx_codec_bin_append_string_field(out_payload, ASX_CODEC_BIN_TAG_EXPECTED_FINAL_SNAPSHOT_JSON,
                                           fixture->expected_final_snapshot_json);
    if (st != ASX_OK) return st;
    st = asx_codec_bin_append_string_field(out_payload, ASX_CODEC_BIN_TAG_EXPECTED_ERROR_CODES_JSON,
                                           fixture->expected_error_codes_json);
    if (st != ASX_OK) return st;
    st = asx_codec_bin_append_string_field(out_payload, ASX_CODEC_BIN_TAG_SEMANTIC_DIGEST,
                                           fixture->semantic_digest);
    if (st != ASX_OK) return st;
    st = asx_codec_bin_append_string_field(out_payload, ASX_CODEC_BIN_TAG_RUST_BASELINE_COMMIT,
                                           fixture->provenance.rust_baseline_commit);
    if (st != ASX_OK) return st;
    st = asx_codec_bin_append_string_field(out_payload, ASX_CODEC_BIN_TAG_RUST_TOOLCHAIN_COMMIT_HASH,
                                           fixture->provenance.rust_toolchain_commit_hash);
    if (st != ASX_OK) return st;
    st = asx_codec_bin_append_string_field(out_payload, ASX_CODEC_BIN_TAG_RUST_TOOLCHAIN_RELEASE,
                                           fixture->provenance.rust_toolchain_release);
    if (st != ASX_OK) return st;
    st = asx_codec_bin_append_string_field(out_payload, ASX_CODEC_BIN_TAG_RUST_TOOLCHAIN_HOST,
                                           fixture->provenance.rust_toolchain_host);
    if (st != ASX_OK) return st;
    st = asx_codec_bin_append_string_field(out_payload, ASX_CODEC_BIN_TAG_CARGO_LOCK_SHA256,
                                           fixture->provenance.cargo_lock_sha256);
    if (st != ASX_OK) return st;
    st = asx_codec_bin_append_string_field(out_payload, ASX_CODEC_BIN_TAG_CAPTURE_RUN_ID,
                                           fixture->provenance.capture_run_id);
    if (st != ASX_OK) return st;

    payload_len = out_payload->len - payload_start;
    if (payload_len > 0xffffffffu) {
        return ASX_E_RESOURCE_EXHAUSTED;
    }
    asx_codec_bin_store_u32_be((unsigned char *)(out_payload->data + 7), (uint32_t)payload_len);

    checksum = asx_codec_bin_checksum32((const unsigned char *)out_payload->data, out_payload->len);
    asx_codec_bin_store_u32_be(checksum_bytes, checksum);
    st = asx_codec_buffer_append_bytes(out_payload, (const char *)checksum_bytes, sizeof(checksum_bytes));
    if (st != ASX_OK) {
        return st;
    }

    return ASX_OK;
}

static asx_status asx_codec_decode_fixture_bin(const void *payload,
                                               size_t payload_len,
                                               asx_canonical_fixture *out_fixture)
{
    asx_codec_bin_fixture_view view;
    asx_status st;

    if (out_fixture == NULL) {
        return ASX_E_INVALID_ARGUMENT;
    }

    asx_codec_bin_fixture_view_init(&view);
    st = asx_codec_decode_fixture_bin_view(payload, payload_len, &view);
    if (st != ASX_OK) {
        return st;
    }
    return asx_codec_fixture_from_bin_view(&view, out_fixture);
}

static const asx_codec_vtable g_asx_codec_json_vtable = {
    ASX_CODEC_KIND_JSON,
    asx_codec_encode_fixture_json,
    asx_codec_decode_fixture_json_payload
};

static const asx_codec_vtable g_asx_codec_bin_vtable = {
    ASX_CODEC_KIND_BIN,
    asx_codec_encode_fixture_bin,
    asx_codec_decode_fixture_bin
};

const asx_codec_vtable *asx_codec_vtable_for(asx_codec_kind codec)
{
    switch (codec) {
    case ASX_CODEC_KIND_JSON:
        return &g_asx_codec_json_vtable;
    case ASX_CODEC_KIND_BIN:
        return &g_asx_codec_bin_vtable;
    default:
        return NULL;
    }
}

asx_status asx_codec_encode_fixture(asx_codec_kind codec,
                                    const asx_canonical_fixture *fixture,
                                    asx_codec_buffer *out_payload)
{
    const asx_codec_vtable *vt;

    vt = asx_codec_vtable_for(codec);
    if (vt == NULL || vt->encode_fixture == NULL) {
        return ASX_E_INVALID_ARGUMENT;
    }
    return vt->encode_fixture(fixture, out_payload);
}

asx_status asx_codec_decode_fixture(asx_codec_kind codec,
                                    const void *payload,
                                    size_t payload_len,
                                    asx_canonical_fixture *out_fixture)
{
    const asx_codec_vtable *vt;

    vt = asx_codec_vtable_for(codec);
    if (vt == NULL || vt->decode_fixture == NULL) {
        return ASX_E_INVALID_ARGUMENT;
    }
    return vt->decode_fixture(payload, payload_len, out_fixture);
}

/* ------------------------------------------------------------------ */
/* Cross-codec semantic equivalence (bd-2n0.3)                         */
/* ------------------------------------------------------------------ */

void asx_codec_equiv_report_init(asx_codec_equiv_report *report)
{
    if (report == NULL) return;
    memset(report, 0, sizeof(*report));
}

static void asx_equiv_report_add_diff(asx_codec_equiv_report *report,
                                      const char *field_name)
{
    size_t len;

    if (report == NULL) return;
    if (report->count >= ASX_EQUIV_MAX_DIFFS) return;

    len = strlen(field_name);
    if (len >= ASX_EQUIV_MAX_FIELD_NAME) {
        len = ASX_EQUIV_MAX_FIELD_NAME - 1u;
    }
    memcpy(report->diffs[report->count].field_name, field_name, len);
    report->diffs[report->count].field_name[len] = '\0';
    report->count++;
}

static int asx_equiv_str_eq(const char *a, const char *b)
{
    if (a == NULL && b == NULL) return 1;
    if (a == NULL || b == NULL) return 0;
    return strcmp(a, b) == 0;
}

asx_status asx_codec_fixture_semantic_eq(const asx_canonical_fixture *a,
                                         const asx_canonical_fixture *b,
                                         asx_codec_equiv_report *report)
{
    int mismatch = 0;

    if (a == NULL || b == NULL) return ASX_E_INVALID_ARGUMENT;

    if (report != NULL) {
        asx_codec_equiv_report_init(report);
    }

    /* Compare all semantic fields — codec is explicitly excluded */

    if (!asx_equiv_str_eq(a->scenario_id, b->scenario_id)) {
        asx_equiv_report_add_diff(report, "scenario_id");
        mismatch = 1;
    }
    if (!asx_equiv_str_eq(a->fixture_schema_version, b->fixture_schema_version)) {
        asx_equiv_report_add_diff(report, "fixture_schema_version");
        mismatch = 1;
    }
    if (!asx_equiv_str_eq(a->scenario_dsl_version, b->scenario_dsl_version)) {
        asx_equiv_report_add_diff(report, "scenario_dsl_version");
        mismatch = 1;
    }
    if (!asx_equiv_str_eq(a->profile, b->profile)) {
        asx_equiv_report_add_diff(report, "profile");
        mismatch = 1;
    }
    if (a->seed != b->seed) {
        asx_equiv_report_add_diff(report, "seed");
        mismatch = 1;
    }
    if (!asx_equiv_str_eq(a->input_json, b->input_json)) {
        asx_equiv_report_add_diff(report, "input_json");
        mismatch = 1;
    }
    if (!asx_equiv_str_eq(a->expected_events_json, b->expected_events_json)) {
        asx_equiv_report_add_diff(report, "expected_events_json");
        mismatch = 1;
    }
    if (!asx_equiv_str_eq(a->expected_final_snapshot_json,
                          b->expected_final_snapshot_json)) {
        asx_equiv_report_add_diff(report, "expected_final_snapshot_json");
        mismatch = 1;
    }
    if (!asx_equiv_str_eq(a->expected_error_codes_json,
                          b->expected_error_codes_json)) {
        asx_equiv_report_add_diff(report, "expected_error_codes_json");
        mismatch = 1;
    }
    if (!asx_equiv_str_eq(a->semantic_digest, b->semantic_digest)) {
        asx_equiv_report_add_diff(report, "semantic_digest");
        mismatch = 1;
    }

    /* Provenance fields */
    if (!asx_equiv_str_eq(a->provenance.rust_baseline_commit,
                          b->provenance.rust_baseline_commit)) {
        asx_equiv_report_add_diff(report, "provenance.rust_baseline_commit");
        mismatch = 1;
    }
    if (!asx_equiv_str_eq(a->provenance.rust_toolchain_commit_hash,
                          b->provenance.rust_toolchain_commit_hash)) {
        asx_equiv_report_add_diff(report, "provenance.rust_toolchain_commit_hash");
        mismatch = 1;
    }
    if (!asx_equiv_str_eq(a->provenance.rust_toolchain_release,
                          b->provenance.rust_toolchain_release)) {
        asx_equiv_report_add_diff(report, "provenance.rust_toolchain_release");
        mismatch = 1;
    }
    if (!asx_equiv_str_eq(a->provenance.rust_toolchain_host,
                          b->provenance.rust_toolchain_host)) {
        asx_equiv_report_add_diff(report, "provenance.rust_toolchain_host");
        mismatch = 1;
    }
    if (!asx_equiv_str_eq(a->provenance.cargo_lock_sha256,
                          b->provenance.cargo_lock_sha256)) {
        asx_equiv_report_add_diff(report, "provenance.cargo_lock_sha256");
        mismatch = 1;
    }
    if (!asx_equiv_str_eq(a->provenance.capture_run_id,
                          b->provenance.capture_run_id)) {
        asx_equiv_report_add_diff(report, "provenance.capture_run_id");
        mismatch = 1;
    }

    return mismatch ? ASX_E_EQUIVALENCE_MISMATCH : ASX_OK;
}

asx_status asx_codec_fixture_semantic_key(const asx_canonical_fixture *fixture,
                                          asx_codec_buffer *out_key)
{
    asx_status st;
    int is_first = 1;

    if (fixture == NULL || out_key == NULL) {
        return ASX_E_INVALID_ARGUMENT;
    }

    st = asx_canonical_fixture_validate(fixture);
    if (st != ASX_OK) {
        return st;
    }

    out_key->len = 0u;
    if (out_key->data != NULL) {
        out_key->data[0] = '\0';
    }

    st = asx_codec_buffer_append_char(out_key, '{');
    if (st != ASX_OK) return st;

    /* Alphabetical field order, codec excluded */
    st = asx_codec_buffer_append_string_field(out_key, &is_first,
                                              "profile", fixture->profile);
    if (st != ASX_OK) return st;

    st = asx_codec_buffer_append_string_field(out_key, &is_first,
                                              "scenario_id",
                                              fixture->scenario_id);
    if (st != ASX_OK) return st;

    st = asx_codec_buffer_append_u64_field(out_key, &is_first,
                                           "seed", fixture->seed);
    if (st != ASX_OK) return st;

    st = asx_codec_buffer_append_string_field(out_key, &is_first,
                                              "semantic_digest",
                                              fixture->semantic_digest);
    if (st != ASX_OK) return st;

    return asx_codec_buffer_append_char(out_key, '}');
}

asx_status asx_codec_cross_codec_verify(const asx_canonical_fixture *fixture,
                                        asx_codec_equiv_report *report)
{
    asx_canonical_fixture from_json;
    asx_canonical_fixture from_bin;
    asx_codec_buffer json_buf;
    asx_codec_buffer bin_buf;
    asx_status st;
    asx_status result;

    if (fixture == NULL) return ASX_E_INVALID_ARGUMENT;

    asx_canonical_fixture_init(&from_json);
    asx_canonical_fixture_init(&from_bin);
    asx_codec_buffer_init(&json_buf);
    asx_codec_buffer_init(&bin_buf);

    /* Encode as JSON */
    st = asx_codec_encode_fixture(ASX_CODEC_KIND_JSON, fixture, &json_buf);
    if (st != ASX_OK) {
        result = st;
        goto cleanup;
    }

    /* Encode as BIN */
    st = asx_codec_encode_fixture(ASX_CODEC_KIND_BIN, fixture, &bin_buf);
    if (st != ASX_OK) {
        result = st;
        goto cleanup;
    }

    /* Decode JSON */
    st = asx_codec_decode_fixture(ASX_CODEC_KIND_JSON,
                                  json_buf.data, json_buf.len,
                                  &from_json);
    if (st != ASX_OK) {
        result = st;
        goto cleanup;
    }

    /* Decode BIN */
    st = asx_codec_decode_fixture(ASX_CODEC_KIND_BIN,
                                  bin_buf.data, bin_buf.len,
                                  &from_bin);
    if (st != ASX_OK) {
        result = st;
        goto cleanup;
    }

    /* Compare decoded fixtures for semantic equivalence */
    result = asx_codec_fixture_semantic_eq(&from_json, &from_bin, report);

cleanup:
    asx_codec_buffer_reset(&bin_buf);
    asx_codec_buffer_reset(&json_buf);
    asx_canonical_fixture_reset(&from_bin);
    asx_canonical_fixture_reset(&from_json);
    return result;
}
