/*
 * asx/runtime/lab.h — seeded lab runtime for deterministic testing
 *
 * Provides a high-level entrypoint that wires together virtual time,
 * seeded entropy, scenario configuration, and runtime init into a
 * single deterministic lab runtime. This is the bridge from low-level
 * testability to product-level reproducibility.
 *
 * Time is Rust's LabRuntime clock: frozen. It moves only when the lab
 * advances it (asx_lab_advance_time, asx_lab_advance_to_next_timer) or,
 * with config.auto_advance set, when asx_lab_run finds every task
 * waiting on a timer and jumps to the earliest one. Reading the clock
 * never moves it.
 *
 * Scheduling is Rust's LabRuntime dispatch with one worker, seeded from
 * config.seed (asx_scheduler_use_lab_dispatch): each step draws one value
 * from a seeded xorshift64 and polls one task chosen by lane, priority and
 * wake order, ties broken by the draw. The same tasks under another seed
 * can run in another order, and under the same seed in the same order, so a
 * schedule is reproduced by its seed. asx_scheduler_record_dispatches
 * reads the order back. config.scheduler selects the sweep scheduler (the
 * one a live host runs) instead.
 *
 * Usage:
 *   asx_lab lab;
 *   asx_lab_config cfg;
 *   asx_lab_config_init(&cfg);
 *   cfg.seed = 42;
 *
 *   asx_lab_init(&lab, &cfg);
 *   // ... run scenarios ...
 *   asx_lab_shutdown(&lab);
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_RUNTIME_LAB_H

#include <asx/asx_config.h>
#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>
#include <asx/core/budget.h>
#include <asx/core/outcome.h>
#include <asx/runtime/rt.h>
#include <asx/runtime/runtime.h>

#if !defined(ASX_PROFILE_BROWSER) || ASX_HAS_BROWSER_TRACE ||                                      \
    defined(ASX_INTERNAL_TRACE_FAMILY_ACCESS)
/* Guard defined only with the contents; see asx/runtime/trace.h. */
#define ASX_RUNTIME_LAB_H

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Lab configuration
 * ------------------------------------------------------------------- */

/* The scheduler asx_lab_run drives (asx_lab_config.scheduler). */
#define ASX_LAB_SCHEDULER_DISPATCH 0u /* Rust's lab dispatch, seeded from config.seed */
#define ASX_LAB_SCHEDULER_SWEEP 1u    /* the sweep scheduler of a live host; seed unused */

typedef struct {
    uint64_t seed;          /* seeds task dispatch and entropy (default 42, as Rust) */
    asx_time start_time_ns; /* Virtual time start (0 = default) */
    uint32_t max_polls;     /* Max polls per scenario step (0 = 1024 default) */
    /* Rust LabConfig::auto_advance (default off): asx_lab_run jumps the
     * clock to the earliest timer when every task waits on one. */
    uint8_t auto_advance;
    /* ASX_LAB_SCHEDULER_DISPATCH (default) polls one task per step, as
     * Rust's LabRuntime with one worker, drawing from a PRNG seeded with
     * `seed`. ASX_LAB_SCHEDULER_SWEEP polls every runnable task each round
     * in arena order: what asx_scheduler_run does for a live host, where
     * the seed only reaches entropy. Any other value is refused by
     * asx_lab_init. */
    uint8_t scheduler;
} asx_lab_config;

/* Initialize config with defaults. */
ASX_API asx_status asx_lab_config_init_checked(asx_lab_config *cfg,
                                               uint64_t application_fingerprint);
#define asx_lab_config_init(cfg) asx_lab_config_init_checked((cfg), ASX_LAB_CONFIG_FINGERPRINT)

/* -------------------------------------------------------------------
 * Lab runtime
 * ------------------------------------------------------------------- */

typedef struct {
    asx_runtime rt;
    asx_lab_config config;
    uint64_t entropy_state; /* seeded PRNG state */
    int initialized;
} asx_lab;

