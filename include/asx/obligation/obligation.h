/*
 * asx/obligation/obligation.h — public obligation helpers
 *
 * Packages the runtime obligation lifecycle into a dedicated public
 * module family so consumers do not need to treat obligations as an
 * incidental side-effect of the runtime header.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_OBLIGATION_OBLIGATION_H
#define ASX_OBLIGATION_OBLIGATION_H

#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>
#include <asx/runtime/runtime.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    asx_obligation_id id;
    asx_obligation_state state;
} asx_obligation_record;

static inline ASX_MUST_USE asx_status asx_obligation_open(asx_region_id region,
                                                          asx_obligation_id *out_id) {
    return asx_obligation_reserve(region, out_id);
}

static inline ASX_MUST_USE asx_status asx_obligation_resolve_commit(asx_obligation_id id) {
    return asx_obligation_commit(id);
}

static inline ASX_MUST_USE asx_status asx_obligation_resolve_abort(asx_obligation_id id) {
    return asx_obligation_abort(id);
}

static inline ASX_MUST_USE asx_status asx_obligation_capture(asx_obligation_id id,
                                                             asx_obligation_record *out_record) {
    asx_status st;

    if (out_record == NULL) return ASX_E_INVALID_ARGUMENT;
    out_record->id = id;
    st = asx_obligation_get_state(id, &out_record->state);
    if (st != ASX_OK) {
        out_record->id = ASX_INVALID_ID;
        return st;
    }
    return ASX_OK;
}

static inline int asx_obligation_is_resolved(asx_obligation_state state) {
    return state == ASX_OBLIGATION_COMMITTED || state == ASX_OBLIGATION_ABORTED ||
           state == ASX_OBLIGATION_LEAKED;
}

static inline const char *asx_obligation_state_name(asx_obligation_state state) {
    switch (state) {
    case ASX_OBLIGATION_RESERVED: return "reserved";
    case ASX_OBLIGATION_COMMITTED: return "committed";
    case ASX_OBLIGATION_ABORTED: return "aborted";
    case ASX_OBLIGATION_LEAKED: return "leaked";
    default: return "unknown";
    }
}

#ifdef __cplusplus
}
#endif

#endif /* ASX_OBLIGATION_OBLIGATION_H */
