/*
 * e2e_foundational_contracts.c — e2e scenario exercising foundational type
 *                                 contracts in realistic runtime use
 *
 * Demonstrates ID/outcome/cancel/budget/policy working together in a
 * runtime lifecycle: task spawn with budget, cancellation with severity
 * strengthening, outcome aggregation, and cleanup budget enforcement.
 *
 * Output: one line per scenario in the format:
 *   SCENARIO <id> <pass|fail> [diagnostic]
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <asx/core/budget.h>
#include <asx/core/cancel.h>
#include <asx/core/outcome.h>
#include <stdio.h>
#include <string.h>

/* -------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------- */

static int g_pass = 0;
static int g_fail = 0;

#define SCENARIO_BEGIN(id)                                                                         \
    do {                                                                                           \
        const char *_scenario_id = (id);                                                           \
        int _scenario_ok = 1;                                                                      \
    (void)0

#define SCENARIO_CHECK(cond, msg)                                                                  \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("SCENARIO %s fail %s\n", _scenario_id, (msg));                                  \
            _scenario_ok = 0;                                                                      \
            g_fail++;                                                                              \
            goto _scenario_end;                                                                    \
        }                                                                                          \
    } while (0)

#define SCENARIO_END()                                                                             \
    _scenario_end:                                                                                 \
    if (_scenario_ok) {                                                                            \
        printf("SCENARIO %s pass\n", _scenario_id);                                                \
        g_pass++;                                                                                  \
    }                                                                                              \
    }                                                                                              \
    while (0)

/* -------------------------------------------------------------------
 * Scenario 1: Budget-bounded task lifecycle
 *
 * A task is granted a poll budget. Each poll consumes one unit.
 * When the budget is exhausted, the task observes exhaustion and
 * the outcome is ERR. Demonstrates budget algebra in context.
 * ------------------------------------------------------------------- */

