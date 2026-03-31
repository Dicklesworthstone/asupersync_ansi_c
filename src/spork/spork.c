/*
 * spork.c — spawn/fork orchestration primitives implementation
 *
 * Work pools, parallel map, scatter-gather, and pipeline composition.
 * Zero dynamic allocation.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/spork/spork.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Work item                                                           */
/* ------------------------------------------------------------------ */

void asx_work_item_init(asx_work_item *item, asx_work_fn fn, void *user_data, void *result) {
    if (item == NULL) { return; }
    item->fn = fn;
    item->user_data = user_data;
    item->result = result;
    item->status = ASX_OK;
    item->completed = 0;
}

asx_status asx_work_item_execute(asx_work_item *item) {
    if (item == NULL || item->fn == NULL) { return ASX_E_INVALID_ARGUMENT; }
    item->status = item->fn(item->user_data, item->result);
    item->completed = 1;
    return item->status;
}

/* ------------------------------------------------------------------ */
/* Work pool                                                           */
/* ------------------------------------------------------------------ */

void asx_work_pool_init(asx_work_pool *pool) {
    if (pool == NULL) { return; }
    memset(pool, 0, sizeof(*pool));
}

asx_status asx_work_pool_add(asx_work_pool *pool, asx_work_fn fn, void *user_data, void *result) {
    if (pool == NULL || fn == NULL) { return ASX_E_INVALID_ARGUMENT; }
    if (pool->count >= ASX_WORK_POOL_MAX_ITEMS) { return ASX_E_RESOURCE_EXHAUSTED; }

    asx_work_item_init(&pool->items[pool->count], fn, user_data, result);
    pool->count++;
    return ASX_OK;
}

asx_status asx_work_pool_execute_all(asx_work_pool *pool) {
    uint32_t i;
    asx_status first_error = ASX_OK;

    if (pool == NULL) { return ASX_E_INVALID_ARGUMENT; }

    for (i = 0; i < pool->count; i++) {
        asx_status st = asx_work_item_execute(&pool->items[i]);
        if (st == ASX_OK) {
            pool->completed_count++;
        } else {
            pool->failed_count++;
            if (first_error == ASX_OK) { first_error = st; }
        }
    }
    return first_error;
}

uint32_t asx_work_pool_completed(const asx_work_pool *pool) {
    if (pool == NULL) { return 0u; }
    return pool->completed_count;
}

uint32_t asx_work_pool_failed(const asx_work_pool *pool) {
    if (pool == NULL) { return 0u; }
    return pool->failed_count;
}

uint32_t asx_work_pool_count(const asx_work_pool *pool) {
    if (pool == NULL) { return 0u; }
    return pool->count;
}

/* ------------------------------------------------------------------ */
/* Parallel map                                                        */
/* ------------------------------------------------------------------ */

void asx_parallel_map_init(asx_parallel_map_state *state, asx_map_fn fn, void *user_data,
                           const void *inputs, void *outputs, size_t item_size_in,
                           size_t item_size_out, size_t count) {
    if (state == NULL) { return; }
    state->fn = fn;
    state->user_data = user_data;
    state->inputs = inputs;
    state->outputs = outputs;
    state->item_size_in = item_size_in;
    state->item_size_out = item_size_out;
    state->count = count;
    state->completed = 0;
    state->first_error = ASX_OK;
}

asx_status asx_parallel_map_execute(asx_parallel_map_state *state) {
    size_t i;

    if (state == NULL || state->fn == NULL) { return ASX_E_INVALID_ARGUMENT; }
    if (state->count > 0u && (state->inputs == NULL || state->outputs == NULL ||
                              state->item_size_in == 0u || state->item_size_out == 0u)) {
        return ASX_E_INVALID_ARGUMENT;
    }

    for (i = 0; i < state->count; i++) {
        const void *in = (const char *)state->inputs + i * state->item_size_in;
        void *out = (char *)state->outputs + i * state->item_size_out;
        asx_status st = state->fn(in, out, state->user_data);

        state->completed++;
        if (st != ASX_OK && state->first_error == ASX_OK) { state->first_error = st; }
    }
    return state->first_error;
}

