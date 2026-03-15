/*
 * asx_config.h — compile-time configuration and profile selection
 *
 * Profiles control resource defaults and platform adaptation without
 * altering semantic behavior. All profiles produce identical canonical
 * semantic digests for shared fixture sets.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_CONFIG_H
#define ASX_CONFIG_H

#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>
#include <stddef.h>
#include <stdint.h>

/* -------------------------------------------------------------------
 * Checkpoint-coverage waiver annotations (bd-66l.6)
 *
 * Used by the CI checkpoint-coverage lint gate to suppress false
 * positives for loops that intentionally do not call asx_checkpoint().
 *
 * ASX_CHECKPOINT_WAIVER("reason")      — per-loop waiver
 * ASX_CHECKPOINT_WAIVER_FILE("reason") — file-level waiver (all loops exempt)
 *
 * The reason string must explain why the loop is exempt (e.g.,
 * "kernel-scheduler: budget exhaustion provides bounded termination",
 * "codec-utility: input length bounded by buffer capacity contract").
 * ------------------------------------------------------------------- */
#ifndef ASX_CHECKPOINT_WAIVER
#define ASX_CHECKPOINT_WAIVER(reason) ((void)0)
#endif
#ifndef ASX_CHECKPOINT_WAIVER_FILE
#define ASX_CHECKPOINT_WAIVER_FILE(reason) ((void)0)
#endif

/*
 * Profile selection (exactly one must be defined at compile time).
 * If none is defined, ASX_PROFILE_CORE is assumed.
 */
#if !defined(ASX_PROFILE_CORE) && !defined(ASX_PROFILE_POSIX) && !defined(ASX_PROFILE_WIN32) &&    \
    !defined(ASX_PROFILE_FREESTANDING) && !defined(ASX_PROFILE_EMBEDDED_ROUTER) &&                 \
    !defined(ASX_PROFILE_HFT) && !defined(ASX_PROFILE_AUTOMOTIVE) &&                               \
    !defined(ASX_PROFILE_PARALLEL) && !defined(ASX_PROFILE_BROWSER)
#define ASX_PROFILE_CORE
#endif

/* Debug mode: enables ghost monitors (borrow ledger, protocol, linearity, determinism) */
#if !defined(ASX_DEBUG) && !defined(NDEBUG)
#define ASX_DEBUG 1
#endif

/* Ghost monitors: enabled by default when ASX_DEBUG is set.
 * Override with -DASX_DEBUG_GHOST=0 to disable ghost monitors in debug builds. */
#if defined(ASX_DEBUG) && ASX_DEBUG && !defined(ASX_DEBUG_GHOST)
#define ASX_DEBUG_GHOST 1
#endif

/* Deterministic mode: stable ordering, replay identity for fixed input/seed */
#ifndef ASX_DETERMINISTIC
#define ASX_DETERMINISTIC 1
#endif

/* ------------------------------------------------------------------ */
/* Resource classes                                                     */
/*                                                                     */
/* Resource classes are capability envelopes, not feature switches.     */
/* They scale operational limits (regions, tasks, timers, trace) while  */
/* preserving identical semantic behavior. Used primarily with          */
/* embedded/constrained profiles.                                      */
/*                                                                     */
/*   R1 — tight: aggressive caps for very constrained footprints       */
/*   R2 — balanced: typical router-class device defaults               */
/*   R3 — roomy: higher-capacity embedded/server crossover             */
/* ------------------------------------------------------------------ */

typedef enum {
    ASX_CLASS_R1 = 0, /* tight: very constrained footprint */
    ASX_CLASS_R2 = 1, /* balanced: typical router-class device */
    ASX_CLASS_R3 = 2, /* roomy: higher-capacity embedded/server */
    ASX_CLASS_COUNT = 3
} asx_resource_class;

/* Return the human-readable name of a resource class. Never returns NULL. */
ASX_API const char *asx_resource_class_name(asx_resource_class cls);

/* ------------------------------------------------------------------ */
/* Trace mode selection                                                */
/*                                                                     */
/* Controls how runtime trace events are retained:                     */
/*   RAM_RING         — bounded in-memory ring buffer (default)        */
/*   PERSISTENT_SPILL — spill to persistent storage path               */
/* ------------------------------------------------------------------ */

