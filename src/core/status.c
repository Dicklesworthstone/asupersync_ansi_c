/*
 * status.c — status code string mapping
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>

typedef struct asx_task_ledger {
    asx_task_id            owner;
    uint32_t               used;
    uint32_t               write_index;
    uint32_t               next_sequence;
    int                    overflowed;
    asx_error_ledger_entry entries[ASX_ERROR_LEDGER_DEPTH];
} asx_task_ledger;

static asx_task_ledger g_task_ledgers[ASX_ERROR_LEDGER_TASK_SLOTS];
static asx_task_ledger g_fallback_ledger;
static asx_task_id     g_bound_task = ASX_INVALID_ID;

static const char *g_must_use_surfaces[] = {
    "asx_region_transition_check",
    "asx_task_transition_check",
    "asx_obligation_transition_check",
    "asx_region_slot_lookup",
    "asx_task_slot_lookup",
    "asx_region_open",
    "asx_region_close",
    "asx_region_get_state",
    "asx_task_spawn",
    "asx_task_get_state",
    "asx_task_get_outcome",
    "asx_scheduler_run",
    "asx_quiescence_check",
    "asx_region_drain",
    "asx_cleanup_push",
    "asx_cleanup_pop",
    "asx_error_ledger_get"
};

static void asx_task_ledger_clear(asx_task_ledger *ledger, asx_task_id owner)
{
    uint32_t i;

    ledger->owner = owner;
    ledger->used = 0;
    ledger->write_index = 0;
    ledger->next_sequence = 0;
    ledger->overflowed = 0;
    for (i = 0; i < ASX_ERROR_LEDGER_DEPTH; i++) {
        ledger->entries[i].task_id = owner;
        ledger->entries[i].status = ASX_OK;
        ledger->entries[i].operation = "";
        ledger->entries[i].file = "";
        ledger->entries[i].line = 0;
        ledger->entries[i].sequence = 0;
    }
}

static int asx_error_ledger_is_task_id(asx_task_id task_id)
{
    return asx_handle_is_valid(task_id) && asx_handle_type_tag(task_id) == ASX_TYPE_TASK;
}

static asx_task_ledger *asx_error_ledger_slot_for_task(asx_task_id task_id)
{
    uint16_t slot;

    if (!asx_error_ledger_is_task_id(task_id)) {
        return &g_fallback_ledger;
    }

    slot = asx_handle_slot(task_id);
    if (slot >= ASX_ERROR_LEDGER_TASK_SLOTS) {
        return &g_fallback_ledger;
    }
    return &g_task_ledgers[slot];
}

static const asx_task_ledger *asx_error_ledger_readonly(asx_task_id task_id)
{
    const asx_task_ledger *ledger;

    ledger = asx_error_ledger_slot_for_task(task_id);
    if (ledger->owner != task_id) {
        return NULL;
    }
    return ledger;
}

static void asx_error_ledger_record_impl(asx_task_id task_id,
                                         asx_status status,
                                         const char *operation,
                                         const char *file,
                                         uint32_t line)
{
    asx_task_ledger *ledger;
    uint32_t index;

    if (status == ASX_OK) {
        return;
    }

    ledger = asx_error_ledger_slot_for_task(task_id);
    if (ledger->owner != task_id) {
        asx_task_ledger_clear(ledger, task_id);
    }

    index = ledger->write_index;
    ledger->entries[index].task_id = task_id;
    ledger->entries[index].status = status;
    ledger->entries[index].operation = (operation != NULL) ? operation : "";
    ledger->entries[index].file = (file != NULL) ? file : "";
    ledger->entries[index].line = line;
    ledger->entries[index].sequence = ledger->next_sequence++;

    ledger->write_index = (index + 1u) % ASX_ERROR_LEDGER_DEPTH;
    if (ledger->used < ASX_ERROR_LEDGER_DEPTH) {
        ledger->used++;
    } else {
        ledger->overflowed = 1;
    }
}

const char *asx_status_str(asx_status s)
{
    switch (s) {
    case ASX_OK:                          return "OK";
    case ASX_E_PENDING:                   return "pending";
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
    case ASX_E_REGION_POISONED:           return "region poisoned";
    case ASX_E_TASK_NOT_FOUND:            return "task not found";
    case ASX_E_SCHEDULER_UNAVAILABLE:     return "scheduler unavailable";
    case ASX_E_NAME_CONFLICT:             return "name conflict";
    case ASX_E_TASK_NOT_COMPLETED:        return "task not completed";
    case ASX_E_POLL_BUDGET_EXHAUSTED:     return "poll budget exhausted";
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
    case ASX_E_QUIESCENCE_NOT_REACHED:    return "quiescence not reached";
    case ASX_E_QUIESCENCE_TASKS_LIVE:     return "quiescence tasks still live";
    case ASX_E_RESOURCE_EXHAUSTED:        return "resource exhausted";
    case ASX_E_STALE_HANDLE:              return "stale handle";
    case ASX_E_HOOK_MISSING:              return "required runtime hook missing";
    case ASX_E_HOOK_INVALID:              return "runtime hook contract invalid";
    case ASX_E_DETERMINISM_VIOLATION:     return "deterministic mode hook violation";
    case ASX_E_ALLOCATOR_SEALED:          return "allocator is sealed";
    case ASX_E_AFFINITY_VIOLATION:        return "affinity domain violation";
    case ASX_E_AFFINITY_NOT_BOUND:        return "entity not bound to affinity domain";
    case ASX_E_AFFINITY_ALREADY_BOUND:    return "entity already bound to different domain";
    case ASX_E_AFFINITY_TRANSFER_REQUIRED: return "cross-domain transfer required";
    case ASX_E_AFFINITY_TABLE_FULL:       return "affinity tracking table full";
    case ASX_E_EQUIVALENCE_MISMATCH:     return "cross-codec semantic equivalence mismatch";
    default:                              return "unknown status";
    }
}

void asx_error_ledger_reset(void)
{
    uint32_t i;

    for (i = 0; i < ASX_ERROR_LEDGER_TASK_SLOTS; i++) {
        asx_task_ledger_clear(&g_task_ledgers[i], ASX_INVALID_ID);
    }
    asx_task_ledger_clear(&g_fallback_ledger, ASX_INVALID_ID);
    g_bound_task = ASX_INVALID_ID;
}

void asx_error_ledger_bind_task(asx_task_id task_id)
{
    g_bound_task = task_id;
}

asx_task_id asx_error_ledger_bound_task(void)
{
    return g_bound_task;
}

void asx_error_ledger_record_current(asx_status status,
                                     const char *operation,
                                     const char *file,
                                     uint32_t line)
{
    asx_error_ledger_record_impl(g_bound_task, status, operation, file, line);
}

void asx_error_ledger_record_for_task(asx_task_id task_id,
                                      asx_status status,
                                      const char *operation,
                                      const char *file,
                                      uint32_t line)
{
    asx_error_ledger_record_impl(task_id, status, operation, file, line);
}

uint32_t asx_error_ledger_count(asx_task_id task_id)
{
    const asx_task_ledger *ledger;

    ledger = asx_error_ledger_readonly(task_id);
    if (ledger == NULL) {
        return 0;
    }
    return ledger->used;
}

int asx_error_ledger_overflowed(asx_task_id task_id)
{
    const asx_task_ledger *ledger;

    ledger = asx_error_ledger_readonly(task_id);
    if (ledger == NULL) {
        return 0;
    }
    return ledger->overflowed;
}

int asx_error_ledger_get(asx_task_id task_id,
                         uint32_t index,
                         asx_error_ledger_entry *out_entry)
{
    const asx_task_ledger *ledger;
    uint32_t oldest;
    uint32_t logical_index;

    if (out_entry == NULL) {
        return 0;
    }

    ledger = asx_error_ledger_readonly(task_id);
    if (ledger == NULL || index >= ledger->used) {
        return 0;
    }

    oldest = (ledger->used < ASX_ERROR_LEDGER_DEPTH)
        ? 0u
        : ledger->write_index;
    logical_index = (oldest + index) % ASX_ERROR_LEDGER_DEPTH;
    *out_entry = ledger->entries[logical_index];
    return 1;
}

uint32_t asx_must_use_surface_count(void)
{
    return (uint32_t)(sizeof(g_must_use_surfaces) / sizeof(g_must_use_surfaces[0]));
}

const char *asx_must_use_surface_name(uint32_t index)
{
    if (index >= asx_must_use_surface_count()) {
        return NULL;
    }
    return g_must_use_surfaces[index];
}