/* Initialize the lab runtime with deterministic config.
 * Sets up virtual time, seeded entropy, and runtime hooks, and, with
 * config.scheduler == ASX_LAB_SCHEDULER_DISPATCH (the default), turns on
 * lab dispatch seeded with config.seed (asx_scheduler_use_lab_dispatch), so
 * the step counter, certificate and dispatch recording start from the lab.
 * ASX_E_INVALID_ARGUMENT for an unknown config.scheduler. */
ASX_API ASX_MUST_USE asx_status asx_lab_init_checked(asx_lab *lab, const asx_lab_config *cfg,
                                                     uint64_t application_fingerprint);
#define asx_lab_init(lab, cfg) asx_lab_init_checked((lab), (cfg), ASX_LAB_CONFIG_FINGERPRINT)

/* The lab's own public object/config layout joins the runtime build identity. */
static inline uint64_t asx_header_lab_fingerprint(uint64_t fingerprint) {
    typedef struct {
        char pad;
        asx_lab value;
    } lab_alignment;
    typedef struct {
        char pad;
        asx_lab_config value;
    } config_alignment;
    static const uint64_t values[] = {
        sizeof(asx_lab),
        offsetof(lab_alignment, value),
        offsetof(asx_lab, rt),
        offsetof(asx_lab, config),
        offsetof(asx_lab, entropy_state),
        offsetof(asx_lab, initialized),
        sizeof(asx_lab_config),
        offsetof(config_alignment, value),
        offsetof(asx_lab_config, seed),
        offsetof(asx_lab_config, start_time_ns),
        offsetof(asx_lab_config, max_polls),
        offsetof(asx_lab_config, auto_advance),
        offsetof(asx_lab_config, scheduler),
    };
    size_t i;
    for (i = 0u; i < sizeof(values) / sizeof(values[0]); i++) {
        /* ASX_CHECKPOINT_WAIVER("bounded lab bootstrap layout facts") */
        fingerprint = (fingerprint ^ values[i]) * UINT64_C(1099511628211);
    }
    return fingerprint;
}
#define ASX_LAB_CONFIG_FINGERPRINT (asx_header_lab_fingerprint(ASX_CONFIG_FINGERPRINT))

/* Shut down the lab runtime and restore hooks. */
ASX_API void asx_lab_shutdown(asx_lab *lab);

/* Reset the lab runtime to initial state (preserves config) and
 * re-bootstrap the deterministic runtime hook table. */
ASX_API asx_status asx_lab_reset_checked(asx_lab *lab, uint64_t application_fingerprint);
#define asx_lab_reset(lab) asx_lab_reset_checked((lab), ASX_LAB_CONFIG_FINGERPRINT)

/* -------------------------------------------------------------------
 * Time control
 * ------------------------------------------------------------------- */

/* Advance virtual time by `ns` nanoseconds (Rust advance_time),
 * saturating. It only moves the clock: timers that come due fire inside
 * a later scheduler step, or by asx_lab_advance_to_next_timer. */
ASX_API void asx_lab_advance_time(asx_lab *lab, asx_time ns);

/* The earliest armed task timer's deadline (Rust next_timer_deadline):
 * ASX_OK and *out, or ASX_E_NOT_FOUND when no timer is armed. */
ASX_API ASX_MUST_USE asx_status asx_lab_next_timer_deadline(const asx_lab *lab, asx_time *out);

/* Advance virtual time to the earliest armed task timer (when it is
 * later than now) and fire the timers then due, waking their tasks (Rust
 * advance_to_next_timer). ASX_OK and the number of tasks woken in
 * *out_woken (if non-NULL), or ASX_E_NOT_FOUND when no timer is armed. */
ASX_API ASX_MUST_USE asx_status asx_lab_advance_to_next_timer(asx_lab *lab, uint32_t *out_woken);

/* Get current virtual time. Reading it never moves it. */
ASX_API asx_time asx_lab_now(const asx_lab *lab);

/* Run the lab's tasks: with config.auto_advance, as asx_scheduler_run (the
 * clock jumps to the earliest timer whenever every task waits on one);
 * without it, as asx_scheduler_run_until_idle (due timers fire, the clock
 * stays). Returns that function's status.
 *
 * Under lab dispatch (the default scheduler) every live task of the lab
 * runs, as Rust's run_until_idle does, and `region` only has to be valid;
 * the budget counts steps. Under the sweep scheduler the region's subtree
 * runs and the budget counts polls. */
