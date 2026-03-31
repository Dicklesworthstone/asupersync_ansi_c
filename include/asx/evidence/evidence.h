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

/* Return the human-readable string for an evidence level. */
ASX_API const char *asx_evidence_level_str(asx_evidence_level level);

/* Check if an evidence entry has an associated entity ID. */
ASX_API int asx_evidence_entry_has_entity(const asx_evidence_entry *entry);

/* Check if an evidence entry is a failure. */
ASX_API int asx_evidence_is_failure(const asx_evidence_entry *entry);

/* Check if an evidence entry is a warning. */
ASX_API int asx_evidence_is_warning(const asx_evidence_entry *entry);

/* Check if an evidence entry is passing. */
ASX_API int asx_evidence_is_pass(const asx_evidence_entry *entry);

/* Count entries by level in a sink. */
ASX_API uint32_t asx_evidence_count_by_level(const asx_evidence_sink *sink,
                                             asx_evidence_level level);

/* Count entries for a specific source prefix (e.g., "monitor:"). */
ASX_API uint32_t asx_evidence_count_for_source(const asx_evidence_sink *sink,
                                               const char *source_prefix);

/* Get the first failure entry, or NULL if no failures. */
ASX_API const asx_evidence_entry *asx_evidence_first_failure(const asx_evidence_sink *sink);

/* Check if the sink contains any entries for a given entity ID. */
ASX_API int asx_evidence_has_entity(const asx_evidence_sink *sink, uint64_t entity_id);

/* Get the overall pass/fail/warn summary as a human-readable string. */
ASX_API const char *asx_evidence_verdict_str(asx_evidence_level verdict);

#ifdef __cplusplus
}
#endif

#endif /* ASX_EVIDENCE_EVIDENCE_H */
