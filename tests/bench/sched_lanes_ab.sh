#!/usr/bin/env bash
# sched_lanes_ab.sh — before and after of the sweep's dispatch order
# (bd-9kll.4.6, the bench the bead asks for)
#
# The sweep scheduler (asx_scheduler_run) polled tasks in arena order and
# ignored budget priority. It now polls by lane (cancel, then the timed lane,
# then ready by priority). This script builds the library three ways from the
# one tree, in release (-O2) CORE deterministic builds of their own:
#
#   arena   -DASX_MUTANT_SWEEP_ARENA_ORDER      the walk of before 4.6
#   lanes   -DASX_MUTANT_SWEEP_NO_TIMED_LANE    priority and cancel lanes only
#   timed   (no define)                         the shipped order
#
# (the macros are the planted bugs of tests/gates/test_oracle_mutants.sh; a
# product build defines none of them), builds tests/bench/bench_sched_lanes.c
# against each, and prints two tables of the same four workloads, whose polls
# are the same in every build and only their order can differ:
#
#   instructions per poll   valgrind --tool=callgrind over asx_scheduler_run
#                           only: deterministic, so a change of 3% is a
#                           change. Printed when valgrind is present; the
#                           budget is judged on this table.
#   ns per poll             the median of --runs interleaved runs (DEC-016's
#                           median of five; the three builds alternate so
#                           drift lands on all of them). Noisy on a shared
#                           machine: each of these polls is ~30 ns.
#
#   make bench-sched-ab
#   make bench-sched-ab SCHED_AB_ARGS="--runs 9 --gate"
#   rch exec -- make bench-sched-ab
#
# Budgets, shipped build over the arena build, per workload. A plain round
# (ready_equal: one priority, no cancel, no timer, the round the sweep has
# always had) costs one pass over the tasks more: --plain-budget-pct, default
# 5. A round with priorities, cancels or timers in play builds its order, a
# per-task cost of about a hundred instructions that a no-op poll shows and a
# real poll (a channel operation is thousands) does not: --budget-pct, default
# 25.
#
# Arguments: --runs N (odd, default 5), --reps N (repetitions inside one run,
# default 15), --tasks T, --plain-budget-pct P, --budget-pct P, --no-ir (skip
# the instruction table), --gate (exit 1 when a workload of the shipped build
# is OVER its budget, on instructions when they were counted, else on ns).
#
# Environment: SCHED_AB_BUILD_DIR (default build/bench-sched-ab), MAKE, CC.
# Exit: 0; 1 with --gate and an OVER; 2 usage; the build's status on failure.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="${SCHED_AB_BUILD_DIR:-build/bench-sched-ab}"
RUNS=5
REPS=15
TASKS=""
BUDGET_PCT=25
PLAIN_BUDGET_PCT=5
GATE=0
IR=1

while [ $# -gt 0 ]; do
    case "$1" in
        --runs) RUNS="${2:?--runs N}"; shift 2 ;;
        --reps) REPS="${2:?--reps N}"; shift 2 ;;
        --tasks) TASKS="${2:?--tasks T}"; shift 2 ;;
        --budget-pct) BUDGET_PCT="${2:?--budget-pct P}"; shift 2 ;;
        --plain-budget-pct) PLAIN_BUDGET_PCT="${2:?--plain-budget-pct P}"; shift 2 ;;
        --no-ir) IR=0; shift ;;
        --gate) GATE=1; shift ;;
        *)
            echo "sched_lanes_ab.sh: unknown argument: $1" >&2
            exit 2
            ;;
    esac
done
case "$RUNS" in ''|*[!0-9]*) echo "sched_lanes_ab.sh: --runs must be a number" >&2; exit 2 ;; esac
if [ "$RUNS" -lt 3 ] || [ $((RUNS % 2)) -eq 0 ]; then
    echo "sched_lanes_ab.sh: --runs must be odd and at least 3 (got $RUNS)" >&2
    exit 2
fi
command -v jq >/dev/null 2>&1 || { echo "sched_lanes_ab.sh: jq is required" >&2; exit 2; }
command -v valgrind >/dev/null 2>&1 || IR=0

