/*
 * asx/runtime/replay.h — replay oracles, snapshot/restore, and
 *                         counterexample minimization
 *
 * Builds on the lab runtime to provide:
 *   - Snapshot/restore for deterministic replay debugging
 *   - Oracles for detecting quiescence violations, leaks, and
 *     replay divergence
 *   - Counterexample minimization hooks for shrinking failing scenarios
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_RUNTIME_REPLAY_H

#include <asx/app/report.h>
#include <asx/asx_config.h>
#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>

#if !defined(ASX_PROFILE_BROWSER) || ASX_HAS_BROWSER_TRACE ||                                      \
    defined(ASX_INTERNAL_TRACE_FAMILY_ACCESS)
/* Guard defined only with the contents; see asx/runtime/trace.h. */
#define ASX_RUNTIME_REPLAY_H

#include <asx/runtime/lab.h>
#include <asx/runtime/trace.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Snapshot — save and restore lab state
 * ------------------------------------------------------------------- */

typedef struct {
    asx_lab_config config;  /* saved lab config */
    uint64_t entropy_state; /* saved PRNG state */
    asx_time current_time;  /* saved virtual time */
    int valid;              /* 1 if snapshot is populated */
} asx_snapshot;

typedef struct {
    uint32_t slot;
} asx_snapshot_id;

/* Take a snapshot of the current lab state. */
ASX_API ASX_MUST_USE asx_status asx_snapshot_take(const asx_lab *lab, asx_snapshot_id *out);

/* Restore a lab to a previously saved snapshot, including rebuilding the
 * embedded deterministic runtime hooks before reinstating saved lab state. */
ASX_API asx_status asx_snapshot_restore_checked(asx_lab *lab, asx_snapshot_id id,
                                                uint64_t application_fingerprint);
#define asx_snapshot_restore(lab, id)                                                              \
    asx_snapshot_restore_checked((lab), (id), ASX_LAB_CONFIG_FINGERPRINT)

/* Discard a snapshot. */
ASX_API asx_status asx_snapshot_discard(asx_snapshot_id id);

/* Check if a snapshot is valid. */
ASX_API int asx_snapshot_is_valid(asx_snapshot_id id);

/* Reset all snapshots. */
ASX_API void asx_snapshot_reset(void);

/* -------------------------------------------------------------------
 * Oracle — post-run analysis of lab state
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_ORACLE_PASS = 0,
    ASX_ORACLE_FAIL = 1,
    ASX_ORACLE_INCONCLUSIVE = 2
} asx_oracle_verdict;

/* Oracle result with diagnostic info. */
typedef struct {
    asx_oracle_verdict verdict;
    const char *oracle_name;
    const char *message;   /* diagnostic string (static) */
    uint32_t detail_count; /* number of detail items */
} asx_oracle_result;

/* Oracle function signature. Analyzes lab state and produces verdict. */
typedef asx_oracle_result (*asx_oracle_fn)(const asx_lab *lab, void *ctx);

/* Built-in oracles */

/* Quiescence oracle: checks that no tasks remain active. */
ASX_API asx_oracle_result asx_oracle_quiescence(const asx_lab *lab, void *ctx);

/* Leak oracle: checks that every live region is closed. Obligations are the
 * obligation_leak oracle's (asx_oracle_obligation_leak): a closed region that
 * holds a reserved one. */
ASX_API asx_oracle_result asx_oracle_leak(const asx_lab *lab, void *ctx);

/* Replay oracle: checks that two runs produce identical results. */
ASX_API asx_oracle_result asx_oracle_replay_match(const asx_lab_result *r1,
                                                  const asx_lab_result *r2);

/* Kernel-state oracles (bd-9kll.9.2, docs/LAB_ORACLE_CATALOG.md): Rust's lab
 * oracles of the same names over the region, task and obligation arenas, each
 * one pass that writes nothing. They check what the kernel enforces by
 * construction, so a FAIL is a kernel bug. A NULL lab fails and an
 * uninitialized one is inconclusive, as above; the state read is the kernel
 * of the lab's runtime. detail_count is the number of violating entities. */

/* task_leak: no closed region has a live task. */
ASX_API asx_oracle_result asx_oracle_task_leak(const asx_lab *lab, void *ctx);

/* obligation_leak: no closed region holds a reserved obligation, and no
 * reserved obligation's holder task has completed or is gone. */
ASX_API asx_oracle_result asx_oracle_obligation_leak(const asx_lab *lab, void *ctx);

/* quiescence: a closed region has no live task, no child that is not closed,
 * no reserved obligation and no cleanup pending. */
ASX_API asx_oracle_result asx_oracle_region_quiescence(const asx_lab *lab, void *ctx);

/* finalizer: no closed region has cleanup still pending. */
ASX_API asx_oracle_result asx_oracle_finalizer(const asx_lab *lab, void *ctx);

/* region_tree: one root, every other live region listed by a live parent,
 * every listed child a live region that names its parent, no cycle. */
