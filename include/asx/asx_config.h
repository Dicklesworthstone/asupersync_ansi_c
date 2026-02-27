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

#include <stdint.h>
#include <asx/asx_export.h>

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

/* Deterministic mode: stable ordering, replay identity for fixed input/seed */
#ifndef ASX_DETERMINISTIC
  #define ASX_DETERMINISTIC 1
#endif

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

#endif /* ASX_CONFIG_H */