ASX_API ASX_MUST_USE asx_status asx_lab_run(asx_lab *lab, asx_region_id region, asx_budget *budget);

/* -------------------------------------------------------------------
 * Scenario execution
 * ------------------------------------------------------------------- */

/* A scenario step: poll function + user data. */
typedef asx_status (*asx_lab_step_fn)(asx_lab *lab, void *user_data);

/* Scenario definition: a sequence of steps. */
typedef struct {
    const char *name;
    asx_lab_step_fn steps[ASX_LAB_MAX_STEPS];
    void *step_data[ASX_LAB_MAX_STEPS];
    uint32_t step_count;
} asx_lab_scenario;

/* Initialize a scenario. */
ASX_API void asx_lab_scenario_init(asx_lab_scenario *sc, const char *name);

/* Add a step to a scenario. */
ASX_API ASX_MUST_USE asx_status asx_lab_scenario_add_step(asx_lab_scenario *sc,
                                                          asx_lab_step_fn step_fn, void *user_data);

/* -------------------------------------------------------------------
 * Scenario result
 * ------------------------------------------------------------------- */

typedef struct {
    const char *scenario_name;
    uint32_t steps_completed;
    uint32_t steps_total;
    asx_status last_status; /* status of last step run */
    asx_time elapsed_ns;    /* virtual time elapsed */
    uint64_t polls_total;   /* total polls across all steps */
} asx_lab_result;

/* Run a scenario in the lab runtime.
 * Executes each step in order. Stops on first error.
 * Fails closed with ASX_E_INVALID_STATE if the scenario descriptor is
 * malformed (step_count overflow or NULL step entry inside the active range).
 * Returns ASX_OK if all steps succeed. */
ASX_API ASX_MUST_USE asx_status asx_lab_run_scenario(asx_lab *lab, const asx_lab_scenario *scenario,
                                                     asx_lab_result *out_result);

/* -------------------------------------------------------------------
 * Seeded entropy
 * ------------------------------------------------------------------- */

/* Get a deterministic random u64 from the lab's PRNG. */
ASX_API uint64_t asx_lab_random_u64(asx_lab *lab);

/* -------------------------------------------------------------------
 * Convenience: open region in lab context
 * ------------------------------------------------------------------- */

/* Open a new region within the lab runtime context. */
ASX_API ASX_MUST_USE asx_status asx_lab_open_region(asx_lab *lab, asx_region_id *out_id);

/* -------------------------------------------------------------------
 * Forced schedules (Rust ForcedSchedule, lab/runtime.rs, bd-9kll.9.3)
 *
 * A forced schedule is the exact dispatch projection of one lab run: for
 * every step that polled a task, the task, its worker, its lane, the lab
 * step and the virtual time, then the terminal step, time and schedule
 * certificate. Rust writes it (finish_forced_schedule_recording) and
 * replays it (run_forced_schedule); twin_run stores Rust's in each
 * fixture's `schedule.forced_schedule`. asx reads and writes the same
 * canonical bytes and replays one on its own lab dispatcher.
 *
 * Canonical bytes (little-endian, fixed width):
 *   header   40: "ASUPFSC\0", u32 artifact version (1), u32 schema (1),
 *                u64 seed, u64 config hash, u64 dispatch count
 *   dispatch 29: u64 task, u32 worker, u8 lane, u64 at_step, u64 at_nanos
 *   terminal 26: u64 steps, u64 nanos, u64 schedule hash,
 *                u8 quiescent, u8 truncated
 *   checksum 32: SHA-256 of "asupersync.lab.forced-schedule.artifact.v1\0"
 *                followed by every byte before the checksum
 * A task is Rust's CompactTaskId: (arena index << 32) | generation.
 * ------------------------------------------------------------------- */

#define ASX_FORCED_SCHEDULE_ARTIFACT_VERSION 1u
#define ASX_FORCED_SCHEDULE_SCHEMA_VERSION 1u
#define ASX_FORCED_SCHEDULE_HEADER_BYTES 40u
#define ASX_FORCED_SCHEDULE_DISPATCH_BYTES 29u
#define ASX_FORCED_SCHEDULE_TERMINAL_BYTES 26u
#define ASX_FORCED_SCHEDULE_CHECKSUM_BYTES 32u

