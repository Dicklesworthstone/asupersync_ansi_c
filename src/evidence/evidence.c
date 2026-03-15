/*
 * evidence.c — explicit evidence-family helpers
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/evidence/evidence.h>

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
