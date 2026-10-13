# Static analysis triage

> Plan section 10.7.2 (Static Analyzer Gate), bead bd-9kll.16.4. What each
> analyzer lane covers, how a finding is triaged, and the register of every
> documented false positive with its rationale.

## 1. The rule

Run at least one static analyzer over the core and kernel paths. A finding is
exactly one of:

| class | meaning | CI |
|---|---|---|
| must-fix | a real defect, or a diagnostic the code can be changed to stop provoking | the lane fails until it is fixed |
| waived | a documented false positive, with a rationale at the line | passes, and is listed in section 4 |
| backlog | a real finding deferred | **not allowed**: no lane has this class; kernel and hard-part paths forbid it by plan, and the rest of `src/` is held to the same rule |

So a diagnostic is either fixed or carries an `ASX_ANALYZER_WAIVER` comment
that says why the analyzer is wrong, and every unwaived diagnostic fails CI.
Nothing is parked.

### Waiver mechanism

A comment containing `ASX_ANALYZER_WAIVER("reason: ...")` within **3 lines
above, or 1 line below**, the line the analyzer reports (the same window for
all three lanes). One line, at most 100 columns (clang-format's limit), and the
reason names the invariant that makes the diagnostic false. Prefer changing the
code when the change is cheap and off the hot path; a `memset` on the
scheduler's pick path, or a restructure of a loop that is correct, is not.

## 2. The lanes

| lane | tool (pinned) | scope | configurations | where | make target |
|---|---|---|---|---|---|
| cppcheck | cppcheck 2.22.0 | all of `src/` | its own (all `#ifdef` branches) | `check` job | `lint-static-analysis` |
| clang-tidy | clang-tidy 18.1.8: `bugprone-*`, `cert-*`, `misc-*`, `performance-*`, `portability-*` (minus the exclusions in `tools/ci/run_static_analysis.sh`) | all of `src/` | CORE, JSON, deterministic | `check` job | `lint-static-analysis` |
| GCC `-fanalyzer` | GCC 15.2 (the `gcc:15.2.0` image) | every `src/` unit of `make build` | `CORE` (`PROFILE=CORE DETERMINISTIC=1`) and `POSIX_LIVE` (`PROFILE=POSIX DETERMINISTIC=0`), the build's own flags | `gcc-analyzer` job | `lint-gcc-analyzer` |

`tools/ci/run_gcc_analyzer.sh` takes its compile commands from
`make -n build`, so a define or profile added to the Makefile reaches the lane
without an edit; it drops `-Werror` so every diagnostic of a unit is
collected, and it fails (exit 2) when a unit does not compile, never passing
a unit it did not analyze. The analyzer's findings change between GCC
releases, so the compiler is pinned: a different GCC is a warning locally and
a failure under `STRICT_GATES=1` / CI.

## 3. How a new finding is triaged

1. Reproduce: `make lint-gcc-analyzer` (or `tools/ci/run_static_analysis.sh`).
   The report lists each location (`build/gcc-analyzer/report.txt`).
2. Read the analyzer's path (`-fdiagnostics-path-format=separate-events` is
   what the lane prints into `build/gcc-analyzer/<CONFIG>/diag/`). A path that
   needs an input the API refuses, or an invariant a caller maintains, is a
   false positive; a path a caller can reach is a defect: fix it.
3. A false positive gets the comment and a row in section 4 in the same
   commit, naming the invariant. A defect is fixed, with a test where the
   path can be driven.

## 4. Register of waived diagnostics

### GCC `-fanalyzer` (gcc 15.2)

Seven locations over the two configurations (measured at `ac534ab`, 128 units
in CORE and 133 in POSIX live; waived in `a769c73`). Lines drift, so each row
names the function.

| location | diagnostic | configurations | why it is a false positive |
|---|---|---|---|
| `src/actor/actor.c`, `name_cleanup`, `key = order[i]` | use of uninitialized value `order[i]` | both | the collecting loop writes `order[n]` and then `n++`, so `order[0..n)` is written before the sort reads `i < n`; the analyzer does not relate a count to an initialized prefix |
| `src/runtime/lab_dispatch.c`, `lab_pop`, `group[r % n]` | use of uninitialized `group[<unknown>]` | both | `group[0..n)` holds every top-priority entry of a lane with `lane->n > 0`, so `n >= 1` and the index is below `n` |
| `src/runtime/lab_dispatch.c`, region command batch, `retained[i]` | use of uninitialized `retained[i]` | both | `lab_apply_retain` writes `woken[(*n)++]` before the loop reads `i < n_retained` |
| `src/runtime/lab_dispatch.c`, region command batch, `watched[i]` | use of uninitialized `watched[i]` | both | `lab_apply_watch` writes `woken[(*n)++]` before the loop reads `i < n_watched` |
| `src/runtime/journal.c`, `j_stage`, `memcpy` (8 inlined copies, one line) | use of uninitialized `src` | both | each caller stages exactly `p - rec` bytes that its `put_u8/16/32/64` and `put_bytes` chain wrote into the record buffer |
| `src/runtime/blocking.c`, `pool_lock` | infinite loop | CORE | in a single-thread build the compare-exchange can fail only if `pool_lock` re-entered itself, which no caller does |
| `src/platform/posix/signal_posix.c`, `sig_ensure_pipe` | leak of file descriptor `fds[1]` | POSIX live | the write end is stored in `g_pipe_wr`, a `volatile sig_atomic_t` the signal handler reads; the analyzer does not follow that store |

### cppcheck and clang-tidy

Their waivers are the `ASX_ANALYZER_WAIVER` comments in the source, each with
its reason (`grep -rn ASX_ANALYZER_WAIVER src`). The two lanes fail on any
finding without one, so the source is the register; this document does not
copy them.

## 5. History

| date | tree | measurement | outcome |
|---|---|---|---|
| 2026-10-10 | `c0c2fab` | `-fanalyzer` over the library, gcc 13 and 15, CORE and POSIX live | 4 findings, all false positives (`lab_dispatch.c` group, `signal_posix.c` fd, `blocking.c` loop, `trace.c` text on gcc 13 only). No lane added: findings differ across GCC releases and a blocking lane needs a pinned compiler |
| 2026-10-11 | | clang-tidy 18.1.8 with `clang-analyzer-*` only, over core, runtime, channel, time, sync, actor | 13 findings: 10 `optin.performance.Padding` notes, 1 `EnumCastOutOfRange` on `io_driver`'s flag enum, 2 path findings (a codec buffer with `data` NULL and `cap` > 0, which init and reset never produce; `tasks_per_shard` 0, which `normalize_locality_config` prevents and `parallel.c` now guards) |
| 2026-10-13 | `ac534ab` | `-fanalyzer`, gcc 15.2, whole `src/`, CORE and POSIX live | 7 locations, all false positives; waived in section 4; the `gcc-analyzer` lane pins gcc 15.2 and blocks new ones |