VARIANTS=(arena lanes timed)
declare -A DEFINE=(
    [arena]="-DASX_MUTANT_SWEEP_ARENA_ORDER"
    [lanes]="-DASX_MUTANT_SWEEP_NO_TIMED_LANE"
    [timed]=""
)

mkdir -p "${ROOT}/${BUILD}/runs"
for v in "${VARIANTS[@]}"; do
    dir="${BUILD}/${v}"
    mkdir -p "${ROOT}/${dir}"
    echo "[asx] bench-sched-ab: building the ${v} library (${dir})..." >&2
    "${MAKE:-make}" -C "$ROOT" --no-print-directory build PROFILE=CORE DETERMINISTIC=1 \
        BUILD_TYPE=release BUILD_DIR="$dir" CFLAGS="${DEFINE[$v]}" \
        >"${ROOT}/${dir}/lib.log" 2>&1 || {
        echo "[asx] bench-sched-ab: the ${v} library build failed; see ${dir}/lib.log" >&2
        tail -20 "${ROOT}/${dir}/lib.log" >&2
        exit 1
    }
    # The bench gets exactly the library's flags (struct layouts must match).
    lib_cflags="$(sed -n 's/^ALL_CFLAGS=//p' "${ROOT}/${dir}/.build-config")"
    # shellcheck disable=SC2086
    ${CC:-cc} $lib_cflags -Wno-unused-result -I"${ROOT}/include" \
        "${ROOT}/tests/bench/bench_sched_lanes.c" \
        "${ROOT}/${dir}/lib/libasx.a" -o "${ROOT}/${dir}/bench_sched_lanes"
    # shellcheck disable=SC2086
    ${CC:-cc} $lib_cflags -Wno-unused-result -I"${ROOT}/include" \
        "${ROOT}/tests/bench/bench_runtime.c" \
        "${ROOT}/${dir}/lib/libasx.a" -lm -o "${ROOT}/${dir}/bench_runtime"
done

args=(--reps "$REPS")
[ -n "$TASKS" ] && args+=(--tasks "$TASKS")

run=1
while [ "$run" -le "$RUNS" ]; do
    for v in "${VARIANTS[@]}"; do
        echo "[asx] bench-sched-ab: run ${run} of ${RUNS}: ${v}" >&2
        "${ROOT}/${BUILD}/${v}/bench_sched_lanes" "${args[@]}" >"${ROOT}/${BUILD}/runs/${v}-${run}.json"
        "${ROOT}/${BUILD}/${v}/bench_runtime" --json >"${ROOT}/${BUILD}/runs/rt-${v}-${run}.json" 2>/dev/null
    done
    run=$((run + 1))
done

WORKLOADS="$(jq -r '.workloads | keys_unsorted[]' "${ROOT}/${BUILD}/runs/arena-1.json")"
tasks_used="$(jq -r '.tasks' "${ROOT}/${BUILD}/runs/arena-1.json")"

# median_ns <variant> <workload>: the middle of the runs' ns_per_poll.
median_ns() {
    local v="$1" w="$2" i
    for ((i = 1; i <= RUNS; i++)); do
        jq -r --arg w "$w" '.workloads[$w].ns_per_poll' "${ROOT}/${BUILD}/runs/${v}-${i}.json"
    done | sort -g | sed -n "$(((RUNS + 1) / 2))p"
}

polls_of() { jq -r --arg w "$2" '.workloads[$w].polls' "${ROOT}/${BUILD}/runs/${1}-1.json"; }

# ir_per_poll <variant> <workload>: instructions executed inside
# asx_scheduler_run for one repetition, over its polls.
ir_per_poll() {
    local v="$1" w="$2" out ir p
    out="$(valgrind --tool=callgrind --toggle-collect=asx_scheduler_run \
        --callgrind-out-file="${ROOT}/${BUILD}/runs/cg-${v}-${w}.out" \
        "${ROOT}/${BUILD}/${v}/bench_sched_lanes" --reps 1 --only "$w" ${TASKS:+--tasks "$TASKS"} \
        2>&1 >/dev/null)" || true
    ir="$(printf '%s\n' "$out" | sed -n 's/.*Collected : \([0-9][0-9]*\).*/\1/p' | head -1)"
    p="$(polls_of "$v" "$w")"
    if [ -z "$ir" ] || [ "$p" -eq 0 ]; then echo 0; else awk -v i="$ir" -v p="$p" 'BEGIN { printf "%.1f", i / p }'; fi
}