/* Lane tags, as Rust's DispatchLane and asx_dispatch_record.lane. The C
 * lab dispatcher feeds only the cancel and ready lanes (Rust's lab never
 * feeds the timed lane at the pinned rev; Stolen needs more than one
 * worker). */
#define ASX_FORCED_LANE_CANCEL 0u
#define ASX_FORCED_LANE_TIMED 1u
#define ASX_FORCED_LANE_READY 2u
#define ASX_FORCED_LANE_STOLEN 3u

typedef struct {
    uint64_t task; /* CompactTaskId: (arena index << 32) | generation */
    uint32_t worker;
    uint8_t lane; /* ASX_FORCED_LANE_* */
    uint64_t at_step;
    uint64_t at_nanos;
} asx_forced_dispatch;

typedef struct {
    uint32_t version; /* schema, ASX_FORCED_SCHEDULE_SCHEMA_VERSION */
    uint64_t seed;
    uint64_t config_hash; /* Rust's LabConfig hash; carried, not checked by asx */
    const asx_forced_dispatch *dispatches;
    uint32_t dispatch_count;
    uint64_t terminal_steps;
    uint64_t terminal_nanos;
    uint64_t terminal_schedule_hash;
    uint8_t terminal_quiescent;
    uint8_t truncated;
} asx_forced_schedule;

/* Why a schedule was refused to decode or encode, or where a replay
 * diverged. `index` is the dispatch index, `step` the lab step (replay),
 * `expected` and `actual` the values that differed (steps, times, hashes,
 * counts, tags) when there are two. */
typedef enum {
    ASX_FORCED_OK = 0,
    /* artifact */
    ASX_FORCED_E_TRUNCATED,        /* fewer bytes than the shape needs */
    ASX_FORCED_E_MAGIC,            /* not a forced-schedule artifact */
    ASX_FORCED_E_ARTIFACT_VERSION, /* codec version unsupported */
    ASX_FORCED_E_SCHEMA_VERSION,   /* projection schema unsupported */
    ASX_FORCED_E_BYTE_LIMIT,       /* longer than the caller's limit */
    ASX_FORCED_E_DISPATCH_LIMIT,   /* more dispatches than limit or storage */
    ASX_FORCED_E_TRAILING_BYTES,   /* bytes beyond the checksum */
    ASX_FORCED_E_CHECKSUM,         /* SHA-256 binding does not match */
    ASX_FORCED_E_LANE_TAG,         /* unknown lane tag */
    ASX_FORCED_E_BOOL_TAG,         /* a flag byte is not 0 or 1 */
    ASX_FORCED_E_STEP_ORDER,       /* steps not strictly increasing from 1 */
    ASX_FORCED_E_TIME_ORDER,       /* virtual time moved backwards */
    ASX_FORCED_E_PARTIAL_SOURCE,   /* truncated or not quiescent, or ends past terminal */
    /* replay */
    ASX_FORCED_E_SEED_MISMATCH,       /* run seed differs from the schedule's */
    ASX_FORCED_E_NOT_PRISTINE,        /* the lab already took steps */
    ASX_FORCED_E_WORK_LIMIT,          /* step limit reached before the end */
    ASX_FORCED_E_EARLY_RUNNABLE,      /* work was runnable before the next recorded step */
    ASX_FORCED_E_STEP_MISMATCH,       /* the run passed the next recorded step */
    ASX_FORCED_E_TIME_MISMATCH,       /* the clock differs from the recorded time */
    ASX_FORCED_E_TASK_UNAVAILABLE,    /* the recorded task is not runnable at that step */
    ASX_FORCED_E_LANE_MISMATCH,       /* runnable, but in another lane */
    ASX_FORCED_E_LANE_UNSUPPORTED,    /* timed or stolen lane: not modelled by asx's lab */
    ASX_FORCED_E_WORKER_MISMATCH,     /* asx's lab models one worker (0) */
    ASX_FORCED_E_SCHEDULE_EXHAUSTED,  /* work left after the last recorded dispatch */
    ASX_FORCED_E_TERMINAL_MISMATCH,   /* run ended at another step or time */
    ASX_FORCED_E_CERTIFICATE_MISMATCH /* run's schedule hash differs */
} asx_forced_error_kind;

