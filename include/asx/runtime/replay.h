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
#define ASX_RUNTIME_REPLAY_H

#include <asx/asx_config.h>
#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>
#include <asx/runtime/lab.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Snapshot — save and restore lab state
 * ------------------------------------------------------------------- */

#ifndef ASX_SNAPSHOT_MAX
#define ASX_SNAPSHOT_MAX 8u
#endif

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
ASX_API asx_status asx_snapshot_restore(asx_lab *lab, asx_snapshot_id id);

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

/* Leak oracle: checks that all regions and obligations are resolved. */
ASX_API asx_oracle_result asx_oracle_leak(const asx_lab *lab, void *ctx);

/* Replay oracle: checks that two runs produce identical results. */
ASX_API asx_oracle_result asx_oracle_replay_match(const asx_lab_result *r1,
                                                  const asx_lab_result *r2);

/* -------------------------------------------------------------------
 * Oracle suite — run multiple oracles and collect results
 * ------------------------------------------------------------------- */

#ifndef ASX_ORACLE_SUITE_MAX
#define ASX_ORACLE_SUITE_MAX 8u
#endif

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

/* Run all oracles against the lab state. Returns ASX_OK if all pass. */
ASX_API asx_status asx_oracle_suite_run(asx_oracle_suite *suite, const asx_lab *lab);

/* -------------------------------------------------------------------
 * Counterexample minimization — shrink failing scenarios
 * ------------------------------------------------------------------- */

typedef struct {
    const asx_lab_config *config; /* lab config to use */
    asx_lab_scenario scenario;    /* current candidate */
    asx_oracle_fn oracle;         /* oracle to check */
    void *oracle_ctx;
    uint32_t original_steps; /* original step count */
    uint32_t attempts;       /* shrink attempts made */
    uint32_t max_attempts;   /* max shrink attempts */
    int found_smaller;       /* 1 if we found a smaller fail */
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

/* Get the minimized scenario (valid after minimize completes). */
ASX_API const asx_lab_scenario *asx_minimize_result(const asx_minimize_state *state);

/* Get the number of shrink attempts made. */
ASX_API uint32_t asx_minimize_attempts(const asx_minimize_state *state);

#ifdef __cplusplus
}
#endif

#endif /* ASX_RUNTIME_REPLAY_H */
