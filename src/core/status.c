/*
 * status.c — status code string mapping
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>

const char *asx_status_str(asx_status s) {
    switch (s) {
    case ASX_OK:                          return "OK";
    case ASX_E_INVALID_ARGUMENT:          return "invalid argument";
    case ASX_E_INVALID_STATE:             return "invalid state";
    case ASX_E_NOT_FOUND:                 return "not found";
    case ASX_E_ALREADY_EXISTS:            return "already exists";
    case ASX_E_INVALID_TRANSITION:        return "invalid state transition";
    case ASX_E_REGION_NOT_FOUND:          return "region not found";
    case ASX_E_REGION_CLOSED:             return "region closed";
    case ASX_E_REGION_AT_CAPACITY:        return "region at capacity";
    case ASX_E_REGION_NOT_OPEN:           return "region not open";
    case ASX_E_ADMISSION_CLOSED:          return "admission closed";
    case ASX_E_ADMISSION_LIMIT:           return "admission limit reached";
    case ASX_E_TASK_NOT_FOUND:            return "task not found";
    case ASX_E_SCHEDULER_UNAVAILABLE:     return "scheduler unavailable";
    case ASX_E_NAME_CONFLICT:             return "name conflict";
    case ASX_E_OBLIGATION_ALREADY_RESOLVED: return "obligation already resolved";
    case ASX_E_UNRESOLVED_OBLIGATIONS:    return "unresolved obligations";
    case ASX_E_CANCELLED:                 return "cancelled";
    case ASX_E_WITNESS_PHASE_REGRESSION:  return "cancel witness phase regression";
    case ASX_E_WITNESS_REASON_WEAKENED:   return "cancel witness reason weakened";
    case ASX_E_WITNESS_TASK_MISMATCH:     return "cancel witness task mismatch";
    case ASX_E_WITNESS_REGION_MISMATCH:   return "cancel witness region mismatch";
    case ASX_E_WITNESS_EPOCH_MISMATCH:    return "cancel witness epoch mismatch";
    case ASX_E_DISCONNECTED:              return "channel disconnected";
    case ASX_E_WOULD_BLOCK:               return "would block";
    case ASX_E_CHANNEL_FULL:              return "channel full";
    case ASX_E_CHANNEL_NOT_DRAINED:       return "channel not drained";
    case ASX_E_TIMER_NOT_FOUND:           return "timer not found";
    case ASX_E_TIMERS_PENDING:            return "timers pending";
    case ASX_E_TASKS_STILL_ACTIVE:        return "tasks still active";
    case ASX_E_OBLIGATIONS_UNRESOLVED:    return "obligations unresolved";
    case ASX_E_REGIONS_NOT_CLOSED:        return "regions not closed";
    case ASX_E_INCOMPLETE_CHILDREN:       return "incomplete children";
    case ASX_E_RESOURCE_EXHAUSTED:        return "resource exhausted";
    case ASX_E_STALE_HANDLE:              return "stale handle";
    default:                              return "unknown status";
    }
}