typedef enum {
    ASX_TRACE_MODE_RAM_RING = 0,        /* in-memory ring (default) */
    ASX_TRACE_MODE_PERSISTENT_SPILL = 1 /* flash/disk spill */
} asx_trace_mode;

/* Trace configuration for embedded/constrained profiles */
typedef struct {
    asx_trace_mode mode;        /* RAM_RING or PERSISTENT_SPILL */
    uint32_t ring_capacity;     /* event slots in RAM ring */
    int wear_safe;              /* 1: minimize flash write cycles */
    uint32_t flush_interval_ms; /* spill interval (0 = on-demand only) */
} asx_trace_config;

/* Initialize trace config with resource-class-appropriate defaults.
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if cfg is NULL. */
ASX_API asx_status asx_trace_config_init(asx_trace_config *cfg, asx_resource_class cls);

/* ------------------------------------------------------------------ */
/* Safety profiles                                                     */
/*                                                                     */
/* Orthogonal to platform profiles (ASX_PROFILE_*). Safety profiles    */
/* control invariant-checking overhead and ghost monitor activation.    */
/*                                                                     */
/* ASX_SAFETY_DEBUG    — full ghost monitors, error ledger, linearity  */
/*                       tracking. Intended for development and test.   */
/* ASX_SAFETY_HARDENED — error ledger active, must-use enforced,       */
/*                       ghost monitors disabled, allocator sealable.  */
/*                       Intended for production with diagnostics.     */
/* ASX_SAFETY_RELEASE  — zero-cost stubs for all monitors, minimal    */
/*                       overhead. Intended for performance-critical.   */
/* ------------------------------------------------------------------ */

typedef enum {
    ASX_SAFETY_DEBUG = 0,
    ASX_SAFETY_HARDENED = 1,
    ASX_SAFETY_RELEASE = 2
} asx_safety_profile;

/* Auto-select safety profile from compile-time flags if not explicit */
#ifndef ASX_SAFETY_PROFILE_SELECTED
#if defined(ASX_DEBUG) && ASX_DEBUG
#define ASX_SAFETY_PROFILE_SELECTED ASX_SAFETY_DEBUG
#elif defined(NDEBUG)
#define ASX_SAFETY_PROFILE_SELECTED ASX_SAFETY_RELEASE
#else
#define ASX_SAFETY_PROFILE_SELECTED ASX_SAFETY_HARDENED
#endif
#endif

/* Query the active safety profile at runtime */
ASX_API asx_safety_profile asx_safety_profile_active(void);

/* Return the human-readable name of a safety profile. Never returns NULL. */
ASX_API const char *asx_safety_profile_str(asx_safety_profile profile);

/* ------------------------------------------------------------------ */
/* Fault containment policy                                            */
/*                                                                     */
/* Determines how the runtime responds to invariant violations:        */
/*   FAIL_FAST     — abort immediately (debug builds)                  */
/*   POISON_REGION — poison the owning region, continue others         */
/*   ERROR_ONLY    — return error code, no containment overhead        */
/*                                                                     */
/* The default policy is derived from the active safety profile.       */
/* ------------------------------------------------------------------ */

typedef enum {
    ASX_CONTAIN_FAIL_FAST = 0,     /* debug: halt on first violation */
    ASX_CONTAIN_POISON_REGION = 1, /* hardened: poison region, continue */
    ASX_CONTAIN_ERROR_ONLY = 2     /* release: return error, minimal overhead */
} asx_containment_policy;

/* Return the default containment policy for a safety profile. */
ASX_API asx_containment_policy asx_containment_policy_for_profile(asx_safety_profile profile);

/* Return the active containment policy (derived from active profile). */
ASX_API asx_containment_policy asx_containment_policy_active(void);

/* ------------------------------------------------------------------ */
/* Fault injection (deterministic-mode only)                           */
/*                                                                     */
/* Injects controlled faults into clock, entropy, and allocator paths  */
/* for testing exhaustion/anomaly handling. Active only when            */
/* ASX_DETERMINISTIC is set. No-op stubs in non-deterministic builds.  */
/* ------------------------------------------------------------------ */

