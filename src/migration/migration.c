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
    {"raptorq", 0,
     "RaptorQ erasure coding deferred per DEF-009 (Wave D): specialized ~18k LOC surface"},
    {"distributed", 0,
     "Distributed runtime deferred per DEF-008 (Wave D): requires stable transport layer"},
    {"grpc", 0, "gRPC deferred per DEF-007 (Wave D): requires HTTP/2 and transport substrate"},
    {"http", 0,
     "HTTP deferred per DEF-006 (Wave D): requires networking core (bd-yx9r.8) to land first"},
    {"tls", 0, "TLS deferred per DEF-005 (Wave D): requires networking and transport substrate"},
    {"actor", 1, NULL},
    {"encoding", 1, NULL},
    {"decoding", 1, NULL},
    {"trace", 1, NULL},
    {"replay", 1, NULL},
    {"evidence", 1, NULL},
    {"monitor", 1, NULL},
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