typedef struct {
    asx_forced_error_kind kind;
    uint32_t index;
    uint64_t step;
    uint64_t expected;
    uint64_t actual;
} asx_forced_error;

typedef struct {
    size_t max_encoded_bytes;
    uint32_t max_dispatches;
} asx_forced_decode_limits;

/* Encoded size in bytes of a schedule with `dispatch_count` dispatches. */
ASX_API size_t asx_forced_schedule_encoded_size(uint32_t dispatch_count);

/* Rust ScheduleCertificate (runtime/scheduler/priority.rs): fold one
 * dispatch into the running hash (start from 0). The hash is DetHasher::
 * for_lab over (previous hash, arena index, generation, lane, step). */
ASX_API uint64_t asx_forced_schedule_certificate_step(uint64_t previous, uint64_t compact_task,
                                                      uint8_t lane, uint64_t step);

/* The certificate over `count` dispatches in order. */
ASX_API uint64_t asx_forced_schedule_certificate(const asx_forced_dispatch *dispatches,
                                                 uint32_t count);

/* Check a schedule's shape as Rust's validate_complete_forced_schedule_shape
 * does: schema version, complete (not truncated, quiescent), strictly
 * increasing steps and non-decreasing time, ending at or before the
 * terminal step and time. ASX_OK, or ASX_E_INVALID_ARGUMENT with *err. */
ASX_API ASX_MUST_USE asx_status asx_forced_schedule_validate(const asx_forced_schedule *schedule,
                                                             asx_forced_error *err);

/* Encode a complete schedule into `out` (capacity `cap`); *out_len is the
 * size written, or needed when ASX_E_BUFFER_TOO_SMALL. */
ASX_API ASX_MUST_USE asx_status asx_forced_schedule_encode(const asx_forced_schedule *schedule,
                                                           uint8_t *out, size_t cap,
                                                           size_t *out_len, asx_forced_error *err);

/* Decode and strictly validate canonical bytes. The dispatches go into
 * `storage` (capacity `storage_cap`), which `out->dispatches` then points
 * to. Everything Rust's try_from_canonical_bytes refuses is refused, in
 * the same order: length and limits before any dispatch is read, then
 * checksum, lane and flag tags, ordering and completeness. ASX_OK, or
 * ASX_E_INVALID_ARGUMENT with *err describing the first problem. */
ASX_API ASX_MUST_USE asx_status asx_forced_schedule_decode(const uint8_t *bytes, size_t len,
                                                           const asx_forced_decode_limits *limits,
                                                           asx_forced_dispatch *storage,
                                                           uint32_t storage_cap,
                                                           asx_forced_schedule *out,
                                                           asx_forced_error *err);

/* Rust's ScheduleCertificate of the lab run so far (0 before the first
 * dispatch). Folded at every pick of the lab dispatcher
 * (asx_scheduler_use_lab_dispatch), whether or not anything records; the
 * same value Rust reports as `certificate_hash` for the same schedule. */
ASX_API uint64_t asx_scheduler_schedule_certificate(void);

/* How many dispatches the certificate folds so far: Rust's `decisions`.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API uint64_t asx_scheduler_schedule_decisions(void);

/* Rust's start_forced_schedule_recording / finish_forced_schedule_recording
 * (lab/runtime.rs:2618-2683). Start (after asx_scheduler_use_lab_dispatch,
 * before the first step) captures every later dispatch, with the task's
 * Rust arena id, into storage[0 .. capacity); finish binds the capture to
 * the run's seed, terminal step, clock and certificate in *out, whose
 * dispatches point at `storage`. `config_hash` is the caller's (Rust's is
 * a LabConfig hash) and `quiescent` whether the run ended quiescent; a
 * schedule that did not has no canonical bytes.
 * ASX_E_INVALID_STATE when the lab dispatcher is off, already recording,
 * or already stepped (start) or not recording (finish);
 * ASX_E_RESOURCE_EXHAUSTED at finish when the run had more dispatches than
 * `capacity` (Rust refuses a partial receipt too). */