typedef enum {
    ASX_FAULT_NONE = 0,
    ASX_FAULT_CLOCK_SKEW = 1,    /* adds param ns to each clock read */
    ASX_FAULT_CLOCK_REVERSE = 2, /* subtracts param ns (simulates reversal) */
    ASX_FAULT_ENTROPY_CONST = 3, /* returns param as constant entropy value */
    ASX_FAULT_ALLOC_FAIL = 4     /* allocation returns NULL after trigger */
} asx_fault_kind;

typedef struct {
    asx_fault_kind kind;
    uint64_t param;         /* kind-specific parameter */
    uint32_t trigger_after; /* inject after N calls (0 = immediate) */
    uint32_t trigger_count; /* deactivate after N injections (0 = permanent) */
} asx_fault_injection;

/* Inject a controlled fault for deterministic testing.
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if fault is NULL. */
ASX_API asx_status asx_fault_inject(const asx_fault_injection *fault);

/* Clear all active fault injections. Returns ASX_OK. */
ASX_API asx_status asx_fault_clear(void);

/* Return the number of currently active fault injections. */
ASX_API uint32_t asx_fault_injection_count(void);

/* ------------------------------------------------------------------ */
/* Runtime hook contracts                                              */
/* ------------------------------------------------------------------ */

typedef void *(*asx_alloc_fn)(void *ctx, size_t size);
typedef void *(*asx_realloc_fn)(void *ctx, void *ptr, size_t size);
typedef void (*asx_free_fn)(void *ctx, void *ptr);

typedef asx_time (*asx_clock_now_ns_fn)(void *ctx);
typedef uint64_t (*asx_entropy_u64_fn)(void *ctx);

typedef asx_status (*asx_reactor_wait_fn)(void *ctx, uint32_t timeout_ms, uint32_t *ready_count);
typedef asx_status (*asx_ghost_reactor_wait_fn)(void *ctx, uint64_t logical_step,
                                                uint32_t *ready_count);

typedef void (*asx_log_sink_fn)(void *ctx, int level, const char *message);

typedef struct {
    void *ctx;
    asx_alloc_fn malloc_fn;
    asx_realloc_fn realloc_fn;
    asx_free_fn free_fn;
} asx_allocator_hooks;

typedef struct {
    void *ctx;
    asx_clock_now_ns_fn now_ns_fn;         /* wall or monotonic time */
    asx_clock_now_ns_fn logical_now_ns_fn; /* deterministic logical clock */
} asx_clock_hooks;

typedef struct {
    void *ctx;
    asx_entropy_u64_fn random_u64_fn;
} asx_entropy_hooks;

typedef struct {
    void *ctx;
    asx_reactor_wait_fn wait_fn;
    asx_ghost_reactor_wait_fn ghost_wait_fn;
} asx_reactor_hooks;

typedef struct {
    void *ctx;
    asx_log_sink_fn write_fn;
} asx_log_hooks;

typedef struct {
    asx_allocator_hooks allocator;
    asx_clock_hooks clock;
    asx_entropy_hooks entropy;
    asx_reactor_hooks reactor;
    asx_log_hooks log;
    uint8_t deterministic_seeded_prng; /* 1 when deterministic entropy stream is configured */
    uint8_t allocator_sealed;          /* 1 after asx_runtime_seal_allocator() */
} asx_runtime_hooks;

/* Wait policy (resource-plane only; does not affect semantics) */
typedef enum { ASX_WAIT_BUSY_SPIN = 0, ASX_WAIT_YIELD = 1, ASX_WAIT_SLEEP = 2 } asx_wait_policy;

/* Obligation leak response policy */
typedef enum {
    ASX_LEAK_PANIC = 0,
    ASX_LEAK_LOG = 1,
    ASX_LEAK_SILENT = 2,
    ASX_LEAK_RECOVER = 3
} asx_leak_response;

/* Finalizer escalation policy */
typedef enum {
    ASX_FINALIZER_SOFT = 0,
    ASX_FINALIZER_BOUNDED_LOG = 1,
    ASX_FINALIZER_BOUNDED_PANIC = 2
} asx_finalizer_escalation;

