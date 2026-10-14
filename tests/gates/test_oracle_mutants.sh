#!/usr/bin/env bash
# test_oracle_mutants.sh — the Rust-parity oracle must catch planted
# semantic bugs (oracle self-test, bd-9kll.2.7).
#
# Each case builds the conformance runner with one ASX_MUTANT_* macro, a
# deliberate semantic bug kept in the source behind #ifdef and compiled only
# here, runs every Rust-captured fixture through it, and expects the run to
# FAIL, with at least one failing fixture in the family the bug belongs to:
#   - BUDGET_PRIORITY_MIN       budget meet takes the lower priority   -> budget-*
#   - CANCEL_NO_STRENGTHEN      a cancel reason is never strengthened  -> cancel-*
#   - SCHED_REVERSE_ORDER       the lab's tie-break pick is mirrored   -> lab-*
#   - OBLIGATION_DROP_COMMITS   a dropped token commits, not leaks     -> obligation-*
#   - OK_BODY_CANCELS          an Ok body cancels its normal children -> region-body-ok-*
#   - EXCEPTIONAL_BODY_NO_CANCEL Err/Panicked close without FailFast    -> region-body-(err|panic)-*
#   - MASKED_SKIP_UNMASK       callback masking fails to restore       -> cancel-masked-*
#   - SWEEP_PARKED_DEADLINE_NO_WAKE the sweep never acts on a parked task's
#                              deadline before its own wake: neither the wake
#                              timer nor the round-start deadline pass reaches
#                              it (sweep only, compare-sweep)          -> any fixture
#                              the baseline sweep passes
#   - SWEEP_PARKED_DEADLINE_NO_TIMER only the wake timer is dropped, so a lone
#                              parked task is never moved to its deadline
#                              (sweep only, compare-sweep)  -> budget-deadline-parked-*
#   - CODEC_BIN_SNAPSHOT_TRUNCATED the BIN codec frame drops the snapshot's
#                              last byte, so every run's JSON and BIN round
#                              trips differ (bd-vhrp)         -> any fixture
#   - WATCH_BORROW_MARKS_SEEN  a watch borrow marks the version seen, as
#                              borrow_and_update does (bd-9kll.5.11); both
#                              fixtures that read has_changed after a borrow
#                              must fail, each on its own
#                              -> watch-borrow-vs-borrow-and-update-*,
#                                 watch-receiver-clone-continues-*
#   - STUCK_TASK_OMITTED       the stuck-task report leaves out a parked
#                              running task, so a run that ends with one is
#                              not named (bd-9kll.9.6)
#                              -> quiescence-parked-task-*,
#                                 oneshot-closed-send-no-wake-*,
#                                 deadlock-two-tasks-channels-*
#   - OUTCOME_JOIN_KEEPS_FIRST_REASON / OUTCOME_JOIN_ALWAYS_STRENGTHENS /
#     OUTCOME_JOIN_RIGHT_BIAS  the outcome join keeps the first Cancelled
#                              reason, strengthens on a tie, or lets the right
#                              win a tie (bd-9kll.3.14)
#                              -> region-close-outcome-join-cancelled-reasons-*,
#                                 region-close-outcome-join-panics-first-
#                                 message-wins-*
#   - INTERVAL_BURST_FROM_NOW / INTERVAL_DELAY_FROM_DEADLINE /
#     INTERVAL_SKIP_ONE_PERIOD one missed-tick rule is swapped for another's
#                              -> interval-missed-tick-<rule>-*,
#                                 interval-set-behavior-*
#   - INTERVAL_RESET_AFTER_IGNORES_NOW reset_after counts from zero
#                              -> interval-reset-family-*
#   - DELAY_QUEUE_NO_SELF_WAKE / DELAY_QUEUE_WAKE_ALWAYS /
#     DELAY_QUEUE_EMPTY_POLL_KEEPS_TIMER / DELAY_QUEUE_CLEAR_KEEPS_TIMER
#                              the queue's stored waker or armed deadline
#                              differs from Rust's, so an insert polls the
#                              task once too few or too many times
#                              -> delay-queue-wait-*
# A mutant the corpus does not catch means the fixtures stopped exercising
# that rule: the gate fails.
# The raw-ID authorization bypass is tested through actual polling tasks:
# Rust's affine token API cannot express possession of another holder's ID.
#
# Builds under build/mutants/<macro>/ (the normal build is untouched) and
# logs JSONL to build/test-logs/gates.jsonl. Nothing is deleted.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
RUN_ID="mutants-$(date -u +%Y%m%dT%H%M%SZ)-$$"
LOG_DIR="$REPO_ROOT/build/test-logs"
LOG="$LOG_DIR/gates.jsonl"
JOBS="${JOBS:-4}"
MUTANT_BUILD_ROOT="${ASX_MUTANT_BUILD_DIR:-build/mutants}"
if [[ "$MUTANT_BUILD_ROOT" != /* ]]; then
    MUTANT_BUILD_ROOT="$REPO_ROOT/$MUTANT_BUILD_ROOT"
fi
mkdir -p "$LOG_DIR"

failures=0

log_case() {
    # case expected observed status detail
    printf '{"ts":"%s","run_id":"%s","layer":"gates","suite":"oracle_mutants","test":"%s","expected":"%s","observed":"%s","status":"%s","detail":"%s"}\n' \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$RUN_ID" "$1" "$2" "$3" "$4" "$5" >>"$LOG"
}

# run_mutant <macro> <failing fixture family regex> [compare|compare-sweep]
#            [first-divergence regex]
# `compare-sweep` runs the corpus under the default sweep scheduler against
# the known-divergence list (bd-9kll.4.9): the mutant must turn a fixture the
# baseline passes into a FAIL, which lab dispatch cannot show for a sweep-only
# line. The optional last argument is a regex the output must also contain:
# the report names where the runs part, not only that they do (the dispatch
# index for a scheduling difference).
run_mutant() {
    local macro="$1" family="$2" mode="${3:-compare}" detail="${4:-}"
    local dir="$MUTANT_BUILD_ROOT/$macro"
    local out rc status observed fails
    mkdir -p "$dir"
    if ! make -C "$REPO_ROOT" -j"$JOBS" --no-print-directory BUILD_DIR="$dir" \
        CFLAGS="${CFLAGS:-} -DASX_MUTANT_$macro" "$dir/bin/asx-conformance" \
        >"$dir/build.log" 2>&1; then
        echo "  FAIL: $macro (the mutant does not build; log $dir/build.log)"
        log_case "$macro" "build" "build-failed" "fail" "$dir/build.log"
        failures=$((failures + 1))
        return
    fi
    if [ "$mode" = "compare-sweep" ]; then
        out="$(cd "$REPO_ROOT" && "$dir/bin/asx-conformance" compare-sweep \
            --known tests/conformance/sweep_known_divergences.txt \
            fixtures/rust_reference_v2/*.json 2>&1)"
    else
        out="$(cd "$REPO_ROOT" && "$dir/bin/asx-conformance" compare fixtures/rust_reference_v2/*.json 2>&1)"
    fi
    rc=$?
    printf '%s\n' "$out" >"$dir/compare.log"
    fails="$(grep -cE '^FAIL ' <<<"$out")"
    observed="exit$rc fail=$fails"
    status="pass"
    if [ "$rc" -eq 0 ]; then status="fail"; fi
    if ! grep -Eq -- "^FAIL ($family)" <<<"$out"; then status="fail"; fi
    if [ -n "$detail" ] && ! grep -Eq -- "$detail" <<<"$out"; then status="fail"; fi
    if [ "$status" = "pass" ]; then
        echo "  PASS: $macro caught ($fails fixture(s) fail, family /$family/ among them)"
    else
        echo "  FAIL: $macro not caught as expected ($observed; family /$family/; log $dir/compare.log)"
        failures=$((failures + 1))
    fi
    log_case "$macro" "nonzero:$family" "$observed" "$status" "$dir/compare.log"
}

echo "=== test_oracle_mutants ($RUN_ID) ==="

# The min meet gives a child a lower effective priority than Rust's max, so the
# lab polls it later: the report must name the first dispatch where the two
# runs part, with Rust's task and C's (bd-9kll.2.6).
run_mutant BUDGET_PRIORITY_MIN 'budget-' compare \
    '^    dispatch [0-9]+ differs \(rust has [0-9]+, c has [0-9]+\)$'
run_mutant CANCEL_NO_STRENGTHEN 'cancel-'
run_mutant SCHED_REVERSE_ORDER 'lab-'
run_mutant OBLIGATION_DROP_COMMITS 'obligation-'
run_mutant OK_BODY_CANCELS 'region-body-ok-'
run_mutant EXCEPTIONAL_BODY_NO_CANCEL 'region-body-(err|panic)-'
run_mutant MASKED_SKIP_UNMASK 'cancel-masked-'
# Lab dispatch never runs the sweep's parked-deadline paths, so only the sweep
# comparison can see this one; the list's divergences keep failing as KNOWN,
# so any FAIL line is a fixture the baseline passes. The macro disables both
# paths: the wake timer a task arms when it parks, and the round-start pass
# of sched_fire_budget_deadlines (d9a6e51), which cancels and wakes a parked
# task whenever another timer has moved the clock past its deadline. With the
# pass in place, dropping the timer alone was seen by no fixture in the corpus
# (batch #7): only a lone parked task, which nothing else wakes, needs it.
# Such a fixture exists since d22031c (budget-deadline-parked-barrier-001, a
# Rust capture), so the timer alone is a mutant of its own, caught by it.
run_mutant SWEEP_PARKED_DEADLINE_NO_WAKE '.' compare-sweep
run_mutant SWEEP_PARKED_DEADLINE_NO_TIMER 'budget-deadline-parked-' compare-sweep

# The runner puts every run through the JSON and the BIN codec (bd-vhrp): a
# BIN encoder that loses a byte must fail the corpus.
run_mutant CODEC_BIN_SNAPSHOT_TRUNCATED '.'

# A watch borrow reads without marking the value seen (Rust's watch.rs:904).
# Two captures observe it, and the mutant must fail each by itself, so a
# corpus that dropped one of them is noticed: has_changed after a borrow, and
# a clone that continues from the same version.
run_mutant WATCH_BORROW_MARKS_SEEN 'watch-borrow-vs-borrow-and-update-'
run_mutant WATCH_BORROW_MARKS_SEEN 'watch-receiver-clone-continues-'

# A run that ends with a task parked names it: the runner compares C's
# asx_scheduler_stuck_tasks with the tasks Rust's snapshot leaves not
# Completed, on every fixture (23 of the corpus end non-quiescent). Each of the
# three fixtures below must fail the mutant by itself; the last is a deliberate
# deadlock, two tasks each parked on a recv from the other (bd-9kll.9.6).
run_mutant STUCK_TASK_OMITTED 'quiescence-parked-task-001'
run_mutant STUCK_TASK_OMITTED 'oneshot-closed-send-no-wake-001'
run_mutant STUCK_TASK_OMITTED 'deadlock-two-tasks-channels-001'

# A region's close outcome is the join of its tasks' outcomes, and what the
# join keeps is observable (bd-9kll.3.14): two Cancelled outcomes strengthen the
# left reason only when the right kind is strictly more severe, and every other
# tie stays left, a panic's message included. The first mutant keeps the first
# reason, the second strengthens on a tie too (the earlier stamp wins), the
# third lets the right outcome win a tie.
run_mutant OUTCOME_JOIN_KEEPS_FIRST_REASON 'region-close-outcome-join-cancelled-reasons-'
run_mutant OUTCOME_JOIN_ALWAYS_STRENGTHENS 'region-close-outcome-join-cancelled-reasons-'
run_mutant OUTCOME_JOIN_RIGHT_BIAS 'region-close-outcome-join-panics-first-message-wins-'

# Each missed-tick rule of Rust's Interval (interval.rs:385-420) has a mutant
# that swaps it for a neighbour's, and the family of captures that pins it
# must fail by itself: Burst steps from the missed deadline, Delay from now,
# Skip to the first boundary after now. interval-set-behavior-001 switches
# between all three, so each mutant fails it as well.
run_mutant INTERVAL_BURST_FROM_NOW 'interval-missed-tick-burst-'
run_mutant INTERVAL_DELAY_FROM_DEADLINE 'interval-missed-tick-delay-'
run_mutant INTERVAL_SKIP_ONE_PERIOD 'interval-missed-tick-skip-'
run_mutant INTERVAL_BURST_FROM_NOW 'interval-set-behavior-'
run_mutant INTERVAL_DELAY_FROM_DEADLINE 'interval-set-behavior-'
run_mutant INTERVAL_SKIP_ONE_PERIOD 'interval-set-behavior-'
# reset_after counts from the clock, not from zero.
run_mutant INTERVAL_RESET_AFTER_IGNORES_NOW 'interval-reset-family-'

# The DelayQueue's stored waker (delay_queue.rs:332): an insert wakes it only
# when the queue needs a re-arm, which costs the task one more poll at that
# instant. A queue that never wakes, one that always wakes, and ones that
# keep the armed deadline across an empty poll or a clear each change the
# dispatch list of the capture that pins the rule.
run_mutant DELAY_QUEUE_NO_SELF_WAKE 'delay-queue-wait-self-wake-'
run_mutant DELAY_QUEUE_NO_SELF_WAKE 'delay-queue-wait-clear-sleep-'
run_mutant DELAY_QUEUE_WAKE_ALWAYS 'delay-queue-wait-reset-insert-'
run_mutant DELAY_QUEUE_EMPTY_POLL_KEEPS_TIMER 'delay-queue-wait-self-wake-'
run_mutant DELAY_QUEUE_CLEAR_KEEPS_TIMER 'delay-queue-wait-clear-sleep-'
# Each also leaves the timer registered after the queue dropped its Sleep, a
# trace and snapshot difference for a task that goes on instead of ending.
run_mutant DELAY_QUEUE_EMPTY_POLL_KEEPS_TIMER 'delay-queue-wait-drop-timer-'
run_mutant DELAY_QUEUE_CLEAR_KEEPS_TIMER 'delay-queue-wait-drop-timer-'

# The scenario parser cannot reach depth 64; the actual public boundary
# test must catch acceptance of depth 65, without a compile/empty-run pass.
MASK_DIR="$MUTANT_BUILD_ROOT/MASK_ACCEPTS_EXCESS_DEPTH"
MASK_TEST="$MASK_DIR/tests/unit/runtime/test_cancellation"
mkdir -p "$MASK_DIR"
if make -C "$REPO_ROOT" -j"$JOBS" --no-print-directory BUILD_DIR="$MASK_DIR" \
    CFLAGS="${CFLAGS:-} -DASX_MUTANT_MASK_ACCEPTS_EXCESS_DEPTH" "$MASK_TEST" \
    >"$MASK_DIR/build.log" 2>&1; then
    mask_out="$(cd "$REPO_ROOT" && "$MASK_TEST" 2>&1)"
    mask_rc=$?
    printf '%s\n' "$mask_out" >"$MASK_DIR/run.log"
    if [ "$mask_rc" -ne 0 ] &&
        grep -q '^  FAIL: masked_callback_refuses_excess_depth_without_running_body$' <<<"$mask_out" &&
        grep -Eq '^[0-9]+/[0-9]+ tests passed$' <<<"$mask_out"; then
        echo "  PASS: MASK_ACCEPTS_EXCESS_DEPTH caught by real mask-depth admission"
        log_case "MASK_ACCEPTS_EXCESS_DEPTH" "nonzero:excess_depth" "exit$mask_rc" "pass" "$MASK_DIR/run.log"
    else
        echo "  FAIL: MASK_ACCEPTS_EXCESS_DEPTH not caught (exit$mask_rc; log $MASK_DIR/run.log)"
        log_case "MASK_ACCEPTS_EXCESS_DEPTH" "nonzero:excess_depth" "exit$mask_rc" "fail" "$MASK_DIR/run.log"
        failures=$((failures + 1))
    fi
else
    echo "  FAIL: MASK_ACCEPTS_EXCESS_DEPTH does not build (log $MASK_DIR/build.log)"
    log_case "MASK_ACCEPTS_EXCESS_DEPTH" "build" "build-failed" "fail" "$MASK_DIR/build.log"
    failures=$((failures + 1))
fi

# A compile failure or empty run cannot count as catching the missing guard.
AUTH_DIR="$MUTANT_BUILD_ROOT/OBLIGATION_AUTH_BYPASS"
AUTH_TEST="$AUTH_DIR/tests/unit/runtime/test_budget_obligation"
mkdir -p "$AUTH_DIR"
if make -C "$REPO_ROOT" -j"$JOBS" --no-print-directory BUILD_DIR="$AUTH_DIR" \
    CFLAGS="${CFLAGS:-} -DASX_MUTANT_OBLIGATION_AUTH_BYPASS" "$AUTH_TEST" \
    >"$AUTH_DIR/build.log" 2>&1; then
    auth_out="$(cd "$REPO_ROOT" && "$AUTH_TEST" 2>&1)"
    auth_rc=$?
    printf '%s\n' "$auth_out" >"$AUTH_DIR/run.log"
    if [ "$auth_rc" -ne 0 ] &&
        grep -q '^  FAIL: non_holder_cannot_resolve_an_obligation_but_its_owner_can$' <<<"$auth_out" &&
        grep -Eq '^[0-9]+/[0-9]+ tests passed$' <<<"$auth_out"; then
        echo "  PASS: OBLIGATION_AUTH_BYPASS caught by real-task holder authorization"
        log_case "OBLIGATION_AUTH_BYPASS" "nonzero:non_holder" "exit$auth_rc" "pass" "$AUTH_DIR/run.log"
    else
        echo "  FAIL: OBLIGATION_AUTH_BYPASS not caught (exit$auth_rc; log $AUTH_DIR/run.log)"
        log_case "OBLIGATION_AUTH_BYPASS" "nonzero:non_holder" "exit$auth_rc" "fail" "$AUTH_DIR/run.log"
        failures=$((failures + 1))
    fi
else
    echo "  FAIL: OBLIGATION_AUTH_BYPASS does not build (log $AUTH_DIR/build.log)"
    log_case "OBLIGATION_AUTH_BYPASS" "build" "build-failed" "fail" "$AUTH_DIR/build.log"
    failures=$((failures + 1))
fi

# The parallel scheduler is not run by the conformance corpus (lab dispatch
# and the sweep are), so its mutants are caught by its own unit test, and
# each must fail the named cases (bd-9kll.4.11). run_unit_mutant builds one
# unit test with the macro and requires a nonzero exit, a summary line, and a
# `  FAIL: <case>` line for every case named: a compile failure or an empty
# run cannot count as caught.
run_unit_mutant() {
    local macro="$1" test_rel="$2"
    shift 2
    local dir="$MUTANT_BUILD_ROOT/$macro"
    local bin="$dir/tests/$test_rel"
    local out rc name missing=""
    mkdir -p "$dir"
    if ! make -C "$REPO_ROOT" -j"$JOBS" --no-print-directory BUILD_DIR="$dir" \
        CFLAGS="${CFLAGS:-} -DASX_MUTANT_$macro" "$bin" >"$dir/build.log" 2>&1; then
        echo "  FAIL: $macro does not build (log $dir/build.log)"
        log_case "$macro" "build" "build-failed" "fail" "$dir/build.log"
        failures=$((failures + 1))
        return
    fi
    out="$(cd "$REPO_ROOT" && "$bin" 2>&1)"
    rc=$?
    printf '%s\n' "$out" >"$dir/run.log"
    for name in "$@"; do
        grep -q "^  FAIL: $name\$" <<<"$out" || missing="$missing $name"
    done
    if [ "$rc" -ne 0 ] && [ -z "$missing" ] &&
        grep -Eq '^[0-9]+/[0-9]+ tests passed' <<<"$out"; then
        echo "  PASS: $macro caught by $test_rel ($# named case(s) fail)"
        log_case "$macro" "nonzero:$*" "exit$rc" "pass" "$dir/run.log"
    else
        echo "  FAIL: $macro not caught (exit$rc; not failing:${missing:- none}; log $dir/run.log)"
        log_case "$macro" "nonzero:$*" "exit$rc" "fail" "$dir/run.log"
        failures=$((failures + 1))
    fi
}

# A task a poll spawns joins a lane when it is created, so the run does not
# say OK with it never polled; a queued task that takes a cancel moves to the
# cancel lane at the next round; and a region that took a cancel refuses new
# work, so nothing is admitted into it.
run_unit_mutant PARALLEL_NO_MIDRUN_ADMISSION unit/runtime/test_parallel \
    parallel_run_does_not_return_ok_while_a_spawned_child_is_live \
    parallel_child_spawned_then_region_cancelled_is_dispatched_from_the_cancel_lane
run_unit_mutant PARALLEL_NO_CANCEL_PROMOTE unit/runtime/test_parallel \
    parallel_child_cancelled_after_admission_is_dispatched_from_the_cancel_lane \
    parallel_child_spawned_then_region_cancelled_is_dispatched_from_the_cancel_lane
run_unit_mutant SPAWN_INTO_CLOSING_REGION unit/runtime/test_parallel \
    parallel_spawn_into_a_cancelled_region_is_refused_and_admits_nothing
# Admission by a walk of the whole arena at every round runs the same tasks in
# the same lanes, so only the cost shows: the arena must be walked once per
# run, however many tasks a poll spawns (bd-9kll.4.11).
run_unit_mutant PARALLEL_RESCAN_ADMISSION unit/runtime/test_parallel \
    parallel_many_mid_run_spawns_walk_the_arena_once

# A reset visits only the used prefix of each arena once they were cleared in
# full (bd-9kll.14.2): the mutant leaves the last used slot as it was, and a
# heavy epoch, a reset, a light epoch, a reset and the heavy epoch again must
# then issue different handles.
run_unit_mutant CLEAR_SKIPS_USED_SLOTS unit/runtime/test_boundary_exhaustion \
    reset_after_heavy_then_light_use_leaves_no_slot_behind

# The sweep scheduler's dispatch order (bd-9kll.4.6): cancel lane, then the
# timed lane (a due wake timer's task, earliest deadline first, then arming
# order), then ready by budget priority. The first mutant is the walk of
# before the bead and is also the "arena" build of `make bench-sched-ab`.
run_unit_mutant SWEEP_ARENA_ORDER unit/runtime/test_scheduler \
    scheduler_polls_higher_budget_priority_first \
    scheduler_polls_the_cancel_lane_before_the_ready_lane \
    scheduler_polls_due_timers_earliest_deadline_first
run_unit_mutant SWEEP_NO_TIMED_LANE unit/runtime/test_scheduler \
    scheduler_polls_due_timers_earliest_deadline_first \
    scheduler_polls_a_due_timer_before_higher_priority_ready_work \
    scheduler_polls_timers_due_at_one_deadline_in_the_order_they_were_armed
run_unit_mutant SWEEP_TIMED_BEFORE_CANCEL unit/runtime/test_scheduler \
    scheduler_polls_the_cancel_lane_before_the_timed_lane
run_unit_mutant SWEEP_NO_LATE_CANCEL unit/runtime/test_scheduler \
    scheduler_polls_a_task_cancelled_this_round_before_the_rest_of_the_timed_lane

# A child's shutdown budget (bd-9kll.8.6): the ceiling a drain sets on a
# generation's region is met into the cleanup budget of every task that
# cancel cancels, is inherited by the region's descendants, and only
# tightens. The conformance corpus shows the first two through a drained
# generation's tasks (supervision-shutdown-budget-*).
run_unit_mutant SHUTDOWN_CEILING_IGNORED unit/runtime/test_cancellation \
    region_cancel_ceiling_meets_into_the_cleanup_budget_of_its_tasks \
    region_cancel_ceiling_is_inherited_by_descendant_regions \
    region_cancel_ceilings_only_tighten
run_unit_mutant SHUTDOWN_CEILING_NOT_INHERITED unit/runtime/test_cancellation \
    region_cancel_ceiling_is_inherited_by_descendant_regions
run_unit_mutant SHUTDOWN_CEILING_LOOSENS unit/runtime/test_cancellation \
    region_cancel_ceilings_only_tighten
run_mutant SHUTDOWN_CEILING_IGNORED 'supervision-shutdown-budget-'
run_mutant SHUTDOWN_CEILING_NOT_INHERITED 'supervision-shutdown-budget-reaches-'

# CapabilityBudget (bd-9kll.3.27): an envelope can only tighten down the
# tree, a spent one is refused when required, a refused child takes nothing,
# and a refused apply leaves the Cx as it was. The conformance corpus has no
# scenario that reads an envelope, so each is caught by its unit test.
run_unit_mutant CAPABILITY_MEET_LOOSENS unit/core/test_budget \
    capability_child_inherits_parent_and_tightens_overrides \
    capability_meet_never_loosens
run_unit_mutant CAPABILITY_SPENT_ADMITS unit/core/test_budget \
    capability_required_exhausted_dimension_rejects \
    capability_requirement_is_checked_against_the_meet
run_unit_mutant CAPABILITY_SLOT_BEFORE_REFUSAL unit/runtime/test_budget_obligation \
    capability_budget_refused_child_is_failure_atomic
run_unit_mutant CAPABILITY_APPLY_ON_REFUSAL unit/cx/test_cx \
    capability_budget_apply_child_refusal_leaves_the_cx_unchanged
# A task's open of a child region carries the Cx's envelope met with the
# spec's (cx.rs:4514-4516), and a command that carries an envelope is planned
# when the scheduler applies it, so a refusal reaches the opener parked.
run_unit_mutant CAPABILITY_CX_OPEN_IGNORES_CX unit/cx/test_cx \
    open_child_region_sends_the_cx_envelope_met_with_the_spec \
    open_child_region_does_not_replenish_a_spent_cx
run_unit_mutant CAPABILITY_LAB_OPEN_DROPS_ENVELOPE unit/runtime/test_lab_dispatch \
    capability_open_refusal_reaches_the_opener_after_it_parked

if [ "$failures" -eq 0 ]; then
    echo "test_oracle_mutants: every mutant caught"
    exit 0
fi
echo "test_oracle_mutants: $failures mutant(s) not caught"
exit 1