ASX_API ASX_MUST_USE asx_status
asx_lab_start_forced_schedule_recording(asx_forced_dispatch *storage, uint32_t capacity);
ASX_API ASX_MUST_USE asx_status asx_lab_finish_forced_schedule_recording(uint64_t config_hash,
                                                                         int quiescent,
                                                                         asx_forced_schedule *out);

typedef struct {
    uint32_t dispatches;     /* recorded dispatches consumed */
    uint64_t steps;          /* lab steps taken */
    uint64_t terminal_nanos; /* virtual time at the end */
    uint64_t schedule_hash;  /* the run's certificate */
} asx_forced_report;

typedef struct {
    /* Step limit for the run (0: the schedule's terminal step). */
    uint64_t max_steps;
    /* Maps a recorded task id to the task that plays it in this run: ASX_OK
     * with the task, anything else to decline. A declined id, and every id
     * when this is NULL, resolves through the lab's arena shadow: the live
     * task whose Rust arena id (the lab dispatcher keeps Rust's arena
     * index and generation for every task) is the recorded one, or a
     * retired task a cancel waker scheduled. So a resolver names the tasks
     * the caller knows (a scenario's named tasks) and the tasks the
     * runtime creates itself (supervisor generations, finalizers) need no
     * entry. */
    asx_status (*resolve)(void *ctx, uint64_t compact_task, asx_task_id *out);
    void *resolve_ctx;
} asx_forced_run_config;

/* Run the region's subtree on the lab dispatcher (asx_scheduler_use_lab_dispatch
 * already called with the schedule's seed, nothing stepped yet), taking
 * each step's task from the schedule instead of from the seeded picker:
 * the recorded task is removed from its recorded lane before it is polled,
 * and anything else (the task not runnable, in another lane, runnable work
 * before or after the recorded dispatches, the clock or step off) is a
 * divergence reported with its step and dispatch index, with the
 * mismatching task left unpolled. At the end the step, time and
 * certificate must equal the schedule's terminal receipt.
 *
 * ASX_OK on an exact replay; ASX_E_REPLAY_MISMATCH on a divergence;
 * ASX_E_INVALID_ARGUMENT for a malformed schedule; ASX_E_INVALID_STATE
 * when the lab dispatcher is off or has already stepped. *err (if
 * non-NULL) says which; *report (if non-NULL) is set on success. */
ASX_API ASX_MUST_USE asx_status asx_lab_run_forced_schedule(const asx_forced_schedule *schedule,
                                                            asx_region_id region,
                                                            const asx_forced_run_config *config,
                                                            asx_forced_report *report,
                                                            asx_forced_error *err);

/* The same replay for a runner that drives the scheduler its own way (to
 * idle between script ops, then with timer auto-advance, as the
 * conformance interpreter does): begin installs the schedule on a fresh
 * lab dispatcher (same checks and statuses as above), every lab step until
 * end takes its task from it, and end takes it off and judges the run:
 * ASX_OK with *report, or ASX_E_REPLAY_MISMATCH with the first divergence
 * (a divergence ends the scheduler run early with that status). The run
 * must end quiescent in Rust's sense: nothing scheduled, every task
 * terminal, no region closing, no obligation reserved (an open region
 * with finished work counts). */
ASX_API ASX_MUST_USE asx_status asx_lab_forced_schedule_begin(const asx_forced_schedule *schedule,
                                                              const asx_forced_run_config *config,
                                                              asx_forced_error *err);
ASX_API ASX_MUST_USE asx_status asx_lab_forced_schedule_end(asx_forced_report *report,
                                                            asx_forced_error *err);

/* A stable lowercase name for an error kind ("task_unavailable", ...). */
ASX_API const char *asx_forced_error_kind_name(asx_forced_error_kind kind);

#ifdef __cplusplus
}
#endif

#endif /* trace family public contract */

#endif /* ASX_RUNTIME_LAB_H */