size_t asx_parallel_map_completed(const asx_parallel_map_state *state) {
    if (state == NULL) { return 0u; }
    return state->completed;
}

/* ------------------------------------------------------------------ */
/* Scatter-gather                                                      */
/* ------------------------------------------------------------------ */

void asx_scatter_gather_init(asx_scatter_gather *sg) {
    if (sg == NULL) { return; }
    memset(sg, 0, sizeof(*sg));
    sg->aggregate_status = ASX_OK;
}

asx_status asx_scatter_gather_add(asx_scatter_gather *sg, asx_work_fn fn, void *user_data,
                                  void *result) {
    if (sg == NULL || fn == NULL) { return ASX_E_INVALID_ARGUMENT; }
    if (sg->count >= ASX_SCATTER_GATHER_MAX_ITEMS) { return ASX_E_RESOURCE_EXHAUSTED; }

    {
        asx_scatter_item *item = &sg->items[sg->count++];
        item->fn = fn;
        item->user_data = user_data;
        item->result = result;
        item->status = ASX_OK;
        item->completed = 0;
    }
    return ASX_OK;
}

asx_status asx_scatter_gather_execute(asx_scatter_gather *sg) {
    uint32_t i;

    if (sg == NULL) { return ASX_E_INVALID_ARGUMENT; }

    for (i = 0; i < sg->count; i++) {
        asx_scatter_item *item = &sg->items[i];
        item->status = item->fn(item->user_data, item->result);
        item->completed = 1;
        sg->gathered++;

        if (item->status != ASX_OK && sg->aggregate_status == ASX_OK) {
            sg->aggregate_status = item->status;
        }
    }
    return sg->aggregate_status;
}

uint32_t asx_scatter_gather_gathered(const asx_scatter_gather *sg) {
    if (sg == NULL) { return 0u; }
    return sg->gathered;
}

/* ------------------------------------------------------------------ */
/* Pipeline                                                            */
/* ------------------------------------------------------------------ */

void asx_spork_pipeline_init(asx_spork_pipeline *pipe) {
    if (pipe == NULL) { return; }
    memset(pipe, 0, sizeof(*pipe));
}

asx_status asx_spork_pipeline_add_stage(asx_spork_pipeline *pipe, asx_spork_stage_fn fn,
                                        void *user_data) {
    if (pipe == NULL || fn == NULL) { return ASX_E_INVALID_ARGUMENT; }
    if (pipe->stage_count >= ASX_SPORK_PIPELINE_MAX_STAGES) { return ASX_E_RESOURCE_EXHAUSTED; }

    pipe->stages[pipe->stage_count].fn = fn;
    pipe->stages[pipe->stage_count].user_data = user_data;
    pipe->stage_count++;
    return ASX_OK;
}

asx_status asx_spork_pipeline_execute(const asx_spork_pipeline *pipe, void *initial_input,
                                      void *final_output, void *scratch) {
    uint32_t i;
    void *current_input = initial_input;
    void *current_output;

    if (pipe == NULL || final_output == NULL) { return ASX_E_INVALID_ARGUMENT; }
    if (pipe->stage_count == 0u) { return ASX_E_INVALID_ARGUMENT; }
    if (pipe->stage_count > 1u && scratch == NULL) { return ASX_E_INVALID_ARGUMENT; }

    for (i = 0; i < pipe->stage_count; i++) {
        asx_status st;

        /* Last stage writes to final_output; intermediate stages use scratch. */
        if (i == pipe->stage_count - 1) {
            current_output = final_output;
        } else {
            current_output = scratch;
        }

        if (pipe->stages[i].fn == NULL) { return ASX_E_INVALID_ARGUMENT; }
        st = pipe->stages[i].fn(current_input, current_output, pipe->stages[i].user_data);
        if (st != ASX_OK) { return st; }

        /* For next stage, input is the previous output. */
        current_input = current_output;
    }

    return ASX_OK;
}

uint32_t asx_spork_pipeline_stage_count(const asx_spork_pipeline *pipe) {
    if (pipe == NULL) { return 0u; }
    return pipe->stage_count;
}
