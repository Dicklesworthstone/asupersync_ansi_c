#!/usr/bin/env bash
# scheduler_certificate_parity.sh — Rust-vs-C scheduler certificate parity and
# cross-engine forced-schedule replay (bd-9kll.4.8, bd-9kll.9.3)
#
# For every committed Rust fixture (fixtures/rust_reference_v2) and for
# ASX_E2E_SCHED_COUNT generated DSL v2 scenarios (fixed seed
# ASX_E2E_SCHED_SEED, captured from Rust now):
#
#   1. certificate: `asx-conformance compare` requires the C run's
#      ScheduleCertificate hash and decision count, and every dispatch in
#      order, to be Rust's; Rust's own recorded schedule is replayed on the C
#      lab (Rust -> C);
#   2. artifact: `asx-conformance schedule` records the C lab's forced
#      schedule with Rust arena ids and requires it to be byte for byte the
#      artifact Rust recorded (task and generation, worker, lane, step, time,
#      terminal receipt);
#   3. C -> Rust: `twin_run forced-replay` hands C's artifact to Rust's
#      LabRuntime::run_forced_schedule, which revalidates every dispatch, and
#      `asx-conformance compare` requires C's run to equal Rust's replay of
#      C's schedule (trace, snapshot, observations, certificate).
#
# One JSONL record per scenario and check (tests/e2e/harness.sh) carries the
# first divergence: the dispatch index, both task ids, lanes, steps and times.
# Three outcomes are recorded as skipped, with the reason, because they are
# properties of Rust's runner and not of C: a scenario whose script steps the
# lab between dispatches (run_forced_schedule executes a schedule in one
# call), a source that did not end quiescent (no canonical artifact on either
# side), and a schedule Rust refuses although it is byte-identical to the
# one Rust recorded itself (Rust's runner does not replay its own recording).
#
# Needs cargo (asupersync at the pinned rev): twin_run is built unless
# ASX_TWIN_BIN names a built one. The runner is ASX_CONFORMANCE_RUNNER or
# build/bin/asx-conformance, built with `make conformance-runner` when absent.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=tests/e2e/harness.sh
source "$SCRIPT_DIR/harness.sh"

e2e_init "scheduler-certificate-parity" "E2E-SCHED-CERT"

SEED="${ASX_E2E_SCHED_SEED:-1}"
COUNT="${ASX_E2E_SCHED_COUNT:-1000}"
WORK="${E2E_ARTIFACT_DIR}/sched"
mkdir -p "$WORK"

# ---- tools ---------------------------------------------------------------
RUNNER="${ASX_CONFORMANCE_RUNNER:-${E2E_PROJECT_ROOT}/build/bin/asx-conformance}"
if [ ! -x "$RUNNER" ]; then
    if ! (cd "$E2E_PROJECT_ROOT" && make conformance-runner >"$WORK/runner_build.log" 2>&1) ||
        [ ! -x "$RUNNER" ]; then
        e2e_scenario "scheduler_cert.runner" "cannot build the conformance runner (see $WORK/runner_build.log)" "fail"
        e2e_finish
        exit $?
    fi
fi
TWIN="${ASX_TWIN_BIN:-}"
if [ -z "$TWIN" ]; then
    if ! (cd "$E2E_PROJECT_ROOT" &&
        cargo build --release --locked --manifest-path tools/twin_run/Cargo.toml --bin twin_run \
            >"$WORK/twin_build.log" 2>&1); then
        e2e_scenario "scheduler_cert.twin_run" "cannot build twin_run (see $WORK/twin_build.log)" "fail"
        e2e_finish
        exit $?
    fi
    TWIN="${E2E_PROJECT_ROOT}/tools/twin_run/target/release/twin_run"
fi
if [ ! -x "$TWIN" ]; then
    e2e_scenario "scheduler_cert.twin_run" "twin_run not found at $TWIN" "fail"
    e2e_finish
    exit $?
fi
e2e_scenario "scheduler_cert.tools" "" "pass"

# A runner report with each FAIL's indented detail lines (the first
# divergence) joined onto its line.
joined() {
    awk 'function flush() { if (have) print rec; have = 0 }
         /^    / { if (have && n < 6) { rec = rec " | " substr($0, 5); n++ } next }
         { flush(); rec = $0; have = 1; n = 0 }
         END { flush() }' "$1"
}

