/*
 * config_types.c — domain-specific runtime configuration
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/core/config_types.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Backoff                                                             */
/* ------------------------------------------------------------------ */

void asx_backoff_config_init(asx_backoff_config *cfg) {
    if (cfg == NULL) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->strategy = ASX_BACKOFF_EXPONENTIAL;
    cfg->initial_delay_ns = 100000000ULL; /* 100ms */
    cfg->max_delay_ns = 30000000000ULL;   /* 30s */
    cfg->multiplier = 2.0;
    cfg->max_retries = 5;
}

uint64_t asx_backoff_delay_for_attempt(const asx_backoff_config *cfg, uint32_t attempt) {
    uint64_t delay;
    uint32_t i;

    if (cfg == NULL) return 0;
    if (attempt >= cfg->max_retries) return cfg->max_delay_ns;

    switch (cfg->strategy) {
    case ASX_BACKOFF_NONE: return 0;
    case ASX_BACKOFF_CONSTANT: return cfg->initial_delay_ns;
    case ASX_BACKOFF_LINEAR: {
        uint64_t factor = (uint64_t)(attempt + 1u);
        /* Overflow-safe: check before multiply */
        if (cfg->initial_delay_ns > 0u && factor > cfg->max_delay_ns / cfg->initial_delay_ns) {
            return cfg->max_delay_ns;
        }
        delay = cfg->initial_delay_ns * factor;
        return delay > cfg->max_delay_ns ? cfg->max_delay_ns : delay;
    }
    case ASX_BACKOFF_EXPONENTIAL:
        delay = cfg->initial_delay_ns;
        for (i = 0; i < attempt; i++) {
            /* ASX_CHECKPOINT_WAIVER("bounded aggregation") */
            delay = (uint64_t)((double)delay * cfg->multiplier);
            if (delay > cfg->max_delay_ns) return cfg->max_delay_ns;
        }
        return delay;
    case ASX_BACKOFF_JITTERED:
        delay = cfg->initial_delay_ns;
        for (i = 0; i < attempt; i++) {
            /* ASX_CHECKPOINT_WAIVER("bounded aggregation") */
            delay = (uint64_t)((double)delay * cfg->multiplier);
            if (delay > cfg->max_delay_ns) {
                delay = cfg->max_delay_ns;
                break;
            }
        }
        /* Simple deterministic "jitter": reduce by attempt-dependent fraction */
        delay = delay - (delay / (uint64_t)(attempt + 2u));
        return delay;
    }
    return cfg->initial_delay_ns;
}

/* ------------------------------------------------------------------ */
/* Timeout                                                             */
/* ------------------------------------------------------------------ */

void asx_timeout_config_init(asx_timeout_config *cfg) {
    if (cfg == NULL) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->connect_timeout_ns = 5000000000ULL;   /* 5s */
    cfg->request_timeout_ns = 30000000000ULL;  /* 30s */
    cfg->idle_timeout_ns = 60000000000ULL;     /* 60s */
    cfg->shutdown_timeout_ns = 10000000000ULL; /* 10s */
}

/* ------------------------------------------------------------------ */
/* Transport                                                           */
/* ------------------------------------------------------------------ */

void asx_transport_config_init(asx_transport_config *cfg) {
    if (cfg == NULL) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->max_connections = 16;
    cfg->max_concurrent_streams = 100;
    cfg->keepalive_interval_ms = 20000; /* 20s */
    cfg->keepalive_timeout_ms = 10000;  /* 10s */
    cfg->tcp_nodelay = 1;
    cfg->enable_tls = 0;
}

/* ------------------------------------------------------------------ */
/* Security                                                            */
/* ------------------------------------------------------------------ */

void asx_security_config_init(asx_security_config *cfg) {
    if (cfg == NULL) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->mode = ASX_SECURITY_MODE_ENFORCING;
    cfg->require_authentication = 1;
    cfg->enable_audit_trail = 1;
    cfg->max_audit_entries = 1024;
    cfg->token_ttl_ns = 3600000000000ULL; /* 1 hour */
}

/* ------------------------------------------------------------------ */
/* Adaptive                                                            */
/* ------------------------------------------------------------------ */

void asx_adaptive_config_init(asx_adaptive_config *cfg) {
    if (cfg == NULL) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->sample_window_ms = 1000;
    cfg->adjustment_interval_ms = 5000;
    cfg->target_utilization = 0.7;
    cfg->scale_up_threshold = 0.8;
    cfg->scale_down_threshold = 0.3;
    cfg->min_concurrency = 1;
    cfg->max_concurrency = 64;
}

/* ------------------------------------------------------------------ */
/* Config bundle                                                       */
/* ------------------------------------------------------------------ */

void asx_config_bundle_init(asx_config_bundle *bundle) {
    if (bundle == NULL) return;
    asx_backoff_config_init(&bundle->backoff);
    asx_timeout_config_init(&bundle->timeout);
    asx_transport_config_init(&bundle->transport);
    asx_security_config_init(&bundle->security);
    asx_adaptive_config_init(&bundle->adaptive);
}

asx_status asx_config_bundle_validate(const asx_config_bundle *bundle) {
    if (bundle == NULL) return ASX_E_INVALID_ARGUMENT;
    if (bundle->backoff.max_retries > 100) return ASX_E_INVALID_ARGUMENT;
    if (bundle->timeout.connect_timeout_ns == 0) return ASX_E_INVALID_ARGUMENT;
    if (bundle->transport.max_connections == 0) return ASX_E_INVALID_ARGUMENT;
    if (bundle->adaptive.min_concurrency > bundle->adaptive.max_concurrency)
        return ASX_E_INVALID_ARGUMENT;
    if (bundle->adaptive.target_utilization <= 0.0 || bundle->adaptive.target_utilization > 1.0)
        return ASX_E_INVALID_ARGUMENT;
    return ASX_OK;
}