/* Leak escalation threshold */
typedef struct {
    uint64_t threshold;
    asx_leak_response escalate_to;
} asx_leak_escalation_config;

/*
 * Runtime configuration.
 * Uses size-field pattern for forward compatibility:
 *   asx_runtime_config cfg;
 *   cfg.size = sizeof(cfg);
 */
typedef struct {
    uint32_t size;                                 /* sizeof(asx_runtime_config) */
    asx_wait_policy wait_policy;                   /* profile-dependent default */
    asx_leak_response leak_response;               /* default: ASX_LEAK_LOG */
    asx_leak_escalation_config *leak_escalation;   /* optional */
    uint32_t finalizer_poll_budget;                /* default: 100 */
    uint64_t finalizer_time_budget_ns;             /* default: 5000000000 (5s) */
    asx_finalizer_escalation finalizer_escalation; /* default: BOUNDED_LOG */
    uint16_t max_cancel_chain_depth;               /* default: 16 */
    uint32_t max_cancel_chain_memory;              /* default: 4096 */
} asx_runtime_config;

/* Initialize config with profile-appropriate defaults */
ASX_API void asx_runtime_config_init(asx_runtime_config *cfg);

/* Initialize hook table with safe defaults.
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if hooks is NULL. */
ASX_API asx_status asx_runtime_hooks_init(asx_runtime_hooks *hooks);

/* Validate hooks under deterministic/non-deterministic policy.
 * Returns ASX_OK if valid, ASX_E_HOOK_INVALID if constraints violated. */
ASX_API asx_status asx_runtime_hooks_validate(const asx_runtime_hooks *hooks,
                                              int deterministic_mode);

/* Install active runtime hooks.
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if hooks is NULL. */
ASX_API asx_status asx_runtime_set_hooks(const asx_runtime_hooks *hooks);

/* Read currently active hooks (never NULL once initialized) */
ASX_API const asx_runtime_hooks *asx_runtime_get_hooks(void);

/* Seal allocator for steady-state no-allocation profiles.
 * Returns ASX_OK on success. After sealing, asx_runtime_alloc returns
 * ASX_E_ALLOCATOR_SEALED. */
ASX_API asx_status asx_runtime_seal_allocator(void);

/* Allocate memory via hook. Returns ASX_OK on success,
 * ASX_E_HOOK_MISSING if no allocator, ASX_E_ALLOCATOR_SEALED if sealed. */
ASX_API asx_status asx_runtime_alloc(size_t size, void **out_ptr);

/* Reallocate memory via hook. Returns ASX_OK on success,
 * ASX_E_HOOK_MISSING if no allocator, ASX_E_ALLOCATOR_SEALED if sealed. */
ASX_API asx_status asx_runtime_realloc(void *ptr, size_t size, void **out_ptr);

/* Free memory via hook. Returns ASX_OK on success,
 * ASX_E_HOOK_MISSING if no allocator. */
ASX_API asx_status asx_runtime_free(void *ptr);

/* Read current time via clock hook. Returns ASX_OK on success,
 * ASX_E_HOOK_MISSING if no clock hook installed. */
ASX_API asx_status asx_runtime_now_ns(asx_time *out_now);

/* Read random u64 via the configured deterministic entropy stream.
 * Returns ASX_OK on success, ASX_E_HOOK_MISSING if hooks are not installed,
 * and ASX_E_INVALID_STATE when the active hook table does not provide a
 * deterministic entropy source. */
ASX_API asx_status asx_runtime_random_u64(uint64_t *out_value);
/* Wait for reactor readiness. Returns ready count via out_ready_count.
 * Returns ASX_E_HOOK_MISSING if no reactor hook installed. */
ASX_API asx_status asx_runtime_reactor_wait(uint32_t timeout_ms, uint32_t *out_ready_count,
                                            uint64_t logical_step);
/* Write a log message at the given severity level.
 * Returns ASX_E_HOOK_MISSING if the runtime hook table is not installed.
 * Returns ASX_OK with no side effects when hooks are installed but no log sink is configured. */
ASX_API asx_status asx_runtime_log_write(int level, const char *message);

#endif /* ASX_CONFIG_H */
