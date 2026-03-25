/*
 * migration.c — version migration and compatibility helpers
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/migration/migration.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Version constants                                                   */
/* ------------------------------------------------------------------ */

#define ASX_VERSION_MAJOR 0
#define ASX_VERSION_MINOR 1
#define ASX_VERSION_PATCH 0

#define ASX_WIRE_FORMAT_V1 1u

/* ------------------------------------------------------------------ */
/* Version info                                                        */
/* ------------------------------------------------------------------ */

asx_version asx_version_current(void) {
    asx_version v;
    v.major = ASX_VERSION_MAJOR;
    v.minor = ASX_VERSION_MINOR;
    v.patch = ASX_VERSION_PATCH;
    return v;
}

const char *asx_version_str(void) { return "0.1.0"; }

uint32_t asx_wire_format_version(void) { return ASX_WIRE_FORMAT_V1; }

/* ------------------------------------------------------------------ */
/* Compatibility checking                                              */
/* ------------------------------------------------------------------ */

const char *asx_compat_level_str(asx_compat_level level) {
    switch (level) {
    case ASX_COMPAT_FULL:         return "full";
    case ASX_COMPAT_FORWARD:      return "forward";
    case ASX_COMPAT_BACKWARD:     return "backward";
    case ASX_COMPAT_INCOMPATIBLE: return "incompatible";
    default:                      return "unknown";
    }
}

asx_compat_level asx_check_wire_compat(uint32_t wire_version) {
    if (wire_version == ASX_WIRE_FORMAT_V1) return ASX_COMPAT_FULL;
    if (wire_version < ASX_WIRE_FORMAT_V1) return ASX_COMPAT_FORWARD;
    /* Future version — we can't read it */
    return ASX_COMPAT_INCOMPATIBLE;
}

/* ------------------------------------------------------------------ */
/* Migration guidance                                                  */
/* ------------------------------------------------------------------ */

asx_status asx_migration_check(uint32_t wire_version, asx_migration_report *report) {
    if (report == NULL) return ASX_E_INVALID_ARGUMENT;

    report->source_version = wire_version;
    report->target_version = ASX_WIRE_FORMAT_V1;
    report->level = asx_check_wire_compat(wire_version);

    switch (report->level) {
    case ASX_COMPAT_FULL:
        report->summary = "Wire format matches current version";
        report->migration_action = NULL;
        break;
    case ASX_COMPAT_FORWARD:
        report->summary = "Data from older wire format; forward-compatible read";
        report->migration_action = "Re-encode data with current codec for optimal format";
        break;
    case ASX_COMPAT_BACKWARD:
        report->summary = "Data from newer wire format; limited backward compatibility";
        report->migration_action = "Upgrade runtime to match data format version";
        break;
    case ASX_COMPAT_INCOMPATIBLE:
        report->summary = "Wire format incompatible with current runtime";
        report->migration_action = "Upgrade runtime or re-export data from source system";
        break;
    }

    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Feature availability probes                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    int available;
    const char *deferral;
} feature_entry;

static const feature_entry g_features[] = {
    {"raptorq", 1, NULL},
    {"distributed", 1, NULL},
    {"grpc", 1, NULL},
    {"http", 1, NULL},
    {"tls", 1, NULL},
    {"websocket", 1, NULL},
    {"quic", 1, NULL},
    {"web", 1, NULL},
    {"db", 1, NULL},
    {"messaging", 1, NULL},
    {"server", 1, NULL},
    {"pipe", 1, NULL},
    {"join_set", 1, NULL},
    {"gen_server", 1, NULL},
    {"cli", 1, NULL},
    {"actor", 1, NULL},
    {"encoding", 1, NULL},
    {"decoding", 1, NULL},
    {"trace", 1, NULL},
    {"replay", 1, NULL},
    {"evidence", 1, NULL},
    {"monitor", 1, NULL},
    {"rwlock", 1, NULL},
    {"hedge", 1, NULL},
    {"map_reduce", 1, NULL},
    {"contended_mutex", 1, NULL},
    {"stream_combinators", 1, NULL},
    {"pool", 1, NULL},
    {"det_hash", 1, NULL},
    {"concurrency_limit", 1, NULL},
    {"filter_middleware", 1, NULL},
    {"load_balance", 1, NULL},
    {"reconnect", 1, NULL},
    {"steer", 1, NULL},
    {NULL, 0, NULL}};

int asx_feature_available(const char *feature_name) {
    const feature_entry *f;
    if (feature_name == NULL) return 0;
    for (f = g_features; f->name != NULL; f++) {
        if (strcmp(f->name, feature_name) == 0) return f->available;
    }
    return 0;
}

const char *asx_feature_deferral_reason(const char *feature_name) {
    const feature_entry *f;
    if (feature_name == NULL) return NULL;
    for (f = g_features; f->name != NULL; f++) {
        if (strcmp(f->name, feature_name) == 0) return f->deferral;
    }
    return NULL;
}
