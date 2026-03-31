/*
 * asx/cx/cx.h — capability context and explicit authority flow
 *
 * Cx is the token that grants access to runtime capabilities:
 *   - Identity queries (region_id, task_id)
 *   - Cancellation status and checkpoints
 *   - Budget access and enforcement
 *   - Clock reads (deterministic or wall)
 *   - Entropy access
 *
 * All effectful operations flow through explicit Cx tokens.
 * This prevents ambient authority and enables:
 *   - Effect interception (production vs lab/test runtime)
 *   - Cancellation propagation through the task tree
 *   - Budget enforcement (deadlines, poll quotas)
 *   - Observability tied to task identity
 *
 * Cx does NOT use dynamic allocation. It is a value type that
 * borrows from the runtime's static tables.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_CX_CX_H
#define ASX_CX_CX_H

#include <asx/asx_config.h>
#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>
#include <asx/core/budget.h>
#include <asx/core/cancel.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Capability flags                                                    */
/*                                                                     */
/* Each bit gates access to a specific runtime capability.             */
/* A Cx with no flags set is inert — it can only query identity.       */
/* ------------------------------------------------------------------ */

typedef uint32_t asx_cap_flags;

#define ASX_CAP_NONE ((asx_cap_flags)0)
#define ASX_CAP_CANCEL_CHECK ((asx_cap_flags)(1u << 0))   /* can check cancellation */
#define ASX_CAP_CANCEL_REQUEST ((asx_cap_flags)(1u << 1)) /* can request cancellation */
#define ASX_CAP_BUDGET_READ ((asx_cap_flags)(1u << 2))    /* can read budget */
#define ASX_CAP_BUDGET_CONSUME ((asx_cap_flags)(1u << 3)) /* can consume budget */
#define ASX_CAP_CLOCK_READ ((asx_cap_flags)(1u << 4))     /* can read clock */
#define ASX_CAP_ENTROPY ((asx_cap_flags)(1u << 5))        /* can draw entropy */
#define ASX_CAP_SPAWN ((asx_cap_flags)(1u << 6))          /* can spawn tasks */
#define ASX_CAP_CHANNEL ((asx_cap_flags)(1u << 7))        /* can use channels */
#define ASX_CAP_TIMER ((asx_cap_flags)(1u << 8))          /* can register timers */
#define ASX_CAP_OBLIGATION ((asx_cap_flags)(1u << 9))     /* can create obligations */
#define ASX_CAP_TRACE ((asx_cap_flags)(1u << 10))         /* can emit trace events */

/* All capabilities */
#define ASX_CAP_ALL ((asx_cap_flags)0x7FFu)

/* ------------------------------------------------------------------ */
/* Cx type                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_region_id region_id; /* owning region */
    asx_task_id task_id;     /* owning task (0 if region-level) */
    asx_cap_flags caps;      /* granted capabilities */
    asx_budget *budget;      /* borrowed budget (NULL if none) */
    asx_time *clock;         /* borrowed logical clock (NULL = wall clock) */
    uint64_t *entropy_state; /* borrowed entropy state (NULL = system) */
    uint32_t generation;     /* context generation (for stale detection) */
} asx_cx;

typedef struct {
    asx_region_id region_id;
    asx_task_id task_id;
    asx_cap_flags caps;
    uint32_t parent_generation;
} asx_cx_wrapper;

typedef struct {
    asx_cap_flags caps;
    uint32_t parent_generation;
    uint32_t caveat_count;
} asx_cx_macaroon;

typedef struct {
    uint32_t slot;
    uint32_t generation;
} asx_cx_grant;

typedef struct {
    uint32_t generation;
    asx_cap_flags caps;
    asx_region_id parent_region_id;
    asx_task_id parent_task_id;
    uint32_t parent_generation;
    int alive;
} asx_cx_registry_entry;

typedef struct {
    asx_cx_registry_entry *entries;
    uint32_t capacity;
    uint32_t next_generation;
} asx_cx_registry;

/* ------------------------------------------------------------------ */
/* Cx lifecycle                                                        */
/* ------------------------------------------------------------------ */

/* Initialize a Cx with identity and capabilities.
 * Does NOT allocate — all pointers borrow from caller-owned storage.
 * Returns ASX_OK on success. */
ASX_API asx_status asx_cx_init(asx_cx *cx, asx_region_id region_id, asx_task_id task_id,
                               asx_cap_flags caps);

/* Create a child Cx with restricted capabilities (capability narrowing).
 * The child inherits identity but can only have a subset of parent caps.
 * Returns ASX_E_INVALID_ARGUMENT if child_caps contains caps not in parent. */
ASX_API asx_status asx_cx_narrow(const asx_cx *parent, asx_cx *child, asx_cap_flags child_caps);

/* Create a child Cx with the parent's current authority attenuated by cap_mask. */
ASX_API asx_status asx_cx_attenuate(const asx_cx *parent, asx_cx *child, asx_cap_flags cap_mask);

/* Invalidate a Cx (sets generation to 0, clears all fields). */
ASX_API void asx_cx_invalidate(asx_cx *cx);

/* Check if a Cx is valid (non-zero generation, non-zero region). */
ASX_API int asx_cx_is_valid(const asx_cx *cx);

/* ------------------------------------------------------------------ */
/* Identity accessors                                                  */
/* ------------------------------------------------------------------ */

/* Get the region ID from a context. Returns ASX_INVALID_ID if cx is NULL. */
ASX_API asx_region_id asx_cx_region(const asx_cx *cx);

/* Get the task ID from a context. Returns ASX_INVALID_ID if cx is NULL. */
ASX_API asx_task_id asx_cx_task(const asx_cx *cx);

