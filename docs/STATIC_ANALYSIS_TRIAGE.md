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
| cppcheck | cppcheck 2.22.0 | all of `src/` | its own (all `#ifdef` branches, except each `ASX_MUTANT_*` macro the sources test, which is `-U`ndefined: see below) | `check` job | `lint` (and `lint-static-analysis`) |
| clang-tidy | clang-tidy 18.1.8: `bugprone-*`, `cert-*`, `misc-*`, `performance-*`, `portability-*` and `clang-analyzer-*` (minus the exclusions in `tools/ci/run_static_analysis.sh`: `optin.performance.Padding`, `optin.core.EnumCastOutOfRange`) | all of `src/` | CORE (JSON, deterministic) and POSIX live (JSON, `DETERMINISTIC=0`): the profile macros select different code, and CORE compiles `src/platform/posix` to nothing | `check` job | `lint-static-analysis` |
| GCC `-fanalyzer` | GCC 15.2 (the `gcc:15.2.0` image) | every `src/` unit of `make build` | `CORE` (`PROFILE=CORE DETERMINISTIC=1`) and `POSIX_LIVE` (`PROFILE=POSIX DETERMINISTIC=0`), the build's own flags | `gcc-analyzer` job | `lint-gcc-analyzer` |

`tools/ci/run_gcc_analyzer.sh` takes its compile commands from
`make -n build`, so a define or profile added to the Makefile reaches the lane
without an edit; it drops `-Werror` so every diagnostic of a unit is
collected, and it fails (exit 2) when a unit does not compile, never passing
a unit it did not analyze. The analyzer's findings change between GCC
releases, so the compiler is pinned: a different GCC is a warning locally and
a failure under `STRICT_GATES=1` / CI.

### Planted mutants are not analyzed

An `ASX_MUTANT_<NAME>` macro guards a planted bug that only a gate's own build
defines (`tests/gates/test_*_mutants.sh`). cppcheck explores every `#ifdef`
configuration, so left alone it lints the planted code as if it shipped, and
`make lint` failed on `src/net/net.c` `quad[qlen]` (`arrayIndexOutOfBoundsCond`,
the `ASX_MUTANT_RESOLVE_HOST` plant). Both cppcheck invocations (`make lint`
and `tools/ci/run_static_analysis.sh`) therefore pass `-U<macro>` for every
`ASX_MUTANT_*` identifier found by grep in `src/` and `include/`, so a new
mutant is covered without an edit and needs no waiver. Clang-tidy and the GCC
analyzer compile the build's own flags and never define a mutant.
`tests/gates/test_static_analysis_fail_closed.sh`
(`cppcheck_planted_mutants_are_undefined`) pins that the script lane passes
the flag. The `ASX_ANALYZER_WAIVER("planted mutant ...")` comments in
`src/net/net.c` and `src/runtime/event_log.c` predate this and are now
unreachable; they can go with the next edit of those files.

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

### clang-tidy with `clang-analyzer-*`, CORE and POSIX live (2026-10-13)

Adding the path-sensitive checks and the POSIX configuration to the clang-tidy
lane found 25 distinct unwaived locations across the two configurations at
`d0f1b40` (24 in POSIX live, one more in CORE) and 3 more in `process_posix.c`
when it was re-run at `76ba65c`, after the journal's wrappers landed. Each was
fixed in the code unless it is a feature-test macro or a check that is noise by
construction, per the rule that a diagnostic the code can stop provoking is
must-fix.

