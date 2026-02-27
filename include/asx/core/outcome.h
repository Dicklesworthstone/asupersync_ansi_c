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
#include <asx/asx_status.h>

/* Outcome severity levels (total order) */
typedef enum {
    ASX_OUTCOME_OK        = 0,
    ASX_OUTCOME_ERR       = 1,
    ASX_OUTCOME_CANCELLED = 2,
    ASX_OUTCOME_PANICKED  = 3
} asx_outcome_severity;

/* Opaque outcome type holding severity + payload reference */
typedef struct asx_outcome asx_outcome;

/* Return the severity of an outcome */
ASX_API asx_outcome_severity asx_outcome_severity_of(const asx_outcome *o);

/* Join two outcomes: max severity wins, left-bias on equal */
ASX_API asx_outcome asx_outcome_join(const asx_outcome *a, const asx_outcome *b);

#endif /* ASX_CORE_OUTCOME_H */
