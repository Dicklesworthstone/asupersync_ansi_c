/*
 * asx/core/config_types.h — domain-specific runtime configuration types
 *
 * Provides structured configuration types for backoff, timeout, transport,
 * security, and adaptive policies. These correspond to the upstream
 * crate-root re-exports: BackoffConfig, TimeoutConfig, TransportConfig,
 * SecurityConfig, AdaptiveConfig.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_CORE_CONFIG_TYPES_H
#define ASX_CORE_CONFIG_TYPES_H

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Backoff configuration
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_BACKOFF_NONE = 0,
    ASX_BACKOFF_CONSTANT = 1,
    ASX_BACKOFF_LINEAR = 2,
    ASX_BACKOFF_EXPONENTIAL = 3,
    ASX_BACKOFF_JITTERED = 4
} asx_backoff_strategy;

typedef struct {
    asx_backoff_strategy strategy;
    uint64_t initial_delay_ns;
    uint64_t max_delay_ns;
    double multiplier; /* for exponential: delay *= multiplier each attempt */
    uint32_t max_retries;
} asx_backoff_config;

/* Initialize backoff config with defaults (exponential, 100ms initial, 30s max). */
ASX_API void asx_backoff_config_init(asx_backoff_config *cfg);

/* Compute the backoff delay for the given attempt number. */
ASX_API uint64_t asx_backoff_delay_for_attempt(const asx_backoff_config *cfg, uint32_t attempt);

/* -------------------------------------------------------------------
 * Timeout configuration
 * ------------------------------------------------------------------- */

typedef struct {
    uint64_t connect_timeout_ns;
    uint64_t request_timeout_ns;
    uint64_t idle_timeout_ns;
    uint64_t shutdown_timeout_ns;
} asx_timeout_config;

/* Initialize timeout config with defaults (5s connect, 30s request, 10s idle). */
ASX_API void asx_timeout_config_init(asx_timeout_config *cfg);

/* -------------------------------------------------------------------
 * Transport configuration
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t max_connections;
    uint32_t max_concurrent_streams;
    uint32_t keepalive_interval_ms;
    uint32_t keepalive_timeout_ms;
    uint8_t tcp_nodelay;
    uint8_t enable_tls;
} asx_transport_config;

/* Initialize transport config with defaults. */
ASX_API void asx_transport_config_init(asx_transport_config *cfg);

/* -------------------------------------------------------------------
 * Security configuration
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_SECURITY_MODE_DISABLED = 0,
    ASX_SECURITY_MODE_PERMISSIVE = 1,
    ASX_SECURITY_MODE_ENFORCING = 2
} asx_security_mode;

typedef struct {
    asx_security_mode mode;
    uint8_t require_authentication;
    uint8_t enable_audit_trail;
    uint32_t max_audit_entries;
    uint64_t token_ttl_ns;
} asx_security_config;

/* Initialize security config with defaults (disabled mode). */
ASX_API void asx_security_config_init(asx_security_config *cfg);

/* -------------------------------------------------------------------
 * Adaptive configuration
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t sample_window_ms;
    uint32_t adjustment_interval_ms;
    double target_utilization;   /* 0.0 - 1.0 */
    double scale_up_threshold;   /* utilization above this triggers scale-up */
    double scale_down_threshold; /* utilization below this triggers scale-down */
    uint32_t min_concurrency;
    uint32_t max_concurrency;
} asx_adaptive_config;

/* Initialize adaptive config with defaults. */
ASX_API void asx_adaptive_config_init(asx_adaptive_config *cfg);

/* -------------------------------------------------------------------
 * Config loader (runtime config from structured sources)
 * ------------------------------------------------------------------- */

typedef struct {
    asx_backoff_config backoff;
    asx_timeout_config timeout;
    asx_transport_config transport;
    asx_security_config security;
    asx_adaptive_config adaptive;
} asx_config_bundle;

/* Initialize a config bundle with defaults for all sub-configs. */
ASX_API void asx_config_bundle_init(asx_config_bundle *bundle);

/* Validate a config bundle. Returns ASX_OK if all sub-configs are consistent. */
ASX_API asx_status asx_config_bundle_validate(const asx_config_bundle *bundle);

#ifdef __cplusplus
}
#endif

#endif /* ASX_CORE_CONFIG_TYPES_H */
