/*
 * asx/core/outcome.h — outcome severity lattice
 *
 * Outcome semantics: Ok < Err < Cancelled < Panicked
 * Join operator returns the operand with greater severity.
 * Left-bias on equal severity.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_CORE_OUTCOME_H
#define ASX_CORE_OUTCOME_H

#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>

/* asx_outcome_severity enum is defined in asx_ids.h. */

/* Outcome type (severity + future payload) */
typedef struct asx_outcome {
    asx_outcome_severity severity;
    /* Payload union will be added by bd-hwb.1 */
} asx_outcome;

/* Construct an outcome with the given severity */
static inline asx_outcome asx_outcome_make(asx_outcome_severity s) {
    asx_outcome o;
    o.severity = s;
    return o;
}

/* Return the severity of an outcome */
ASX_API asx_outcome_severity asx_outcome_severity_of(const asx_outcome *o);

/* Join two outcomes: max severity wins, left-bias on equal */
ASX_API asx_outcome asx_outcome_join(const asx_outcome *a, const asx_outcome *b);

#endif /* ASX_CORE_OUTCOME_H */