ASX_API asx_oracle_result asx_oracle_region_tree(const asx_lab *lab, void *ctx);

/* deadline_monotone: no region's deadline is later than its parent's. */
ASX_API asx_oracle_result asx_oracle_deadline_monotone(const asx_lab *lab, void *ctx);

/* cancellation_protocol, the region part (INV-CANCEL-PROPAGATES): every live,
 * not closed child of a region with a cancel requested has one too. */
ASX_API asx_oracle_result asx_oracle_cancel_propagation(const asx_lab *lab, void *ctx);

/* cancel_correctness: every active cancel witness is its task's own (same
 * task, region and cancel epoch, a non-zero epoch, a phase not ahead of the
 * task's), and no completed task still holds one. */
ASX_API asx_oracle_result asx_oracle_cancel_correctness(const asx_lab *lab, void *ctx);

/* cancel_debt: under lab dispatch, no task has a cancel requested and
 * nothing left to run it (not scheduled, not polled since, not masked, no
 * timer). Without lab dispatch the sweep's idle handling delivers the
 * cancel and the oracle passes without looking. */
ASX_API asx_oracle_result asx_oracle_cancel_debt(const asx_lab *lab, void *ctx);

/* task_counters (no Rust twin): every region's live-task counter equals the
 * live, not completed tasks that name it. */
ASX_API asx_oracle_result asx_oracle_task_counters(const asx_lab *lab, void *ctx);

/* arena_counters (no Rust twin): the kernel's live counts of regions, tasks
 * and obligations equal their live slots, and the task and obligation free
 * lists hold exactly the released slots. */
ASX_API asx_oracle_result asx_oracle_arena_counters(const asx_lab *lab, void *ctx);

/* channel_atomicity: every channel's counters agree: queued messages plus
 * reserved permits within its capacity, the reserved count equal to the live
 * permit tokens, the sender count equal to the creator's sender (until
 * dropped) plus the live clones. A mailbox is a bounded channel, so this is
 * also its capacity accounting. */
ASX_API asx_oracle_result asx_oracle_channel_atomicity(const asx_lab *lab, void *ctx);

/* down_order: monitors fired in order: no DOWN delivered with an earlier
 * completion time than the one before it, and the monitors of one exit by
 * ascending reference (the monitor module keeps the last
 * ASX_MONITOR_CAPACITY deliveries). */
ASX_API asx_oracle_result asx_oracle_down_order(const asx_lab *lab, void *ctx);

/* reply_linearity: every obligation (a call's reply is a SEND_PERMIT
 * obligation) resolved at most once, after its reservation, by the trace
 * ring's record (the reservation is checked while the ring dropped
 * nothing). */
ASX_API asx_oracle_result asx_oracle_reply_linearity(const asx_lab *lab, void *ctx);

/* loser_drain: no race loser (a live task cancelled with RACE_LOST) is
 * still running after the task that was collecting it, its completion
 * watcher, has ended. */
ASX_API asx_oracle_result asx_oracle_loser_drain(const asx_lab *lab, void *ctx);

/* region_leak: no region is stuck closing: one in CLOSING, DRAINING or
 * FINALIZING with no live task, no child not closed, no cleanup pending or
 * running and no reserved obligation would never close. (Rust's version is
 * the same idea with wall-clock thresholds; state has no clock.) */
ASX_API asx_oracle_result asx_oracle_region_leak(const asx_lab *lab, void *ctx);

/* cancel_signal_ordering: a region cancelled by its parent (a propagated
 * ParentCancelled reason) has a parent that was cancelled, and the parent's
 * region.cancelled event precedes the child's in the trace ring (checked
 * while the ring dropped nothing). */
ASX_API asx_oracle_result asx_oracle_cancel_signal_ordering(const asx_lab *lab, void *ctx);

/* The kernel-state oracles are also reachable by Rust's registry name, for a
 * caller with no asx_lab (the conformance interpreter), over the default
 * runtime's kernel. The number of them. */
ASX_API uint32_t asx_oracle_kernel_count(void);

/* The index-th kernel-state oracle's name (index < asx_oracle_kernel_count()),
 * or NULL past the end. */
ASX_API const char *asx_oracle_kernel_name(uint32_t index);

/* Run the named kernel-state oracle (task_leak, obligation_leak, quiescence,
 * finalizer, region_tree, deadline_monotone, cancellation_protocol,
 * cancel_correctness, cancel_debt, task_counters, arena_counters,
 * region_leak, cancel_signal_ordering, channel_atomicity, down_order,
 * reply_linearity, loser_drain). An unknown or NULL name fails with the
 * oracle name "unknown". */
ASX_API asx_oracle_result asx_oracle_check_named(const char *name);

/* Render a replay result as JSON for retained diagnostics/artifacts. */
ASX_API ASX_MUST_USE asx_status asx_replay_render_result_json(const asx_replay_result *result,
                                                              asx_report_buf *out);