| location | check | resolution |
|---|---|---|
| `src/net/websocket.c`, `asx_ws_engine_recv` | `core.NonNullParamChecker` | fixed: `ws->msg_len` is read once; `len` is a caller's `uint32_t *` that may alias the field, so the check and the copy could see different values |
| `src/net/net.c`, `parse_ipv6_host` | `deadcode.DeadStores` | fixed: the dead `p = end` after the dotted-quad tail is gone |
| `src/runtime/journal.c`, `asx_journal_ring_sink` (2 locations) | `core.NonNullParamChecker` | fixed: a zero-length write returns before `memcpy(..., NULL, 0)`, which is undefined for a NULL pointer; the argument check allows `bytes == NULL` when `len == 0` |
| `src/runtime/hooks.c`, `asx_codec_buffer_append_bytes` (through `reserve`) | `core.NonNullParamChecker` | fixed: `asx_codec_buffer_reserve` no longer counts a NULL `data` as room, so a hand-built buffer with `cap > 0` allocates |
| `src/platform/posix/net_posix.c`, `native_tcp_accept_impl`, `native_udp_recv_impl`, `native_refresh_local` (one line, `native_from_sockaddr`) | `core.UndefinedBinaryOperatorResult` | fixed: the address is zeroed before `accept`, `recvfrom` and `getsockname`, which write only the length they report |
| `src/platform/posix/process_posix.c`, `native_process_spawn_impl` (3 locations) | `core.NullDereference` | fixed: the backend returns `ASX_E_INVALID_ARGUMENT` for a NULL `cmd` or `out`; the callers in `src/process` check, but the journal's wrapper (`asx_native_process_spawn`) passes both through, so the backend is reachable with NULL |
| `src/platform/posix/net_posix.c` (12 casts) | `bugprone-casting-through-void` | fixed: `sockaddr_storage` is the type POSIX defines for casting to the specific `sockaddr` types, so the casts are direct |
| `src/runtime/io_driver.c`, interest 0 for a free slot | `optin.core.EnumCastOutOfRange` | check excluded in the lane: C permits the zero of a flag enum |
| `fs_posix.c` (2), `hooks.c`, `net_posix.c`, `process_posix.c`, `signal_posix.c` under `src/platform/posix` | `bugprone-reserved-identifier`, `cert-dcl51-cpp` | waived: `_GNU_SOURCE` and `_FILE_OFFSET_BITS` are feature-test macros the platform headers require a program to define; the waiver is a trailing comment on the `#define` |

### cppcheck and the rest of clang-tidy

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
| 2026-10-13 | `d0f1b40` | clang-tidy 18.1.8, the lane's checks plus `clang-analyzer-*`, whole `src/`, CORE and POSIX live | 25 distinct unwaived locations; 5 were edge-case undefined behaviour or indeterminate reads (a zero-length `memcpy` of NULL in two places, an unzeroed `accept` address, an aliasable length, a NULL-data buffer with room), the rest style checks; all fixed or, for the feature-test macros and the flag-enum check, waived or excluded (section 4). The POSIX adapters had never been analyzed by clang-tidy |
| 2026-10-13 | `76ba65c` | the same lane re-run with the fixes | 3 more locations, all in `process_posix.c` (a NULL `cmd`/`out` reaching the backend through the journal's wrapper), and the same `ss` read through two more callers; fixed. cppcheck reports 3 `moduloofone` at `hindsight.c:103,154,155` (a capacity of 1): not part of this change, for the file's owner |
| 2026-10-13 | `502fced` | `make lint STRICT_GATES=1`, cppcheck 2.22.0, whole `src/` | 1 finding: `src/net/net.c` `quad[qlen]` `arrayIndexOutOfBoundsCond`, in the `ASX_MUTANT_RESOLVE_HOST` configuration, which `make lint` explored (the waiver comment applied only to `lint-static-analysis`). Both cppcheck invocations now `-U` every `ASX_MUTANT_*` macro (section 2) |
| 2026-10-13 | `8bd095a` | `make lint-gcc-analyzer`, gcc 15.2, whole `src/`, CORE and POSIX live: the lane's first run since K18's `asx_kernel_of` returns NULL for a tag no runtime holds | 5 locations, each in both configurations, all `-Wanalyzer-null-dereference` of a kernel a handle lookup can answer NULL for (`asx_region_cancel`, `asx_region_drain` and its subtree scan, `parallel_run_impl`, `asx_inspect`, the finalizer task's poll). None waived: each takes the lookup's refusal (`ASX_E_NOT_FOUND`, a zeroed inspection report, `ASX_E_INVALID_STATE`) before it reads the kernel, and each has a unit test that hands it a handle or object whose tag no runtime holds |
| 2026-10-13 | `ad49717` | `make lint-gcc-analyzer`, gcc 15.2, whole `src/`, CORE and POSIX live (SapphireMill, mail 5315; identical on `5a645e4`) | 2 locations in both configurations, `dereference of NULL 'k'` at `scheduler.c` `sched_live_tasks` and `sched_fire_budget_deadlines`, both reached from `sched_run`, where `k = asx_kernel_of(region)` is NULL for a tag no runtime holds. Not reachable: `sched_run` took `asx_region_slot_lookup(region, &root)` next, which answers `ASX_E_NOT_FOUND` for such a handle, so no read of `k` followed; the analyzer could not relate the lookup's status to `k`. None waived: the lookup is now `k != NULL ? asx_region_slot_lookup(...) : ASX_E_NOT_FOUND` where the kernel is taken (the c38b051 contract; line-neutral). `test_handle_generation` hands `asx_scheduler_run` and `asx_scheduler_run_until_idle` a region handle whose tag no runtime holds (`ASX_E_NOT_FOUND`; it passes before and after, as the other four of c38b051 did). 0 must-fix after |
