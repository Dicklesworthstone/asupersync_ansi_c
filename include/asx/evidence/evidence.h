/*
 * asx/evidence/evidence.h — explicit evidence-family helpers
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_EVIDENCE_EVIDENCE_H
#define ASX_EVIDENCE_EVIDENCE_H

#include <asx/runtime/diagnostic.h>

#ifdef __cplusplus
extern "C" {
#endif

ASX_API const char *asx_evidence_level_str(asx_evidence_level level);

ASX_API int asx_evidence_entry_has_entity(const asx_evidence_entry *entry);

ASX_API int asx_evidence_is_failure(const asx_evidence_entry *entry);

#ifdef __cplusplus
}
#endif

#endif /* ASX_EVIDENCE_EVIDENCE_H */
