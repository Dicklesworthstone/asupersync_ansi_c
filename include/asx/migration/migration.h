/*
 * asx/migration/migration.h — version migration and compatibility helpers
 *
 * Provides version identification, compatibility checking, and
 * migration guidance for data-plane and wire-format evolution.
 *
 * These helpers allow callers to:
 *   - Query the current runtime version
 *   - Check wire-format compatibility between versions
 *   - Get migration guidance when formats diverge
 *
 * Upstream Rust parity: migration module exports.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_MIGRATION_H
#define ASX_MIGRATION_H

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Version info
 * ------------------------------------------------------------------- */

typedef struct {
    uint16_t major;
    uint16_t minor;
    uint16_t patch;
} asx_version;

/* Returns the current runtime version. */
ASX_API asx_version asx_version_current(void);

/* Returns the version as a string (e.g., "0.1.0"). Never NULL. */
ASX_API ASX_MUST_USE const char *asx_version_str(void);

/* -------------------------------------------------------------------
 * Wire format version
 * ------------------------------------------------------------------- */

/* Returns the current wire format version used by codecs and traces. */
ASX_API uint32_t asx_wire_format_version(void);

/* -------------------------------------------------------------------
 * Compatibility checking
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_COMPAT_FULL = 0,        /* fully compatible, no migration needed */
    ASX_COMPAT_FORWARD = 1,     /* forward-compatible: newer reader, older data */
    ASX_COMPAT_BACKWARD = 2,    /* backward-compatible: older reader, newer data */
    ASX_COMPAT_INCOMPATIBLE = 3 /* incompatible: migration required */
} asx_compat_level;

/* Returns a human-readable string for a compatibility level. */
ASX_API ASX_MUST_USE const char *asx_compat_level_str(asx_compat_level level);

/* Check compatibility between current runtime and a wire format version.
 * Returns the compatibility level. */
ASX_API ASX_MUST_USE asx_compat_level asx_check_wire_compat(uint32_t wire_version);

/* -------------------------------------------------------------------
 * Migration guidance
 * ------------------------------------------------------------------- */

typedef struct {
    asx_compat_level level;
    const char *summary;          /* brief description of compatibility status */
    const char *migration_action; /* recommended action (NULL if none needed) */
    uint32_t source_version;      /* version being migrated from */
    uint32_t target_version;      /* version being migrated to (current) */
} asx_migration_report;

/* Produce a migration report for data produced with a given wire version.
 * Returns ASX_E_INVALID_ARGUMENT if report is NULL. */
ASX_API ASX_MUST_USE asx_status asx_migration_check(uint32_t wire_version,
                                                      asx_migration_report *report);

/* -------------------------------------------------------------------
 * Feature availability probes
 * ------------------------------------------------------------------- */

/* Check if a named feature is available in this build.
 * Known features: "raptorq", "distributed", "grpc", "http", "tls".
 * Returns nonzero if available, zero if deferred/absent. */
ASX_API int asx_feature_available(const char *feature_name);

/* Get the deferral reason for an unavailable feature.
 * Returns NULL if the feature is available or unknown. */
ASX_API const char *asx_feature_deferral_reason(const char *feature_name);

#ifdef __cplusplus
}
#endif

#endif /* ASX_MIGRATION_H */
