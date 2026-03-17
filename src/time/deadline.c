/*
 * deadline.c — deadline abstraction with timer-wheel integration
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx_config.h>
#include <asx/time/deadline.h>
#include <stdint.h>
#include <string.h>

/* -------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------- */

asx_status asx_deadline_init(asx_deadline *dl, asx_time target_ns) {
    if (dl == NULL) return ASX_E_INVALID_ARGUMENT;

    dl->target_ns = target_ns;
    memset(&dl->timer, 0, sizeof(dl->timer));
    dl->registered = 0;
    dl->expired = 0;
    return ASX_OK;
}

asx_status asx_deadline_after(asx_deadline *dl, uint64_t duration_ns) {
    asx_time now;
    asx_status st;

    if (dl == NULL) return ASX_E_INVALID_ARGUMENT;

    st = asx_runtime_now_ns(&now);
    if (st != ASX_OK) return st;
    if (duration_ns > UINT64_MAX - now) return ASX_E_TIMER_DURATION_EXCEEDED;

    return asx_deadline_init(dl, now + duration_ns);
}

asx_status asx_deadline_arm(asx_deadline *dl, void *waker_data) {
    if (dl == NULL) return ASX_E_INVALID_ARGUMENT;
    if (dl->registered) return ASX_E_INVALID_STATE;

    {
        asx_status st;
        st = asx_timer_register(asx_timer_wheel_global(), dl->target_ns, waker_data, &dl->timer);
        if (st != ASX_OK) return st;
    }

    dl->registered = 1;
    return ASX_OK;
}

asx_status asx_deadline_disarm(asx_deadline *dl) {
    if (dl == NULL) return ASX_E_INVALID_ARGUMENT;

    if (dl->registered) {
        (void)asx_timer_cancel(asx_timer_wheel_global(), &dl->timer);
        dl->registered = 0;
    }
    return ASX_OK;
}

/* -------------------------------------------------------------------
 * Queries
 * ------------------------------------------------------------------- */

int asx_deadline_is_expired(asx_deadline *dl) {
    asx_time now;

    if (dl == NULL) return 0;
    if (dl->expired) return 1;

    if (asx_runtime_now_ns(&now) != ASX_OK) return 0;

    return asx_deadline_is_expired_at(dl, now);
}

int asx_deadline_is_expired_at(asx_deadline *dl, asx_time now) {
    if (dl == NULL) return 0;
    if (dl->expired) return 1;

    if (now >= dl->target_ns) {
        dl->expired = 1;
        return 1;
    }
    return 0;
}

uint64_t asx_deadline_remaining_ns(const asx_deadline *dl, asx_time now) {
    if (dl == NULL) return 0;
    if (now >= dl->target_ns) return 0;
    return dl->target_ns - now;
}

asx_time asx_deadline_target(const asx_deadline *dl) {
    if (dl == NULL) return 0;
    return dl->target_ns;
}

int asx_deadline_is_armed(const asx_deadline *dl) {
    if (dl == NULL) return 0;
    return dl->registered;
}
