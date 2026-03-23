/*
 * evidence.c — explicit evidence-family helpers
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/evidence/evidence.h>
#include <asx/evidence_sink/evidence_sink.h>
#include <string.h>

const char *asx_evidence_level_str(asx_evidence_level level) {
    switch (level) {
    case ASX_EVIDENCE_INFO: return "INFO";
    case ASX_EVIDENCE_PASS: return "PASS";
    case ASX_EVIDENCE_WARN: return "WARN";
    case ASX_EVIDENCE_FAIL: return "FAIL";
    }
    return "UNKNOWN";
}

int asx_evidence_entry_has_entity(const asx_evidence_entry *entry) {
    if (entry == NULL) return 0;
    return entry->entity_id != 0 ? 1 : 0;
}

int asx_evidence_is_failure(const asx_evidence_entry *entry) {
    if (entry == NULL) return 0;
    return entry->level == ASX_EVIDENCE_FAIL ? 1 : 0;
}

int asx_evidence_is_warning(const asx_evidence_entry *entry) {
    if (entry == NULL) return 0;
    return entry->level == ASX_EVIDENCE_WARN ? 1 : 0;
}

int asx_evidence_is_pass(const asx_evidence_entry *entry) {
    if (entry == NULL) return 0;
    return entry->level == ASX_EVIDENCE_PASS ? 1 : 0;
}

uint32_t asx_evidence_count_by_level(const asx_evidence_sink *sink, asx_evidence_level level) {
    uint32_t i, count;

    if (sink == NULL) return 0u;
    count = 0u;
    for (i = 0u; i < sink->count; i++) {
        if (sink->entries[i].level == level) count++;
    }
    return count;
}

uint32_t asx_evidence_count_for_source(const asx_evidence_sink *sink, const char *source_prefix) {
    uint32_t i, count;
    size_t prefix_len;

    if (sink == NULL || source_prefix == NULL) return 0u;
    prefix_len = strlen(source_prefix);
    count = 0u;
    for (i = 0u; i < sink->count; i++) {
        if (sink->entries[i].source != NULL &&
            strncmp(sink->entries[i].source, source_prefix, prefix_len) == 0) {
            count++;
        }
    }
    return count;
}

const asx_evidence_entry *asx_evidence_first_failure(const asx_evidence_sink *sink) {
    uint32_t i;

    if (sink == NULL) return NULL;
    for (i = 0u; i < sink->count; i++) {
        if (sink->entries[i].level == ASX_EVIDENCE_FAIL) return &sink->entries[i];
    }
    return NULL;
}

int asx_evidence_has_entity(const asx_evidence_sink *sink, uint64_t entity_id) {
    uint32_t i;

    if (sink == NULL) return 0;
    for (i = 0u; i < sink->count; i++) {
        if (sink->entries[i].entity_id == entity_id) return 1;
    }
    return 0;
}

const char *asx_evidence_verdict_str(asx_evidence_level verdict) {
    switch (verdict) {
    case ASX_EVIDENCE_PASS: return "PASS";
    case ASX_EVIDENCE_WARN: return "WARN";
    case ASX_EVIDENCE_FAIL: return "FAIL";
    case ASX_EVIDENCE_INFO: return "INFO";
    }
    return "UNKNOWN";
}
