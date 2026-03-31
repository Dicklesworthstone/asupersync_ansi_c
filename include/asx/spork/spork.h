/*
 * asx/spork/spork.h — spawn/fork orchestration primitives
 *
 * Provides operator-oriented task orchestration: work pools, parallel
 * map, scatter-gather, and pipeline composition patterns.
 *
 * Design:
 *   - All types are value types with inline storage (no heap allocation).
 *   - Orchestration uses the asx_service/asx_combinator infrastructure.
 *   - Work items are (function pointer, user_data) pairs.
 *   - Results are collected in fixed-size arrays.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_SPORK_SPORK_H
#define ASX_SPORK_SPORK_H

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Work item — a single unit of schedulable work                       */
/* ------------------------------------------------------------------ */

typedef asx_status (*asx_work_fn)(void *user_data, void *result);

typedef struct {
    asx_work_fn fn;
    void *user_data;
    void *result;      /* pointer to result storage (caller-provided) */
    asx_status status; /* outcome status */
    int completed;     /* nonzero when work is done */
} asx_work_item;

/* Initialize a work item. result must point to caller-owned storage. */
ASX_API void asx_work_item_init(asx_work_item *item, asx_work_fn fn, void *user_data, void *result);

/* Execute a work item synchronously. */
ASX_API ASX_MUST_USE asx_status asx_work_item_execute(asx_work_item *item);

/* ------------------------------------------------------------------ */
/* Work pool — bounded set of parallel work items                      */
/* ------------------------------------------------------------------ */

#ifndef ASX_WORK_POOL_MAX_ITEMS
#define ASX_WORK_POOL_MAX_ITEMS 16u
#endif

typedef struct {
    asx_work_item items[ASX_WORK_POOL_MAX_ITEMS];
    uint32_t count;
    uint32_t completed_count;
    uint32_t failed_count;
} asx_work_pool;

/* Initialize an empty work pool. */
ASX_API void asx_work_pool_init(asx_work_pool *pool);

/* Add a work item to the pool. */
ASX_API ASX_MUST_USE asx_status asx_work_pool_add(asx_work_pool *pool, asx_work_fn fn,
                                                  void *user_data, void *result);

/* Execute all work items in the pool sequentially.
 * Returns ASX_OK if all items succeed, or the first error encountered. */
ASX_API ASX_MUST_USE asx_status asx_work_pool_execute_all(asx_work_pool *pool);

/* Return the number of completed items. */
ASX_API uint32_t asx_work_pool_completed(const asx_work_pool *pool);

/* Return the number of failed items. */
ASX_API uint32_t asx_work_pool_failed(const asx_work_pool *pool);

/* Return the total number of items in the pool. */
ASX_API uint32_t asx_work_pool_count(const asx_work_pool *pool);

/* ------------------------------------------------------------------ */
/* Parallel map — apply a function to each element of an array         */
/* ------------------------------------------------------------------ */

typedef asx_status (*asx_map_fn)(const void *input, void *output, void *user_data);

typedef struct {
    asx_map_fn fn;
    void *user_data;
    const void *inputs;     /* pointer to input array */
    void *outputs;          /* pointer to output array */
    size_t item_size_in;    /* size of each input element */
    size_t item_size_out;   /* size of each output element */
    size_t count;           /* number of elements */
    size_t completed;       /* elements processed so far */
    asx_status first_error; /* first error encountered, or ASX_OK */
} asx_parallel_map_state;

/* Initialize a parallel map operation. */
ASX_API void asx_parallel_map_init(asx_parallel_map_state *state, asx_map_fn fn, void *user_data,
                                   const void *inputs, void *outputs, size_t item_size_in,
                                   size_t item_size_out, size_t count);

/* Execute the map operation (sequentially in the CORE profile).
 * Returns ASX_OK if all elements succeed. */
ASX_API ASX_MUST_USE asx_status asx_parallel_map_execute(asx_parallel_map_state *state);

/* Return the number of elements processed. */
ASX_API size_t asx_parallel_map_completed(const asx_parallel_map_state *state);

/* ------------------------------------------------------------------ */
/* Scatter-gather — distribute work and collect results                */
/* ------------------------------------------------------------------ */

#ifndef ASX_SCATTER_GATHER_MAX_ITEMS
#define ASX_SCATTER_GATHER_MAX_ITEMS 8u
#endif

typedef struct {
    asx_work_fn fn;
    void *user_data;
    void *result;
    asx_status status;
    int completed;
} asx_scatter_item;

typedef struct {
    asx_scatter_item items[ASX_SCATTER_GATHER_MAX_ITEMS];
    uint32_t count;
    uint32_t gathered;
    asx_status aggregate_status; /* ASX_OK if all succeed */
} asx_scatter_gather;

/* Initialize a scatter-gather operation. */
ASX_API void asx_scatter_gather_init(asx_scatter_gather *sg);

/* Add a work item to scatter. */
ASX_API ASX_MUST_USE asx_status asx_scatter_gather_add(asx_scatter_gather *sg, asx_work_fn fn,
                                                       void *user_data, void *result);

/* Execute all scatter items and gather results.
 * Returns ASX_OK if all succeed, or the first error. */
ASX_API ASX_MUST_USE asx_status asx_scatter_gather_execute(asx_scatter_gather *sg);

/* Return the number of gathered (completed) results. */
ASX_API uint32_t asx_scatter_gather_gathered(const asx_scatter_gather *sg);

/* ------------------------------------------------------------------ */
/* Spork pipeline — sequential stage composition                       */
/* (Prefixed to avoid collision with core combinator pipeline.)        */
/* ------------------------------------------------------------------ */

#ifndef ASX_SPORK_PIPELINE_MAX_STAGES
#define ASX_SPORK_PIPELINE_MAX_STAGES 8u
#endif

typedef asx_status (*asx_spork_stage_fn)(void *input, void *output, void *user_data);

typedef struct {
    asx_spork_stage_fn fn;
    void *user_data;
} asx_spork_stage;

typedef struct {
    asx_spork_stage stages[ASX_SPORK_PIPELINE_MAX_STAGES];
    uint32_t stage_count;
} asx_spork_pipeline;

/* Initialize an empty spork pipeline. */
ASX_API void asx_spork_pipeline_init(asx_spork_pipeline *pipe);

/* Add a stage to the spork pipeline. */
ASX_API ASX_MUST_USE asx_status asx_spork_pipeline_add_stage(asx_spork_pipeline *pipe,
                                                             asx_spork_stage_fn fn,
                                                             void *user_data);

/* Execute the pipeline, feeding output of each stage as input to the next.
 * initial_input is fed to the first stage.
 * final_output receives the output of the last stage.
 * scratch is a buffer used for intermediate results between stages.
 * Returns ASX_OK if all stages succeed, or the first error. */
ASX_API ASX_MUST_USE asx_status asx_spork_pipeline_execute(const asx_spork_pipeline *pipe,
                                                           void *initial_input, void *final_output,
                                                           void *scratch);

/* Return the number of stages. */
ASX_API uint32_t asx_spork_pipeline_stage_count(const asx_spork_pipeline *pipe);

#ifdef __cplusplus
}
#endif

#endif /* ASX_SPORK_SPORK_H */
