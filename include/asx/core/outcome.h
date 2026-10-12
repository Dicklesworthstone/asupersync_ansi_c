/*
 * asx/core/outcome.h — outcome severity lattice
 *
 * Outcome semantics: Ok < Err < Cancelled < Panicked
 * Join operator returns the operand with greater severity.
 * Left-bias on equal severity.
 *
 * An outcome carries the payload of the variants that have one, as Rust's
 * Outcome does (types/outcome.rs): Cancelled(CancelReason) and
 * Panicked(PanicPayload). Ok and Err carry none here; an Err's status is
 * read where it is produced (asx_task_get_error).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_CORE_OUTCOME_H
#define ASX_CORE_OUTCOME_H

#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>
#include <asx/core/cancel.h>
#include <string.h>

/* asx_outcome_severity enum is defined in asx_ids.h. */

/* Outcome type: severity plus the payload of its variant.
 *
 * `cancel_reason` is meaningful only when `severity` is CANCELLED and is
 * zero otherwise. `panic_message` is meaningful only when `severity` is
 * PANICKED and is NULL otherwise: Rust's PanicPayload is a message, and
 * here it is borrowed (a string literal, or storage the caller of
 * asx_task_panic keeps alive), never copied, owned or freed by the runtime. */
typedef struct asx_outcome {
    asx_outcome_severity severity;
    asx_cancel_reason cancel_reason;
    const char *panic_message;
} asx_outcome;

/* Construct a payload-free outcome with the given severity: a CANCELLED one
 * carries the zero reason (USER, no origin, time 0) and a PANICKED one no
 * message. The runtime builds its outcomes with their payload; this
 * constructor is for the lattice alone. */
static inline asx_outcome asx_outcome_make(asx_outcome_severity s) {
    asx_outcome o;
    memset(&o, 0, sizeof(o));
    o.severity = s;
    return o;
}

/* Rust Outcome::Cancelled(reason). A NULL reason is the zero reason of
 * asx_outcome_make. */
static inline asx_outcome asx_outcome_cancelled(const asx_cancel_reason *reason) {
    asx_outcome o = asx_outcome_make(ASX_OUTCOME_CANCELLED);
    if (reason != NULL) o.cancel_reason = *reason;
    return o;
}

/* Rust Outcome::Panicked(PanicPayload::new(message)). A NULL message is
 * reported as "unknown panic", as Rust reports a non-string payload
 * (cx/scope.rs:181-187) and asx_task_panic does. */
static inline asx_outcome asx_outcome_panicked(const char *message) {
    asx_outcome o = asx_outcome_make(ASX_OUTCOME_PANICKED);
    o.panic_message = message != NULL ? message : "unknown panic";
    return o;
}

/* Return the severity of an outcome */
ASX_API asx_outcome_severity asx_outcome_severity_of(const asx_outcome *o);

/* Join two outcomes (Rust Outcome::join, types/outcome.rs:566-581): max
 * severity wins, left-bias on equal severity, the winner's payload whole.
 * Two Cancelled outcomes keep the left reason unless the right one's kind
 * is strictly more severe, which strengthens it (asx_cancel_strengthen):
 * a tie in kind severity stays left, whatever the timestamps say. */
ASX_API asx_outcome asx_outcome_join(const asx_outcome *a, const asx_outcome *b);

#endif /* ASX_CORE_OUTCOME_H */
