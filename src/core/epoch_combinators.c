/*
 * epoch_combinators.c — epoch-scoped combinator execution
 *
 * Wraps join/race/select/bulkhead/circuit-breaker operations within
 * an epoch phase. The epoch advances after the combinator completes.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/core/circuit_breaker.h>
#include <asx/core/combinator.h>
#include <asx/core/epoch.h>
#include <string.h>

asx_status asx_epoch_join(asx_epoch_handle epoch, asx_combinator_poll_fn *poll_fns,
                          void **user_datas, uint32_t count, asx_task_id self) {
    asx_join_state js;
    asx_status st;
    uint32_t i;

    if (poll_fns == NULL || count == 0u) return ASX_E_INVALID_ARGUMENT;
    if (asx_epoch_get_state(epoch) != ASX_EPOCH_ACTIVE) return ASX_E_INVALID_STATE;

    st = asx_join_init(&js);
    if (st != ASX_OK) return st;

    for (i = 0u; i < count && i < ASX_COMBINATOR_MAX_BRANCHES; i++) {
        /* ASX_CHECKPOINT_WAIVER("bounded init over static array") */
        st = asx_join_add(&js, poll_fns[i], user_datas ? user_datas[i] : NULL);
        if (st != ASX_OK) return st;
    }

    st = asx_join_poll(&js, self);
    if (st == ASX_OK) {
        asx_status advance_st = asx_epoch_advance(epoch);
        if (advance_st != ASX_OK) return advance_st;
    }
    return st;
}

asx_status asx_epoch_race(asx_epoch_handle epoch, asx_combinator_poll_fn *poll_fns,
                          void **user_datas, uint32_t count, asx_task_id self, int32_t *winner) {
    asx_race_state rs;
    asx_status st;
    uint32_t i;

    if (poll_fns == NULL || count == 0u) return ASX_E_INVALID_ARGUMENT;
    if (asx_epoch_get_state(epoch) != ASX_EPOCH_ACTIVE) return ASX_E_INVALID_STATE;

    st = asx_race_init(&rs);
    if (st != ASX_OK) return st;

    for (i = 0u; i < count && i < ASX_COMBINATOR_MAX_BRANCHES; i++) {
        /* ASX_CHECKPOINT_WAIVER("bounded init over static array") */
        st = asx_race_add(&rs, poll_fns[i], user_datas ? user_datas[i] : NULL);
        if (st != ASX_OK) return st;
    }

    st = asx_race_poll(&rs, self);
    if (st != ASX_E_PENDING) {
        asx_status advance_st;
        if (winner != NULL) *winner = asx_race_winner(&rs);
        advance_st = asx_epoch_advance(epoch);
        if (advance_st != ASX_OK) return advance_st;
    }
    return st;
}

asx_status asx_epoch_select(asx_epoch_handle epoch, asx_combinator_poll_fn *poll_fns,
                            void **user_datas, uint32_t count, asx_task_id self, int32_t *winner) {
    asx_select_state ss;
    asx_status st;
    uint32_t i;

    if (poll_fns == NULL || count == 0u) return ASX_E_INVALID_ARGUMENT;
    if (asx_epoch_get_state(epoch) != ASX_EPOCH_ACTIVE) return ASX_E_INVALID_STATE;

    st = asx_select_init(&ss);
    if (st != ASX_OK) return st;

    for (i = 0u; i < count && i < ASX_COMBINATOR_MAX_BRANCHES; i++) {
        /* ASX_CHECKPOINT_WAIVER("bounded init over static array") */
        st = asx_select_add(&ss, poll_fns[i], user_datas ? user_datas[i] : NULL);
        if (st != ASX_OK) return st;
    }

    st = asx_select_poll(&ss, self);
    if (st != ASX_E_PENDING) {
        asx_status advance_st;
        if (winner != NULL) *winner = asx_select_winner(&ss);
        advance_st = asx_epoch_advance(epoch);
        if (advance_st != ASX_OK) return advance_st;
    }
    return st;
}

asx_status asx_bulkhead_call_in_epoch(asx_epoch_handle epoch, asx_combinator_poll_fn poll_fn,
                                      void *user_data, asx_task_id self) {
    asx_status st;

    if (poll_fn == NULL) return ASX_E_INVALID_ARGUMENT;
    if (asx_epoch_get_state(epoch) != ASX_EPOCH_ACTIVE) return ASX_E_INVALID_STATE;

    /* Execute the poll function directly — bulkhead semantics are handled
     * by the caller's concurrency limiter wrapping the poll_fn. */
    st = poll_fn(user_data, self);
    if (st != ASX_E_PENDING) {
        asx_status advance_st = asx_epoch_advance(epoch);
        if (advance_st != ASX_OK) return advance_st;
    }
    return st;
}

asx_status asx_circuit_breaker_call_in_epoch(asx_epoch_handle epoch, asx_combinator_poll_fn poll_fn,
                                             void *user_data, asx_task_id self) {
    asx_status st;

    if (poll_fn == NULL) return ASX_E_INVALID_ARGUMENT;
    if (asx_epoch_get_state(epoch) != ASX_EPOCH_ACTIVE) return ASX_E_INVALID_STATE;

    /* Execute the poll function — circuit breaker state is managed by
     * the caller's asx_circuit_breaker wrapping the call. */
    st = poll_fn(user_data, self);
    if (st != ASX_E_PENDING) {
        asx_status advance_st = asx_epoch_advance(epoch);
        if (advance_st != ASX_OK) return advance_st;
    }
    return st;
}