static void scenario_budget_bounded_lifecycle(void) {
    SCENARIO_BEGIN("foundational.budget_bounded_lifecycle");

    asx_budget task_budget = asx_budget_from_polls(5);
    asx_outcome task_outcome = asx_outcome_make(ASX_OUTCOME_OK);
    int polls_executed = 0;

    /* Simulate polling loop with budget enforcement */
    while (!asx_budget_is_exhausted(&task_budget)) {
        uint32_t remaining = asx_budget_consume_poll(&task_budget);
        if (remaining == 0) break; /* exhausted on this poll */
        polls_executed++;
    }

    /* Budget should be exhausted after 5 polls */
    SCENARIO_CHECK(polls_executed == 5, "expected 5 polls");
    SCENARIO_CHECK(asx_budget_is_exhausted(&task_budget), "budget should be exhausted");

    /* Exhaustion produces ERR outcome */
    if (asx_budget_is_exhausted(&task_budget)) {
        asx_outcome exhaustion = asx_outcome_make(ASX_OUTCOME_ERR);
        task_outcome = asx_outcome_join(&task_outcome, &exhaustion);
    }

    SCENARIO_CHECK(asx_outcome_severity_of(&task_outcome) == ASX_OUTCOME_ERR,
                   "outcome should be ERR after exhaustion");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario 2: Cancellation strengthening during region drain
 *
 * Multiple cancellation reasons arrive during region drain. The
 * strengthen operation selects the most severe. Demonstrates cancel
 * severity lattice and attribution preservation.
 * ------------------------------------------------------------------- */

static void scenario_cancel_strengthen_during_drain(void) {
    SCENARIO_BEGIN("foundational.cancel_strengthen_drain");

    /* First: user-initiated cancel */
    asx_cancel_reason user_cancel;
    memset(&user_cancel, 0, sizeof(user_cancel));
    user_cancel.kind = ASX_CANCEL_USER;
    user_cancel.timestamp = 1000;
    user_cancel.origin_region = 0x0001;
    user_cancel.origin_task = 0x0010;

    /* Second: timeout arrives (severity 1 > severity 0) */
    asx_cancel_reason timeout;
    memset(&timeout, 0, sizeof(timeout));
    timeout.kind = ASX_CANCEL_TIMEOUT;
    timeout.timestamp = 1050;
    timeout.origin_region = 0x0001;
    timeout.origin_task = 0x0020;

    /* Third: parent cancel propagates (severity 4 > severity 1) */
    asx_cancel_reason parent;
    memset(&parent, 0, sizeof(parent));
    parent.kind = ASX_CANCEL_PARENT;
    parent.timestamp = 1100;
    parent.origin_region = 0x0002;
    parent.origin_task = 0x0030;

    /* Strengthen accumulates: user -> timeout -> parent */
    asx_cancel_reason strongest = asx_cancel_strengthen(&user_cancel, &timeout);
    SCENARIO_CHECK(asx_cancel_severity(strongest.kind) == 1,
                   "timeout (sev 1) should beat user (sev 0)");

    strongest = asx_cancel_strengthen(&strongest, &parent);
    SCENARIO_CHECK(asx_cancel_severity(strongest.kind) == 4,
                   "parent (sev 4) should beat timeout (sev 1)");

    /* Parent's attribution preserved */
    SCENARIO_CHECK(strongest.origin_region == 0x0002, "parent's origin_region should be preserved");
    SCENARIO_CHECK(strongest.origin_task == 0x0030, "parent's origin_task should be preserved");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario 3: Cleanup budget derived from cancel severity
 *
 * When a task is cancelled, the cleanup budget depends on the cancel
 * kind's severity. More severe cancellations get tighter budgets.
 * The task must complete cleanup within its budget.
 * ------------------------------------------------------------------- */

static void scenario_cleanup_budget_enforcement(void) {
    SCENARIO_BEGIN("foundational.cleanup_budget_enforcement");

    /* User cancel: generous cleanup budget */
    asx_budget user_cleanup = asx_cancel_cleanup_budget(ASX_CANCEL_USER);
    SCENARIO_CHECK(user_cleanup.poll_quota > 0, "user cleanup should have polls");

    /* Shutdown cancel: tight cleanup budget */
    asx_budget shut_cleanup = asx_cancel_cleanup_budget(ASX_CANCEL_SHUTDOWN);
    SCENARIO_CHECK(shut_cleanup.poll_quota > 0, "shutdown cleanup should have polls");
    SCENARIO_CHECK(shut_cleanup.poll_quota < user_cleanup.poll_quota,
                   "shutdown should have fewer cleanup polls than user");
    SCENARIO_CHECK(shut_cleanup.priority > user_cleanup.priority,
                   "shutdown should have higher cleanup priority");

    /* If multiple cancel kinds affect a task, meet tightens the budget */
    asx_budget combined = asx_budget_meet(&user_cleanup, &shut_cleanup);
    SCENARIO_CHECK(combined.poll_quota <= shut_cleanup.poll_quota,
                   "combined budget should be at most as generous as shutdown");

    /* Simulate cleanup within the tight budget */
    int cleanup_steps = 0;
    while (!asx_budget_is_exhausted(&combined)) {
        uint32_t remaining = asx_budget_consume_poll(&combined);
        if (remaining == 0) break;
        cleanup_steps++;
    }

    SCENARIO_CHECK(cleanup_steps > 0, "should complete at least one cleanup step");
    SCENARIO_CHECK(asx_budget_is_exhausted(&combined), "cleanup budget should be exhausted");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario 4: Outcome aggregation across child tasks
 *
 * Parent task spawns children. Each child produces an outcome.
 * The parent joins all outcomes to determine the aggregate result.
 * Demonstrates the severity lattice in multi-task composition.
 * ------------------------------------------------------------------- */

static void scenario_outcome_aggregation(void) {
    SCENARIO_BEGIN("foundational.outcome_aggregation");

    /* Simulate 4 child task outcomes */
    asx_outcome child_outcomes[4];
    child_outcomes[0] = asx_outcome_make(ASX_OUTCOME_OK);        /* child 0: success */
    child_outcomes[1] = asx_outcome_make(ASX_OUTCOME_ERR);       /* child 1: error */
    child_outcomes[2] = asx_outcome_make(ASX_OUTCOME_OK);        /* child 2: success */
    child_outcomes[3] = asx_outcome_make(ASX_OUTCOME_CANCELLED); /* child 3: cancelled */

    /* Join all outcomes */
    asx_outcome aggregate = asx_outcome_make(ASX_OUTCOME_OK); /* identity */
    int i;
    for (i = 0; i < 4; i++) { aggregate = asx_outcome_join(&aggregate, &child_outcomes[i]); }

    /* CANCELLED > ERR > OK, so aggregate should be CANCELLED */
    SCENARIO_CHECK(asx_outcome_severity_of(&aggregate) == ASX_OUTCOME_CANCELLED,
                   "aggregate should be CANCELLED (max severity)");

    /* If any child panics, that dominates everything */
    asx_outcome panic = asx_outcome_make(ASX_OUTCOME_PANICKED);
    aggregate = asx_outcome_join(&aggregate, &panic);
    SCENARIO_CHECK(asx_outcome_severity_of(&aggregate) == ASX_OUTCOME_PANICKED,
                   "PANICKED should dominate all other outcomes");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario 5: Handle generation safety with type discrimination
 *
 * Handles encode type, state, generation, and slot. Stale handles
 * from a previous generation must be distinguishable from current.
 * ------------------------------------------------------------------- */

static void scenario_handle_generation_safety(void) {
    SCENARIO_BEGIN("foundational.handle_generation_safety");

    /* Allocate a "task slot 7, generation 1" handle */
    uint32_t idx_gen1 = asx_handle_pack_index(1, 7);
    uint64_t h_gen1 = asx_handle_pack(ASX_TYPE_TASK, 0x000F, idx_gen1);

    /* Verify decomposition */
    SCENARIO_CHECK(asx_handle_type_tag(h_gen1) == ASX_TYPE_TASK, "type should be TASK");
    SCENARIO_CHECK(asx_handle_slot(h_gen1) == 7, "slot should be 7");
    SCENARIO_CHECK(asx_handle_generation(h_gen1) == 1, "generation should be 1");

    /* "Free" the slot and reallocate as generation 2 */
    uint32_t idx_gen2 = asx_handle_pack_index(2, 7);
    uint64_t h_gen2 = asx_handle_pack(ASX_TYPE_TASK, 0x000F, idx_gen2);

    /* Same slot, different generation -> different handle */
    SCENARIO_CHECK(h_gen1 != h_gen2, "stale and current handles must differ");
    SCENARIO_CHECK(asx_handle_slot(h_gen1) == asx_handle_slot(h_gen2), "same slot number");
    SCENARIO_CHECK(asx_handle_generation(h_gen1) != asx_handle_generation(h_gen2),
                   "different generations");

    /* A region handle for the same slot is also distinct */
    uint64_t h_region = asx_handle_pack(ASX_TYPE_REGION, 0x000F, idx_gen2);
    SCENARIO_CHECK(h_gen2 != h_region,
                   "different type tags for same slot must produce different handles");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario 6: Policy-driven behavior selection
 *
 * Safety profile determines containment policy. This scenario
 * verifies the policy contract is consistent.
 * ------------------------------------------------------------------- */

static void scenario_policy_driven_containment(void) {
    SCENARIO_BEGIN("foundational.policy_containment");

    /* Verify active profile is deterministic */
    asx_safety_profile active = asx_safety_profile_active();
    const char *profile_name = asx_safety_profile_str(active);
    SCENARIO_CHECK(profile_name != NULL, "profile name should not be NULL");

    /* Profile -> containment mapping is consistent */
    asx_containment_policy policy = asx_containment_policy_for_profile(active);
    asx_containment_policy active_policy = asx_containment_policy_active();
    SCENARIO_CHECK(policy == active_policy,
                   "containment policy from profile should match active policy");

    /* Ordering contracts hold */
    SCENARIO_CHECK(ASX_SAFETY_DEBUG < ASX_SAFETY_RELEASE,
                   "debug profile should have lower ordinal than release");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario 7: Cancel cause chain with budget interaction
 *
 * Build a cancel reason chain (child -> parent -> root) and verify
 * the chain walk produces correct cleanup budgets at each level.
 * ------------------------------------------------------------------- */

static void scenario_cancel_chain_budget_interaction(void) {
    SCENARIO_BEGIN("foundational.cancel_chain_budget");

    /* Root cause: shutdown */
    asx_cancel_reason root;
    memset(&root, 0, sizeof(root));
    root.kind = ASX_CANCEL_SHUTDOWN;
    root.timestamp = 100;
    root.cause = NULL;

    /* Intermediate: linked exit caused by shutdown */
    asx_cancel_reason mid;
    memset(&mid, 0, sizeof(mid));
    mid.kind = ASX_CANCEL_LINKED_EXIT;
    mid.timestamp = 110;
    mid.cause = &root;

    /* Leaf: race lost caused by linked exit */
    asx_cancel_reason leaf;
    memset(&leaf, 0, sizeof(leaf));
    leaf.kind = ASX_CANCEL_RACE_LOST;
    leaf.timestamp = 120;
    leaf.cause = &mid;

    /* Walk the chain and verify budgets are monotone with severity */
    asx_budget leaf_b = asx_cancel_cleanup_budget(leaf.kind);
    asx_budget mid_b = asx_cancel_cleanup_budget(mid.kind);
    asx_budget root_b = asx_cancel_cleanup_budget(root.kind);

    /* leaf and mid are same severity (3), root is severity 5 */
    SCENARIO_CHECK(asx_cancel_severity(leaf.kind) == asx_cancel_severity(mid.kind),
                   "RACE_LOST and LINKED_EXIT should have same severity");
    SCENARIO_CHECK(asx_cancel_severity(root.kind) > asx_cancel_severity(mid.kind),
                   "SHUTDOWN should have higher severity than LINKED_EXIT");

    /* Root (highest severity) gets tightest budget */
    SCENARIO_CHECK(root_b.poll_quota <= mid_b.poll_quota,
                   "shutdown cleanup budget should be tighter than linked_exit");

    /* Strengthen through chain selects root (highest severity) */
    asx_cancel_reason strongest = asx_cancel_strengthen(&leaf, &mid);
    strongest = asx_cancel_strengthen(&strongest, &root);
    SCENARIO_CHECK(strongest.kind == ASX_CANCEL_SHUTDOWN, "strongest reason should be SHUTDOWN");

    /* Meet all cleanup budgets: produces the tightest constraints */
    asx_budget combined = asx_budget_meet(&leaf_b, &mid_b);
    combined = asx_budget_meet(&combined, &root_b);
    SCENARIO_CHECK(combined.poll_quota <= root_b.poll_quota,
                   "combined budget should be at most as generous as the tightest");
    SCENARIO_CHECK(!asx_budget_is_exhausted(&combined),
                   "combined budget should not be pre-exhausted");

    /* Verify chain linkage */
    SCENARIO_CHECK(leaf.cause == &mid, "leaf cause should point to mid");
    SCENARIO_CHECK(mid.cause == &root, "mid cause should point to root");
    SCENARIO_CHECK(root.cause == NULL, "root cause should be NULL");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario 8: Full lifecycle — budget + cancel + outcome composition
 *
 * Simulates a task that:
 *   1. Starts with a budget
 *   2. Receives a cancellation mid-flight
 *   3. Gets a tighter cleanup budget from the cancel kind
 *   4. Completes cleanup within budget
 *   5. Produces a final outcome reflecting both cancellation and work
 * ------------------------------------------------------------------- */

static void scenario_full_lifecycle_composition(void) {
    SCENARIO_BEGIN("foundational.full_lifecycle_composition");

    /* Phase 1: Task starts with a work budget */
    asx_budget work_budget = asx_budget_from_polls(100);
    asx_outcome running_outcome = asx_outcome_make(ASX_OUTCOME_OK);
    int work_done = 0;

    /* Simulate some work (30 polls) */
    while (work_done < 30 && !asx_budget_is_exhausted(&work_budget)) {
        asx_budget_consume_poll(&work_budget);
        work_done++;
    }
    SCENARIO_CHECK(work_done == 30, "should complete 30 work polls");
    SCENARIO_CHECK(asx_budget_polls(&work_budget) == 70, "70 polls remaining");

    /* Phase 2: Cancellation arrives (timeout) */
    asx_cancel_reason cancel;
    memset(&cancel, 0, sizeof(cancel));
    cancel.kind = ASX_CANCEL_TIMEOUT;
    cancel.timestamp = 500;

    /* Record cancellation in outcome */
    asx_outcome cancel_outcome = asx_outcome_make(ASX_OUTCOME_CANCELLED);
    running_outcome = asx_outcome_join(&running_outcome, &cancel_outcome);
    SCENARIO_CHECK(asx_outcome_severity_of(&running_outcome) == ASX_OUTCOME_CANCELLED,
                   "outcome should reflect cancellation");

    /* Phase 3: Switch to cleanup budget */
    asx_budget cleanup_budget = asx_cancel_cleanup_budget(cancel.kind);
    SCENARIO_CHECK(!asx_budget_is_exhausted(&cleanup_budget),
                   "cleanup budget should not be pre-exhausted");

    /* Phase 4: Perform cleanup within budget */
    int cleanup_steps = 0;
    int max_cleanup = 10; /* only need a few steps */
    while (cleanup_steps < max_cleanup && !asx_budget_is_exhausted(&cleanup_budget)) {
        asx_budget_consume_poll(&cleanup_budget);
        cleanup_steps++;
    }
    SCENARIO_CHECK(cleanup_steps == max_cleanup, "should complete all cleanup steps");

    /* Phase 5: Final outcome */
    SCENARIO_CHECK(asx_outcome_severity_of(&running_outcome) == ASX_OUTCOME_CANCELLED,
                   "final outcome should be CANCELLED");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario 9: Public error taxonomy drives retry/escalation policy
 *
 * Exercises the user-facing status taxonomy helpers in a realistic
 * decision loop instead of only checking literal strings in unit tests.
 * ------------------------------------------------------------------- */

static void scenario_error_taxonomy_recovery_planning(void) {
    SCENARIO_BEGIN("foundational.error_taxonomy_recovery");

    asx_status transient = ASX_E_CHANNEL_FULL;
    asx_status permanent = ASX_E_PERMISSION_DENIED;
    asx_status contextual = ASX_E_RESOURCE_EXHAUSTED;
    asx_backoff_hint hint = asx_status_backoff_hint(transient);

    SCENARIO_CHECK(asx_status_category(transient) == ASX_ERROR_CATEGORY_CHANNEL,
                   "channel_full should classify as channel");
    SCENARIO_CHECK(asx_status_recoverability(transient) == ASX_RECOVERABILITY_TRANSIENT,
                   "channel_full should be transient");
    SCENARIO_CHECK(asx_status_recovery_action(transient) == ASX_RECOVERY_RETRY_IMMEDIATELY,
                   "channel_full should retry immediately");
    SCENARIO_CHECK(asx_status_is_retryable(transient), "channel_full should be retryable");
    SCENARIO_CHECK(hint.max_attempts > 0, "retryable status should have backoff guidance");

    SCENARIO_CHECK(asx_status_category(permanent) == ASX_ERROR_CATEGORY_PERMISSION,
                   "permission_denied should classify as permission");
    SCENARIO_CHECK(asx_status_recoverability(permanent) == ASX_RECOVERABILITY_PERMANENT,
                   "permission_denied should be permanent");
    SCENARIO_CHECK(asx_status_recovery_action(permanent) == ASX_RECOVERY_PROPAGATE,
                   "permission_denied should propagate");
    SCENARIO_CHECK(!asx_status_is_retryable(permanent),
                   "permission_denied should not be retryable");

    SCENARIO_CHECK(asx_status_recoverability(contextual) == ASX_RECOVERABILITY_CONTEXT_DEPENDENT,
                   "resource_exhausted should remain context-dependent");
    SCENARIO_CHECK(asx_status_recovery_action(contextual) == ASX_RECOVERY_RETRY_WITH_BACKOFF,
                   "resource_exhausted should suggest backoff");
    SCENARIO_CHECK(strcmp(asx_error_category_str(asx_status_category(contextual)), "resource") == 0,
                   "resource_exhausted category string should be stable");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario 10: Typed-symbol public contract through umbrella surface
 *
 * Exercises the shipped typed-symbol/value-contract API via <asx/asx.h>
 * instead of leaf headers. This keeps the foundational lane honest
 * about the public surface that downstream users actually see.
 * ------------------------------------------------------------------- */

static void scenario_typed_symbol_public_contract(void) {
    SCENARIO_BEGIN("foundational.typed_symbol_public_contract");

    asx_typed_symbol latency;
    asx_typed_symbol lookup;
    asx_symbol_set exported;

    asx_symbol_registry_reset();
    asx_symbol_set_init(&exported);

    SCENARIO_CHECK(asx_typed_symbol_register("runtime.latency_ns", ASX_TYPE_KIND_U64, 8u, 8u,
                                             &latency) == ASX_OK,
                   "typed symbol should register");
    SCENARIO_CHECK(latency.symbol != ASX_SYMBOL_INVALID, "typed symbol id should be valid");
    SCENARIO_CHECK(asx_type_kind_str(latency.kind) != NULL, "type kind string should exist");

    SCENARIO_CHECK(asx_typed_symbol_lookup("runtime.latency_ns", &lookup) == ASX_OK,
                   "typed symbol should look up by name");
    SCENARIO_CHECK(lookup.symbol == latency.symbol, "lookup should preserve symbol id");
    SCENARIO_CHECK(lookup.kind == ASX_TYPE_KIND_U64, "lookup should preserve type kind");
    SCENARIO_CHECK(lookup.size == 8u && lookup.alignment == 8u,
                   "lookup should preserve size/alignment");

    asx_symbol_set_insert(&exported, latency.symbol);
    SCENARIO_CHECK(asx_symbol_set_contains(&exported, latency.symbol),
                   "symbol set should contain exported typed symbol");
    SCENARIO_CHECK(asx_symbol_set_count(&exported) == 1u, "symbol set count should be stable");
    SCENARIO_CHECK(strcmp(asx_symbol_name(latency.symbol), "runtime.latency_ns") == 0,
                   "registered name should round-trip");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------- */

int main(void) {
    scenario_budget_bounded_lifecycle();
    scenario_cancel_strengthen_during_drain();
    scenario_cleanup_budget_enforcement();
    scenario_outcome_aggregation();
    scenario_handle_generation_safety();
    scenario_policy_driven_containment();
    scenario_cancel_chain_budget_interaction();
    scenario_full_lifecycle_composition();
    scenario_error_taxonomy_recovery_planning();
    scenario_typed_symbol_public_contract();

    printf("SCENARIO _summary %s total=%d pass=%d fail=%d\n", g_fail > 0 ? "fail" : "pass",
           g_pass + g_fail, g_pass, g_fail);

    return g_fail > 0 ? 1 : 0;
}