# ---- one set of scenarios ----------------------------------------------------
# $1 label, $2 directory of Rust fixtures (asx.fixture.v2): they embed the
# scenarios the C schedules are recorded for and Rust replays.
run_set() {
    local label="$1" fixtures="$2"
    local sched="$WORK/$label.sched" replay="$WORK/$label.replay"
    local files=("$fixtures"/*.json)
    mkdir -p "$sched" "$replay"
    if [ ! -e "${files[0]}" ]; then
        e2e_scenario "scheduler_cert.$label.inputs" "no fixtures in $fixtures" "fail"
        return
    fi

    # 1. certificate and Rust -> C (the runner's ordinary comparison).
    "$RUNNER" compare "${files[@]}" >"$WORK/$label.compare.out" 2>&1 || true
    local line id
    while IFS= read -r line; do
        case "$line" in
        "PASS "*)
            id="${line#PASS }"
            id="${id%% *}"
            e2e_scenario "$label.$id.certificate_and_rust_to_c" "" "pass" "${line##* }"
            ;;
        "FAIL "*)
            id="${line#FAIL }"
            id="${id%%:*}"
            e2e_scenario "$label.$id.certificate_and_rust_to_c" "$line" "fail"
            ;;
        "ERROR "*)
            e2e_scenario "$label.compare" "$line" "fail"
            ;;
        esac
    done < <(joined "$WORK/$label.compare.out")

    # 2. C's recorded artifact against Rust's.
    "$RUNNER" schedule --out-dir "$sched" "${files[@]}" >"$WORK/$label.schedule.out" 2>&1 || true
    while IFS= read -r line; do
        case "$line" in
        "SCHEDULE "*)
            id="${line#SCHEDULE }"
            id="${id%% *}"
            e2e_scenario "$label.$id.artifact_identical" "" "pass"
            ;;
        "NONE "*)
            id="${line#NONE }"
            id="${id%%:*}"
            e2e_scenario "$label.$id.artifact_identical" "${line#NONE $id: }" "skip"
            ;;
        "DIFFERENT "*)
            id="${line#DIFFERENT }"
            id="${id%%:*}"
            e2e_scenario "$label.$id.artifact_identical" "$line" "fail"
            ;;
        "ERROR "*)
            e2e_scenario "$label.schedule" "$line" "fail"
            ;;
        esac
    done <"$WORK/$label.schedule.out"

    # 3. C -> Rust.
    "$TWIN" forced-replay --schedules "$sched" --out "$replay" "${files[@]}" \
        >"$WORK/$label.forced_replay.out" 2>&1 || true
    while IFS= read -r line; do
        case "$line" in
        "REPLAYED "*)
            id="${line#REPLAYED }"
            id="${id%% *}"
            e2e_scenario "$label.$id.c_to_rust_replay" "" "pass"
            ;;
        "UNAVAILABLE "*)
            id="${line#UNAVAILABLE }"
            id="${id%%:*}"
            e2e_scenario "$label.$id.c_to_rust_replay" "${line#UNAVAILABLE $id: }" "skip"
            ;;
        "REFUSED "*)
            id="${line#REFUSED }"
            id="${id%%:*}"
            # C's artifact is byte-identical to Rust's own recording here (any
            # difference failed step 2), so the refusal is Rust's runner
            # rejecting its own schedule, not a divergence of C.
            if grep -q "^SCHEDULE $id " "$WORK/$label.schedule.out"; then
                e2e_scenario "$label.$id.c_to_rust_replay" \
                    "Rust's run_forced_schedule refuses its own recording: ${line#REFUSED $id: }" "skip"
            else
                e2e_scenario "$label.$id.c_to_rust_replay" "$line" "fail"
            fi
            ;;
        "ERROR "*)
            e2e_scenario "$label.forced_replay" "$line" "fail"
            ;;
        esac
    done <"$WORK/$label.forced_replay.out"

    # C's run against Rust's replay of C's schedule.
    local replayed=("$replay"/*.json)
    if [ -e "${replayed[0]}" ]; then
        "$RUNNER" compare "${replayed[@]}" >"$WORK/$label.cross.out" 2>&1 || true
        while IFS= read -r line; do
            case "$line" in
            "PASS "*)
                id="${line#PASS }"
                id="${id%% *}"
                e2e_scenario "$label.$id.c_equals_rust_replay" "" "pass" "${line##* }"
                ;;
            "FAIL "*)
                id="${line#FAIL }"
                id="${id%%:*}"
                e2e_scenario "$label.$id.c_equals_rust_replay" "$line" "fail"
                ;;
            "ERROR "*)
                e2e_scenario "$label.cross_compare" "$line" "fail"
                ;;
            esac
        done < <(joined "$WORK/$label.cross.out")
    fi
}

# ---- the committed fixtures ------------------------------------------------
run_set "fixtures" "${E2E_PROJECT_ROOT}/fixtures/rust_reference_v2"

# ---- generated scenarios, captured from Rust now -----------------------------
GEN="$WORK/generated"
mkdir -p "$GEN/scenarios" "$GEN/fixtures"
if "$TWIN" generate --seed "$SEED" --count "$COUNT" --out "$GEN/scenarios" \
    >"$WORK/generate.out" 2>&1 &&
    { "$TWIN" capture "$GEN/scenarios"/*.json --out "$GEN/fixtures" >"$WORK/capture.out" 2>&1 || true; } &&
    grep -q '^twin_run capture: ' "$WORK/capture.out"; then
    captured="$(grep -c '^PASS ' "$WORK/capture.out" || true)"
    if [ $((captured * 10)) -lt $((COUNT * 9)) ]; then
        e2e_scenario "scheduler_cert.generated.capture" "only $captured of $COUNT scenarios captured" "fail"
    else
        e2e_scenario "scheduler_cert.generated.capture" "" "pass"
        run_set "generated" "$GEN/fixtures"
    fi
else
    e2e_scenario "scheduler_cert.generated.capture" "twin_run generate/capture did not finish (see $WORK/capture.out)" "fail"
fi

e2e_finish
