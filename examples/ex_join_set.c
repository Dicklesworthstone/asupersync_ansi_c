/*
 * ex_join_set.c — JoinSet dynamic task collection example
 *
 * Demonstrates spawning multiple tasks into a JoinSet and polling
 * for completions in completion order.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <stdio.h>

static uint32_t g_poll_count;

static asx_status counting_poll(void *user_data, asx_task_id self) {
    uint32_t *target = (uint32_t *)user_data;
    (void)self;
    g_poll_count++;
    if (g_poll_count >= *target) return ASX_OK;
    return ASX_E_PENDING;
}

int main(void) {
    asx_runtime rt;
    asx_region_id region;
    asx_join_set js;
    asx_join_set_result result;
    asx_budget budget;
    asx_task_id tasks[3];
    uint32_t targets[3] = {1, 3, 2};
    uint32_t i;
    asx_status st;

    printf("SCENARIO join_set_example\n");

    st = asx_runtime_init_default(&rt);
    if (st != ASX_OK) {
        printf("SCENARIO join_set_example fail runtime_init\n");
        return 1;
    }

    st = asx_region_open(&region);
    if (st != ASX_OK) {
        printf("SCENARIO join_set_example fail region_open\n");
        return 1;
    }

    /* Spawn 3 tasks with different poll counts to complete */
    asx_join_set_init(&js);
    for (i = 0; i < 3; i++) {
        g_poll_count = 0;
        st = asx_task_spawn(region, counting_poll, &targets[i], &tasks[i]);
        if (st != ASX_OK) {
            printf("SCENARIO join_set_example fail spawn_%u\n", i);
            return 1;
        }
        asx_join_set_add(&js, tasks[i]);
    }

    printf("SCENARIO join_set_spawned pass\n");

    /* Run scheduler and poll for completions */
    budget = asx_budget_from_polls(100);
    st = asx_scheduler_run(region, &budget);

    while (!asx_join_set_is_empty(&js)) {
        st = asx_join_set_poll_next(&js, &result);
        if (st == ASX_OK) {
            printf("SCENARIO join_set_completed_%u pass\n", result.index);
        } else if (st == ASX_E_PENDING) {
            budget = asx_budget_from_polls(50);
            asx_scheduler_run(region, &budget);
        } else {
            break;
        }
    }

    printf("SCENARIO join_set_all_done %s\n",
           asx_join_set_is_empty(&js) ? "pass" : "fail");

    asx_runtime_shutdown(&rt);
    return 0;
}