/* Render the current replay verification state as JSON, including
 * expected/actual divergent events when a reference is loaded. */
ASX_API ASX_MUST_USE asx_status asx_replay_render_current_diff_json(asx_report_buf *out);

/* -------------------------------------------------------------------
 * Oracle suite — run multiple oracles and collect results
 * ------------------------------------------------------------------- */

typedef struct {
    asx_oracle_fn oracles[ASX_ORACLE_SUITE_MAX];
    void *oracle_ctx[ASX_ORACLE_SUITE_MAX];
    asx_oracle_result results[ASX_ORACLE_SUITE_MAX];
    uint32_t count;
    uint32_t pass_count;
    uint32_t fail_count;
} asx_oracle_suite;

/* Initialize an oracle suite. */
ASX_API void asx_oracle_suite_init(asx_oracle_suite *suite);

/* Add an oracle to the suite. */
ASX_API ASX_MUST_USE asx_status asx_oracle_suite_add(asx_oracle_suite *suite, asx_oracle_fn oracle,
                                                     void *ctx);

/* Run all oracles against the lab state. Returns ASX_OK if all pass.
 * Fails closed with ASX_E_INVALID_ARGUMENT when suite->count exceeds
 * ASX_ORACLE_SUITE_MAX. */
ASX_API asx_status asx_oracle_suite_run(asx_oracle_suite *suite, const asx_lab *lab);

/* -------------------------------------------------------------------
 * Counterexample minimization — shrink failing scenarios
 * ------------------------------------------------------------------- */

#define ASX_MINIMIZE_SCHEMA_NAME "asx.replay_counterexample"
#define ASX_MINIMIZE_SCHEMA_VERSION "asx.replay_counterexample.v1"

typedef enum {
    ASX_MINIMIZE_FAILURE_NONE = 0,
    ASX_MINIMIZE_FAILURE_ORACLE = 1,
    ASX_MINIMIZE_FAILURE_RESOURCE = 2,
    ASX_MINIMIZE_FAILURE_RUNTIME = 3,
    ASX_MINIMIZE_FAILURE_CONFORMANCE = 4
} asx_minimize_failure_class;

typedef struct {
    asx_status run_status;
    asx_status last_status;
    asx_error_category status_category;
    asx_oracle_verdict oracle_verdict;
    const char *oracle_name;
    const char *oracle_message;
    asx_minimize_failure_class failure_class;
} asx_minimize_observation;

typedef struct {
    const asx_lab_config *config; /* lab config to use */
    asx_lab_scenario scenario;    /* current candidate */
    asx_oracle_fn oracle;         /* oracle to check */
    void *oracle_ctx;
    asx_minimize_observation original_observation; /* failure to preserve */
    asx_minimize_observation current_observation;  /* current candidate failure */
    uint32_t original_steps;                       /* original step count */
    uint32_t attempts;                             /* shrink attempts made */
    uint32_t accepted_attempts;                    /* shrink attempts accepted */
    uint32_t rejected_attempts;                    /* shrink attempts rejected */
    uint32_t max_attempts;                         /* max shrink attempts */
    int found_smaller;                             /* 1 if we found a smaller fail */
} asx_minimize_state;

/* Initialize counterexample minimization.
 * Takes the failing scenario and oracle, tries to find a smaller
 * scenario that still fails. */
ASX_API asx_status asx_minimize_init(asx_minimize_state *state, const asx_lab_config *config,
                                     const asx_lab_scenario *failing_scenario, asx_oracle_fn oracle,
                                     void *oracle_ctx, uint32_t max_attempts);

/* Run one shrink step. Returns ASX_OK if done minimizing,
 * ASX_E_PENDING if more steps remain. */
ASX_API asx_status asx_minimize_step(asx_minimize_state *state);

/* Run minimization to completion, looping until a terminal status
 * is reached or an error occurs. */
ASX_API asx_status asx_minimize_run(asx_minimize_state *state);

/* Get the minimized scenario (valid after minimize completes). */
ASX_API const asx_lab_scenario *asx_minimize_result(const asx_minimize_state *state);

/* Get the number of shrink attempts made. */
ASX_API uint32_t asx_minimize_attempts(const asx_minimize_state *state);

/* Return the stable string form of a minimizer failure class. */
ASX_API const char *asx_minimize_failure_class_str(asx_minimize_failure_class failure_class);

/* Return captured observations used for class-preserving minimization. */
ASX_API const asx_minimize_observation *asx_minimize_original_observation(
    const asx_minimize_state *state);
ASX_API const asx_minimize_observation *asx_minimize_current_observation(
    const asx_minimize_state *state);

/* Render minimization progress/result as JSON for retained artifacts. */
ASX_API ASX_MUST_USE asx_status asx_minimize_render_json(const asx_minimize_state *state,
                                                         asx_report_buf *out);

#ifdef __cplusplus
}
#endif

#endif /* trace family public contract */

#endif /* ASX_RUNTIME_REPLAY_H */
