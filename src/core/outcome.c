/*
 * outcome.c — outcome severity lattice implementation
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/core/outcome.h>
#include <string.h>

asx_outcome_severity asx_outcome_severity_of(const asx_outcome *o) {
    if (!o) return ASX_OUTCOME_OK;
    return o->severity;
}

asx_outcome asx_outcome_join(const asx_outcome *a, const asx_outcome *b) {
    asx_outcome result;
    memset(&result, 0, sizeof(result));
    if (!a && !b) {
        result.severity = ASX_OUTCOME_OK;
    } else if (!a) {
        result = *b;
    } else if (!b) {
        result = *a;
    } else if (a->severity >= b->severity) {
        result = *a; /* left-bias on equal severity */
        /* Rust strengthens the left reason with the right one only when
         * the right kind is strictly more severe (types/outcome.rs:
         * 568-573). */
        if (a->severity == ASX_OUTCOME_CANCELLED && b->severity == ASX_OUTCOME_CANCELLED &&
            asx_cancel_severity(b->cancel_reason.kind) >
                asx_cancel_severity(a->cancel_reason.kind)) {
            result.cancel_reason = asx_cancel_strengthen(&a->cancel_reason, &b->cancel_reason);
        }
    } else {
        result = *b;
    }
    return result;
}
