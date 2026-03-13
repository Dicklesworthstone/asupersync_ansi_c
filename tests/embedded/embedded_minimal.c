/*
 * embedded_minimal.c — Narrow down embedded test failures
 */
#include <asx/asx.h>

static asx_status noop_poll(void *user_data, asx_task_id self) {
    (void)user_data;
    (void)self;
    return ASX_OK;
}

static volatile int g_stage;

int main(void) {
    asx_region_id rid;
    asx_task_id tid;
    asx_status st;
    asx_budget budget;

    g_stage = 1;
    asx_runtime_reset();

    g_stage = 2;
    st = asx_region_open(&rid);
    if (st != ASX_OK) return 1;

    g_stage = 3;
    st = asx_task_spawn(rid, noop_poll, (void *)0, &tid);
    if (st != ASX_OK) return 1;

    g_stage = 4;
    budget = asx_budget_infinite();
    st = asx_scheduler_run(rid, &budget);
    if (st != ASX_OK) return 1;

    g_stage = 5;
    asx_task_state tstate;
    st = asx_task_get_state(tid, &tstate);
    if (st != ASX_OK) return 1;
    if (tstate != ASX_TASK_COMPLETED) return 1;

    g_stage = 6;
    budget = asx_budget_infinite();
    st = asx_region_drain(rid, &budget);
    if (st != ASX_OK) return 1;

    g_stage = 7;
    st = asx_quiescence_check(rid);
    if (st != ASX_OK) return 1;

    g_stage = 100;
    return 0;
}