/* ------------------------------------------------------------------ */
/* Capability queries                                                  */
/* ------------------------------------------------------------------ */

/* Check if a Cx has a specific capability. */
ASX_API int asx_cx_has_cap(const asx_cx *cx, asx_cap_flags cap);

/* Get the capability flags from a context. Returns ASX_CAP_NONE if cx is NULL. */
ASX_API asx_cap_flags asx_cx_caps(const asx_cx *cx);

/* ------------------------------------------------------------------ */
/* Explicit wrapper / registry / attenuation tokens                    */
/* ------------------------------------------------------------------ */

/* Capture an explicit wrapper for a subset of the parent's authority. */
ASX_API asx_status asx_cx_wrap(const asx_cx *parent, asx_cap_flags child_caps,
                               asx_cx_wrapper *out_wrapper);

/* Materialize a wrapped authority back into a child Cx. */
ASX_API asx_status asx_cx_unwrap(const asx_cx *parent, const asx_cx_wrapper *wrapper,
                                 asx_cx *out_child);

/* Initialize a fixed-capacity zero-allocation registry of grants. */
ASX_API asx_status asx_cx_registry_init(asx_cx_registry *registry, asx_cx_registry_entry *entries,
                                        uint32_t capacity);

/* Issue a revocable grant for a subset of the parent's authority.
 * The grant is bound to the issuing parent context and must be materialized
 * against that same parent identity/generation. */
ASX_API asx_status asx_cx_registry_issue(const asx_cx *parent, asx_cx_registry *registry,
                                         asx_cap_flags child_caps, asx_cx_grant *out_grant);

/* Revoke a previously issued grant. */
ASX_API asx_status asx_cx_registry_revoke(asx_cx_registry *registry, asx_cx_grant grant);

/* Materialize a live grant into a child Cx.
 * Fails closed if the grant is replayed against a different parent context. */
ASX_API asx_status asx_cx_registry_materialize(const asx_cx *parent,
                                               const asx_cx_registry *registry, asx_cx_grant grant,
                                               asx_cx *out_child);

/* Capture an attenuated macaroon-like token for later rebinding. */
ASX_API asx_status asx_cx_macaroon_issue(const asx_cx *parent, asx_cap_flags child_caps,
                                         asx_cx_macaroon *out_macaroon);

/* Apply an additional attenuation caveat to a macaroon. */
ASX_API asx_status asx_cx_macaroon_attenuate(asx_cx_macaroon *macaroon, asx_cap_flags caveat_caps);

/* Rebind a macaroon against its parent context, failing closed on mismatch. */
ASX_API asx_status asx_cx_macaroon_bind(const asx_cx *parent, const asx_cx_macaroon *macaroon,
                                        asx_cx *out_child);

/* ------------------------------------------------------------------ */
/* Budget access                                                       */
/* ------------------------------------------------------------------ */

/* Bind a budget to a Cx. The budget must outlive the Cx.
 * Requires ASX_CAP_BUDGET_READ or ASX_CAP_BUDGET_CONSUME. */
ASX_API asx_status asx_cx_bind_budget(asx_cx *cx, asx_budget *budget);

/* Get the remaining budget. Returns NULL if no budget bound or cap missing. */
ASX_API const asx_budget *asx_cx_budget(const asx_cx *cx);

/* Consume one poll from the budget. Returns ASX_OK if poll consumed,
 * ASX_E_BUDGET_EXHAUSTED if no polls remain, ASX_E_PERMISSION_DENIED
 * if ASX_CAP_BUDGET_CONSUME not set. */
ASX_API asx_status asx_cx_consume_poll(asx_cx *cx);

/* ------------------------------------------------------------------ */
/* Clock access                                                        */
/* ------------------------------------------------------------------ */

/* Bind a logical clock to a Cx. The clock must outlive the Cx.
 * When bound, asx_cx_now() reads from this clock instead of wall clock.
 * Requires ASX_CAP_CLOCK_READ. */
ASX_API asx_status asx_cx_bind_clock(asx_cx *cx, asx_time *clock);

/* Read the current time through the Cx.
 * Uses logical clock if bound, otherwise returns 0 (caller should
 * use platform hooks for wall clock). */
ASX_API asx_time asx_cx_now(const asx_cx *cx);

/* ------------------------------------------------------------------ */
/* Entropy access                                                      */
/* ------------------------------------------------------------------ */

/* Bind an entropy state to a Cx. The state must outlive the Cx.
 * Requires ASX_CAP_ENTROPY. */
ASX_API asx_status asx_cx_bind_entropy(asx_cx *cx, uint64_t *entropy_state);

/* Draw a random u64 through the Cx. Returns 0 if no entropy bound
 * or cap missing. */
ASX_API uint64_t asx_cx_random_u64(asx_cx *cx);

/* ------------------------------------------------------------------ */
/* Cancellation checkpoint                                             */
/* ------------------------------------------------------------------ */

/* Check if cancellation has been requested for this context's task.
 * Returns nonzero if cancel was requested.
 * Requires ASX_CAP_CANCEL_CHECK. Returns 0 if cap missing. */
ASX_API int asx_cx_is_cancelled(const asx_cx *cx);

/* Cooperative cancellation checkpoint.
 * Returns ASX_OK if not cancelled, ASX_E_CANCELLED if cancel requested.
 * Should be called periodically in long-running operations.
 * Also consumes a poll from the budget if bound. */
ASX_API asx_status asx_cx_checkpoint(asx_cx *cx);

#ifdef __cplusplus
}
#endif

#endif /* ASX_CX_CX_H */
