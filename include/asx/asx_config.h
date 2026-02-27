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

#include <stddef.h>
#include <stdint.h>
#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>

/*
 * Profile selection (exactly one must be defined at compile time).
 * If none is defined, ASX_PROFILE_CORE is assumed.
 */
#if !defined(ASX_PROFILE_CORE) && \
    !defined(ASX_PROFILE_POSIX) && \
    !defined(ASX_PROFILE_WIN32) && \
    !defined(ASX_PROFILE_FREESTANDING) && \
    !defined(ASX_PROFILE_EMBEDDED_ROUTER) && \
    !defined(ASX_PROFILE_HFT) && \
    !defined(ASX_PROFILE_AUTOMOTIVE)
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
    ASX_SAFETY_DEBUG    = 0,
    ASX_SAFETY_HARDENED = 1,
    ASX_SAFETY_RELEASE  = 2
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
ASX_API const char *asx_safety_profile_str(asx_safety_profile profile);

/* ------------------------------------------------------------------ */
/* Fault injection (deterministic-mode only)                           */
/*                                                                     */
/* Injects controlled faults into clock, entropy, and allocator paths  */
/* for testing exhaustion/anomaly handling. Active only when            */
/* ASX_DETERMINISTIC is set. No-op stubs in non-deterministic builds.  */
/* ------------------------------------------------------------------ */

typedef enum {
    ASX_FAULT_NONE          = 0,
    ASX_FAULT_CLOCK_SKEW    = 1,  /* adds param ns to each clock read */
    ASX_FAULT_CLOCK_REVERSE = 2,  /* subtracts param ns (simulates reversal) */
    ASX_FAULT_ENTROPY_CONST = 3,  /* returns param as constant entropy value */
    ASX_FAULT_ALLOC_FAIL    = 4   /* allocation returns NULL after trigger */
} asx_fault_kind;

typedef struct {
    asx_fault_kind kind;
    uint64_t       param;         /* kind-specific parameter */
    uint32_t       trigger_after; /* inject after N calls (0 = immediate) */
    uint32_t       trigger_count; /* deactivate after N injections (0 = permanent) */
} asx_fault_injection;

ASX_API asx_status asx_fault_inject(const asx_fault_injection *fault);
ASX_API asx_status asx_fault_clear(void);
ASX_API uint32_t   asx_fault_injection_count(void);

/* ------------------------------------------------------------------ */
/* Runtime hook contracts                                              */
/* ------------------------------------------------------------------ */

typedef void *(*asx_alloc_fn)(void *ctx, size_t size);
typedef void *(*asx_realloc_fn)(void *ctx, void *ptr, size_t size);
typedef void  (*asx_free_fn)(void *ctx, void *ptr);

typedef asx_time (*asx_clock_now_ns_fn)(void *ctx);
typedef uint64_t (*asx_entropy_u64_fn)(void *ctx);

typedef asx_status (*asx_reactor_wait_fn)(void *ctx, uint32_t timeout_ms, uint32_t *ready_count);
typedef asx_status (*asx_ghost_reactor_wait_fn)(void *ctx, uint64_t logical_step, uint32_t *ready_count);

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
typedef enum {
    ASX_WAIT_BUSY_SPIN = 0,
    ASX_WAIT_YIELD     = 1,
    ASX_WAIT_SLEEP     = 2
} asx_wait_policy;

/* Obligation leak response policy */
typedef enum {
    ASX_LEAK_PANIC   = 0,
    ASX_LEAK_LOG     = 1,
    ASX_LEAK_SILENT  = 2,
    ASX_LEAK_RECOVER = 3
} asx_leak_response;

/* Finalizer escalation policy */
typedef enum {
    ASX_FINALIZER_SOFT         = 0,
    ASX_FINALIZER_BOUNDED_LOG  = 1,
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
    uint32_t size;                       /* sizeof(asx_runtime_config) */
    asx_wait_policy wait_policy;         /* profile-dependent default */
    asx_leak_response leak_response;     /* default: ASX_LEAK_LOG */
    asx_leak_escalation_config *leak_escalation; /* optional */
    uint32_t finalizer_poll_budget;      /* default: 100 */
    uint64_t finalizer_time_budget_ns;   /* default: 5000000000 (5s) */
    asx_finalizer_escalation finalizer_escalation; /* default: BOUNDED_LOG */
    uint16_t max_cancel_chain_depth;     /* default: 16 */
    uint32_t max_cancel_chain_memory;    /* default: 4096 */
} asx_runtime_config;

/* Initialize config with profile-appropriate defaults */
ASX_API void asx_runtime_config_init(asx_runtime_config *cfg);

/* Initialize hook table with safe defaults */
ASX_API asx_status asx_runtime_hooks_init(asx_runtime_hooks *hooks);

/* Validate hooks under deterministic/non-deterministic policy */
ASX_API asx_status asx_runtime_hooks_validate(const asx_runtime_hooks *hooks, int deterministic_mode);

/* Install active runtime hooks */
ASX_API asx_status asx_runtime_set_hooks(const asx_runtime_hooks *hooks);

/* Read currently active hooks (never NULL once initialized) */
ASX_API const asx_runtime_hooks *asx_runtime_get_hooks(void);

/* Seal allocator for steady-state no-allocation profiles */
ASX_API asx_status asx_runtime_seal_allocator(void);

/* Hook-backed runtime helpers */
ASX_API asx_status asx_runtime_alloc(size_t size, void **out_ptr);
ASX_API asx_status asx_runtime_realloc(void *ptr, size_t size, void **out_ptr);
ASX_API asx_status asx_runtime_free(void *ptr);
ASX_API asx_status asx_runtime_now_ns(asx_time *out_now);
ASX_API asx_status asx_runtime_random_u64(uint64_t *out_value);
ASX_API asx_status asx_runtime_reactor_wait(uint32_t timeout_ms, uint32_t *out_ready_count, uint64_t logical_step);
ASX_API asx_status asx_runtime_log_write(int level, const char *message);

#endif /* ASX_CONFIG_H */