# table <title> <function>: one row per workload, the verdict on timed/arena.
over=0
table() {
    local title="$1" fn="$2" w a l t la tl ta verdict budget
    echo
    echo "$title"
    printf '%-12s %8s %12s %12s %12s %10s %10s %10s\n' workload polls arena lanes timed \
        "lanes/arena" "timed/lanes" "timed/arena"
    for w in $WORKLOADS; do
        a="$($fn arena "$w")"
        l="$($fn lanes "$w")"
        t="$($fn timed "$w")"
        budget="$BUDGET_PCT"
        if [ "$w" = "ready_equal" ]; then budget="$PLAIN_BUDGET_PCT"; fi
        read -r la tl ta verdict < <(awk -v a="$a" -v l="$l" -v t="$t" -v b="$budget" 'BEGIN {
            if (a == 0 || l == 0) { print "n/a n/a n/a n/a"; exit }
            la = (l - a) * 100.0 / a; tl = (t - l) * 100.0 / l; ta = (t - a) * 100.0 / a;
            printf "%+.1f%% %+.1f%% %+.1f%% %s\n", la, tl, ta, (ta > b ? "OVER" : "ok")
        }')
        printf '%-12s %8s %12s %12s %12s %10s %10s %10s  %s\n' "$w" "$(polls_of arena "$w")" \
            "$a" "$l" "$t" "$la" "$tl" "$ta" "$verdict"
        if [ "$verdict" = "OVER" ]; then over=1; fi
    done
}

echo
echo "sched lanes A/B: ${tasks_used} tasks; budget: the shipped (timed) build at most ${PLAIN_BUDGET_PCT}% over the arena build in ready_equal (a plain round), ${BUDGET_PCT}% in the others"
if [ "$IR" -eq 1 ]; then
    over=0
    table "instructions per poll (callgrind, asx_scheduler_run only, one repetition)" ir_per_poll
    ir_over=$over
fi
over=0
table "ns per poll (median of ${RUNS} interleaved runs of ${REPS} repetitions)" median_ns
ns_over=$over
# The perf gates' scheduler scenarios (tests/bench/bench_runtime.c: ASX_MAX_TASKS
# no-op tasks in one round, or yielding over several): the median p50 and p99
# of the runs, ns per whole scenario. These rounds are plain (one priority, no
# cancel, no timer).
rt_median() {
    local v="$1" s="$2" m="$3" i
    for ((i = 1; i <= RUNS; i++)); do
        jq -r --arg s "$s" --arg m "$m" '.benchmarks[$s][$m]' "${ROOT}/${BUILD}/runs/rt-${v}-${i}.json"
    done | sort -g | sed -n "$(((RUNS + 1) / 2))p"
}
echo
echo "bench_runtime scheduler scenarios (median of ${RUNS} interleaved runs; ns per scenario)"
printf '%-24s %-7s %10s %10s %10s %10s %10s\n' scenario metric arena lanes timed "lanes/arena" "timed/arena"
for s in scheduler_single_task scheduler_multi_task scheduler_multi_round; do
    for m in p50_ns p99_ns; do
        a="$(rt_median arena "$s" "$m")"
        l="$(rt_median lanes "$s" "$m")"
        t="$(rt_median timed "$s" "$m")"
        read -r la ta < <(awk -v a="$a" -v l="$l" -v t="$t" 'BEGIN {
            if (a == 0) { print "n/a n/a"; exit }
            printf "%+.1f%% %+.1f%%\n", (l - a) * 100.0 / a, (t - a) * 100.0 / a
        }')
        printf '%-24s %-7s %10s %10s %10s %10s %10s\n' "$s" "${m%_ns}" "$a" "$l" "$t" "$la" "$ta"
    done
done
echo
echo "runs: ${BUILD}/runs/<variant>-<n>.json, rt-<variant>-<n>.json, cg-<variant>-<workload>.out"
verdict_over=$ns_over
if [ "$IR" -eq 1 ]; then verdict_over=$ir_over; fi
if [ "$GATE" -eq 1 ] && [ "$verdict_over" -eq 1 ]; then
    echo "[asx] bench-sched-ab: OVER budget" >&2
    exit 1
fi
exit 0
