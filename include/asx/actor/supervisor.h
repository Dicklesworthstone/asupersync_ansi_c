/*
 * asx/actor/supervisor.h — managed supervision (Rust src/supervision.rs:
 * the managed controller, CompiledSupervisor::bind_managed and
 * ManagedSupervisor::spawn, :1795-3506)
 *
 * A supervisor is a controller task. It opens a supervisor region below
 * the region it was spawned in, and runs every child generation as a task
 * in a region of its own below that one. The controller (Rust's
 * Controller::execute and ::finish, :3064-3387):
 *
 *   1. starts the children in compiled order (dependencies first,
 *      otherwise in spec order). A generation counts as started once its
 *      start function ran inside its task, at that task's first poll;
 *   2. waits for a generation to end. Ends that happened together are
 *      taken by (end time, task) order. A child is eligible for a restart
 *      by its restart mode: PERMANENT after any end, TRANSIENT after an
 *      error or a panic (not after success or cancellation), TEMPORARY
 *      never;
 *   3. for an eligible end, consults the restart intensity (at most
 *      max_restarts replacement batches in any restart window): allowed,
 *      it cancels the children the strategy names (ONE_FOR_ONE: the failed
 *      one; ONE_FOR_ALL: all; REST_FOR_ONE: the failed one and the later
 *      ones), each through its region with the reason "managed supervisor
 *      generation drain", drains them in reverse order (joins the task,
 *      closes the region), sleeps the backoff, and starts a new generation
 *      of each that is not TEMPORARY and either was shut down before it
 *      ended or is eligible itself. Refused, the escalation policy decides:
 *      STOP drains the failed child and leaves it stopped;
 *      RESET_COUNTER forgets the restart history and retries; ESCALATE
 *      records a RestartLimit error, cancels the controller's own region
 *      (a FailFast "managed supervisor restart intensity exhausted"), and
 *      stops;
 *   4. stops when no child is left running, on an error, or when it is
 *      cancelled (the drained generations are never replaced), then drains
 *      every generation in reverse order and closes the supervisor region
 *      before it publishes its report.
 *
 * The controller traces "managed_supervisor_v1 action=<action>
 * supervisor="<name>" child="<name>" generation=<n> region=<id>
 * task=<id> outcome=<ok|err|cancelled|panicked|pending>" as Rust does, with
 * the actions started, terminal, drained, parent_escalated,
 * restart_dependency_unavailable and restart_storm; <id> is the handle in
 * hex (0x...).
 *
 * The C model of the start function and the body: Rust's factory returns
 * a future whose Outcome is the generation's; here the start function
 * returns the poll function and data of the generation's body, which the
 * generation's task polls. Its ASX_OK is an ok outcome and any other
 * status an error carrying that status; a panic in the body
 * (asx_task_panic) is a panicked outcome, caught so that the generation's
 * task itself completes normally, as Rust catches it. A start function
 * that fails is the factory panicking.
 *
 * Restart storm detection (Rust SupervisionConfig::with_storm_threshold,
 * RestartIntensityWindow, :5555-5640; the controller's check, :3294-3305):
 * with a storm threshold set, each counted replacement batch asks whether
 * the restarts in the window per second of window exceed the threshold.
 * The first time they do, the report's restart_storm_detected is set and
 * the controller traces "action=restart_storm" once, for the failed
 * child's latest generation; restart decisions do not change. Rust also
 * feeds an e-process (RestartStormMonitor, :5645-5840) and reports a storm
 * when it alerts, which needs f64 ln and exp. It is not ported, and no
 * report differs: the monitor's likelihood ratio exceeds 1 exactly when
 * the intensity exceeds the threshold (its expected rate is the threshold
 * divided by its tolerance, and the likelihood ratio is
 * max(1, intensity / expected) / tolerance), so its e-value can reach the
 * 1/alpha alert only at a batch whose intensity check has fired, at that
 * batch or before.
 *
 * Name registration (Rust bind_managed_with_registry, :2117;
 * supervision/managed_names.rs): a child spec may carry a name and a
 * collision policy, and the config a registry. Each generation takes the
 * name inside its own task, before its start function runs, in this order:
 * a cancel check; register under the policy at the current time (Fail
 * refuses a taken name; Replace displaces the holder, which is cancelled
 * with the User reason "managed supervisor name replaced"; Wait enqueues a
 * waiter whose deadline is the task's budget deadline); then a loop that
 * checks the cancel, the lease, the deadline and its grant, and sleeps 1 ms
 * between looks. A refusal (taken, deadline passed, the lease replaced) ends
 * the generation Cancelled with the User reason "managed child registration
 * failed" without running its start function, and the supervisor with
 * ASX_SUPERVISOR_ERR_REGISTRATION. A generation gives its name up when it
 * has been drained and its region closed, before a successor starts; the
 * name then goes to the first waiter. The registry is not touched by
 * anything else the supervisor does. Fail and Replace have Rust fixtures
 * (DSL 3.8); Wait is covered by unit tests only, because the 1 ms grant
 * checks are Sleeps made inside Rust's library whose Timer events a capture
 * cannot attribute to a task. A controller boots its children one by one
 * and handles a sibling's end only afterwards, so a Wait behind a sibling
 * waits until the supervisor is cancelled or the name's deadline passes,
 * as in Rust; it is meant for a name another supervisor or task holds.
 *
 * Not in the C model, with the reason for each:
 *   - shared restart domains: Rust attaches one only from its dynamic
 *     supervisor (cx/dynamic_supervisor.rs; set_shared_restarts is
 *     pub(crate)), a separate module this port does not have, so the
 *     SharedRestartLimit error is unreachable through the managed
 *     supervisor;
 *   - the part of a child's shutdown budget that reaches finalizers: Rust's
 *     ceiling (RuntimeState::cancel_request_with_shutdown_budget,
 *     state.rs:7556) is also the ShutdownBudget handle of every finalizer
 *     task the region spawns (:8615) and re-publishes a finalizer task it
 *     tightens (:7871). The tasks of a drained generation and of its
 *     sub-regions take the ceiling (asx_child_spec.shutdown_budget); a
 *     region finalizer's budget stays the runtime config's;
 *   - region cleanup outcomes (ManagedSupervisorError::Cleanup and
 *     SupervisorCleanup): Rust's region close returns the finalizers'
 *     outcome (close_with_outcome), which C regions do not record until
 *     bd-9kll.3.4 does, so neither error occurs;
 *   - the legacy CompiledSupervisor::spawn / ChildStart boot path (:1639),
 *     EXCLUDED: it threads Rust's RuntimeState through a synchronous start
 *     closure, boots the children in compiled order and restarts nothing
 *     (its own documentation says a crash is not restarted), so
 *     asx_supervisor_spawn, which boots in the same order and also
 *     supervises, subsumes it; Rust itself calls the managed entry point
 *     additive (:1784). Its pure restart planners (restart_plan_for,
 *     compile_restart_ops) are not used by the managed controller.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_ACTOR_SUPERVISOR_H
#define ASX_ACTOR_SUPERVISOR_H

#include <asx/actor/actor.h>
#include <asx/asx_config.h>
#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>
#include <asx/core/budget.h>
#include <asx/core/cancel.h>
#include <asx/runtime/runtime.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Limits
 * ------------------------------------------------------------------- */

#if (ASX_MAX_SUPERVISORS) < 1
#error "ASX_MAX_SUPERVISORS must be at least 1"
#endif

#if (ASX_SUPERVISOR_MAX_CHILDREN) < 2
#error "ASX_SUPERVISOR_MAX_CHILDREN must be at least 2"
#endif

/* Supervisor and child names, terminator included. */
/* The most restart batches a window can hold (max_restarts' bound). */
/* -------------------------------------------------------------------
 * Policies
 * ------------------------------------------------------------------- */

/* Which children a restart replaces (Rust RestartPolicy). */
typedef enum {
    ASX_SUPERVISOR_ONE_FOR_ONE = 0, /* the failed child */
    ASX_SUPERVISOR_ONE_FOR_ALL = 1, /* every running child */
    ASX_SUPERVISOR_REST_FOR_ONE = 2 /* the failed child and the later ones */
} asx_supervisor_strategy;

/* When a child is replaced (Rust ManagedRestartMode). */
typedef enum {
    ASX_CHILD_PERMANENT = 0, /* after any end */
    ASX_CHILD_TRANSIENT = 1, /* after an error or a panic */
    ASX_CHILD_TEMPORARY = 2  /* never */
} asx_child_restart;

/* What a refused restart does (Rust EscalationPolicy). */
typedef enum {
    ASX_ESCALATION_STOP = 0,
    ASX_ESCALATION_ESCALATE = 1,
    ASX_ESCALATION_RESET_COUNTER = 2
} asx_supervisor_escalation;

/* The delay before a replacement batch (Rust BackoffStrategy), for the
 * n-th batch in the window (n from 0): NONE no delay; FIXED initial_ns;
 * EXPONENTIAL min(initial_ns * multiplier^min(n, 30), max_ns). Rust
 * computes the exponential in f64 seconds and rounds to the nanosecond;
 * with an integer multiplier that is this exact product below 2^51 ns. */
typedef enum {
    ASX_RESTART_BACKOFF_NONE = 0,
    ASX_RESTART_BACKOFF_FIXED = 1,
    ASX_RESTART_BACKOFF_EXPONENTIAL = 2
} asx_restart_backoff_kind;

typedef struct {
    asx_restart_backoff_kind kind;
    uint64_t initial_ns;
    uint64_t max_ns;
    uint32_t multiplier;
} asx_restart_backoff;

/* -------------------------------------------------------------------
 * Children
 * ------------------------------------------------------------------- */

/* One generation of a child (Rust ManagedGeneration). */
typedef struct {
    uint32_t child;       /* index of its spec */
    uint64_t number;      /* 1 for the first generation */
    asx_region_id region; /* its own region */
    asx_task_id task;     /* its task */
} asx_supervisor_generation;

/* Called inside a generation's task at its first poll (Rust
 * ManagedChildFactory::start): set *out_poll and *out_data to the body the
 * task then polls with the task's own id. A status other than ASX_OK is
 * the factory panicking.
 *
 * The body's status is the generation's outcome, as the factory future's
 * Outcome<(), E> is in Rust: ASX_OK is Ok; ASX_E_PENDING is not finished;
 * ASX_E_CANCELLED is Outcome::Cancelled, which ManagedRestartMode::eligible
 * does not replace a Transient child for, with the reason of the cancel
 * that reached the task (cx.cancel_reason()) or, when none did, a User
 * reason attributed to the generation's region and task; any other status
 * is Outcome::Err(status), and a panic is Outcome::Panicked. */
typedef asx_status (*asx_child_start_fn)(void *user_data, const asx_supervisor_generation *gen,
                                         asx_task_poll_fn *out_poll, void **out_data);

typedef struct {
    const char *name; /* unique among the children; copied */
    asx_child_start_fn start;
    void *user_data;
    asx_child_restart restart;
    uint32_t depends_on[ASX_SUPERVISOR_MAX_DEPS]; /* spec indices */
    uint32_t dep_count;
    uint8_t required;          /* a failed start stops the supervisor (default 1) */
    uint8_t start_immediately; /* started at boot (default 1) */
    /* Name registration (Rust ChildSpec::registration,
     * NameRegistrationPolicy::Register): each generation takes this name in
     * the supervisor's registry (asx_supervisor_config.registry) with this
     * collision policy, inside its own task and before its start function
     * runs, and gives it up when it is drained. A generation that cannot
     * (the name is taken under FAIL, its wait expired) never runs its start
     * function and ends the supervisor with ASX_SUPERVISOR_ERR_REGISTRATION.
     * NULL for none; copied; at most ASX_NAME_MAX_LEN characters. */
    const char *registration_name;
    asx_name_collision registration_collision;
    /* Shutdown budget (Rust ChildSpec::shutdown_budget, supervision.rs:626):
     * the ceiling each of this child's generations is drained with. The
     * drain cancels the generation's region with it (CancelWithBudget,
     * :2235): the region and its descendants keep the tightest ceiling that
     * reaches them, and every task that cancel cancels takes the cleanup
     * budget of its reason met with it. Default asx_budget_infinite(),
     * which changes nothing; a deadline is absolute time. */
    asx_budget shutdown_budget;
} asx_child_spec;

/* A spec with Rust ChildSpec::new's defaults: no dependencies, required,
 * started at boot, not registered, an infinite shutdown budget. */
ASX_API void asx_child_spec_init(asx_child_spec *spec, const char *name, asx_child_restart restart,
                                 asx_child_start_fn start, void *user_data);

/* Make `spec` depend on the child at `dep_index`: it starts after that
 * child, and a dependency that is not running when it would start makes a
 * required child's supervisor fail (DependencyUnavailable) and keeps an
 * optional child stopped. ASX_E_RESOURCE_EXHAUSTED past
 * ASX_SUPERVISOR_MAX_DEPS. */
ASX_API ASX_MUST_USE asx_status asx_child_spec_depends_on(asx_child_spec *spec, uint32_t dep_index);

/* -------------------------------------------------------------------
 * Configuration
 * ------------------------------------------------------------------- */

typedef struct {
    const char *name; /* copied */
    asx_supervisor_strategy strategy;
    uint32_t max_restarts; /* at most ASX_SUPERVISOR_MAX_RESTARTS */
    uint64_t window_ns;    /* the restart window */
    asx_restart_backoff backoff;
    asx_supervisor_escalation escalation;
    const asx_budget *budget; /* the supervisor region's budget, or NULL */
    /* Restart storm detection (Rust SupervisionConfig::storm_threshold):
     * restarts per second above which the report says so; 0 for none (Rust's
     * None). A value that is negative, NaN or infinite is refused, as Rust's
     * InvalidStormThreshold is. */
    double storm_threshold;
    /* The registry the children's registrations use (Rust
     * bind_managed_with_registry): owned by the caller, shared with whoever
     * else registers names in it, and alive as long as the supervisor.
     * NULL when no child registers; a child that registers without one is
     * refused, as Rust's UnsupportedRegistration is. */
    asx_name_registry *registry;
} asx_supervisor_config;

/* A config with Rust SupervisionConfig::new's defaults: ONE_FOR_ONE,
 * exponential backoff from 100 ms doubling up to 10 s, STOP, no budget, no
 * storm detection. */
ASX_API void asx_supervisor_config_init(asx_supervisor_config *cfg, const char *name,
                                        uint32_t max_restarts, uint64_t window_ns);

/* -------------------------------------------------------------------
 * The report (Rust ManagedSupervisorReport)
 * ------------------------------------------------------------------- */

/* Why a supervisor stopped with an error (Rust ManagedSupervisorError). */
typedef enum {
    ASX_SUPERVISOR_ERR_NONE = 0,
    ASX_SUPERVISOR_ERR_REGION = 1,                 /* a region open failed */
    ASX_SUPERVISOR_ERR_SPAWN = 2,                  /* a generation's spawn failed */
    ASX_SUPERVISOR_ERR_CHILD_NOT_STARTED = 3,      /* it ended before it started */
    ASX_SUPERVISOR_ERR_DEPENDENCY_UNAVAILABLE = 4, /* a dependency was not running */
    ASX_SUPERVISOR_ERR_RESTART_LIMIT = 5,          /* restart refused under ESCALATE */
    ASX_SUPERVISOR_ERR_GENERATION_EXHAUSTED = 6,   /* no generation number left */
    ASX_SUPERVISOR_ERR_ESCALATION = 7,             /* the escalation could not be queued */
    /* A named generation could not take its name (Rust Registration):
     * error_child is the child, error_status the registry's refusal,
     * ASX_E_NAME_CONFLICT (NameTaken), ASX_E_NOT_FOUND (the lease no longer
     * owned the name) or ASX_E_TIMED_OUT (the wait's deadline passed). Its
     * start function did not run. */
    ASX_SUPERVISOR_ERR_REGISTRATION = 8
} asx_supervisor_error;

/* The latest generation of a child to end (Rust ManagedChildCompletion). */
typedef struct {
    uint32_t child; /* spec index */
    asx_supervisor_generation generation;
    /* What the generation produced: OK, ERR (status), CANCELLED
     * (cancel_reason) or PANICKED (panic_message). */
    asx_outcome_severity outcome;
    asx_status status;
    asx_cancel_reason cancel_reason;
    const char *panic_message;
    /* How its task completed (OK, CANCELLED or PANICKED). */
    asx_outcome_severity task_outcome;
    /* The supervisor asked it to stop before it ended. */
    int shutdown_requested_before_completion;
    asx_time completed_at;
} asx_supervisor_completion;

typedef struct {
    /* OK; ERR (error, error_child, error_status); CANCELLED
     * (cancel_reason); PANICKED (panic_message). */
    asx_outcome_severity outcome;
    asx_supervisor_error error;
    uint32_t error_child;    /* spec index, UINT32_MAX for none */
    asx_status error_status; /* the failed operation's status */
    asx_cancel_reason cancel_reason;
    const char *panic_message;
    asx_region_id region; /* the supervisor region, ASX_INVALID_ID if none */
    uint64_t started;     /* generations whose start function ran */
    uint64_t joined;      /* generations joined */
    uint64_t restart_batches;
    uint8_t escalations;
    /* A storm threshold was set and a counted replacement batch's restart
     * intensity exceeded it (Rust restart_storm_detected). */
    uint8_t restart_storm_detected;
    /* Each child's latest ended generation, in compiled order. */
    uint32_t completion_count;
    asx_supervisor_completion completions[ASX_SUPERVISOR_MAX_CHILDREN];
} asx_supervisor_report;

/* -------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t slot;
    uint32_t generation;
} asx_supervisor_handle;

/* Compile the children (unique names, dependencies known and acyclic, a
 * child started at boot depending only on children started at boot) and
 * spawn the controller in `region` (Rust SupervisorBuilder::compile,
 * CompiledSupervisor::bind_managed, ManagedSupervisor::spawn). Under lab
 * dispatch a spawn its admission refuses still returns a handle: its
 * join then reports ASX_E_CANCELLED.
 *
 * Returns ASX_OK; ASX_E_INVALID_ARGUMENT for a NULL argument, no or too
 * many children, a missing start function, max_restarts above
 * ASX_SUPERVISOR_MAX_RESTARTS, a storm threshold that is negative, NaN or
 * infinite, or a bad dependency; ASX_E_NAME_CONFLICT for
 * duplicate child names; ASX_E_RESOURCE_EXHAUSTED when every supervisor
 * slot is in use; the spawn's status otherwise. */
ASX_API ASX_MUST_USE asx_status asx_supervisor_spawn(asx_supervisor_handle *out,
                                                     asx_region_id region,
                                                     const asx_supervisor_config *config,
                                                     const asx_child_spec *children,
                                                     uint32_t child_count);

/* Ask the controller to stop (Rust ManagedSupervisorHandle::abort, also
 * its drop): a User "abort" cancel of its task. Its join still waits for
 * every generation to drain. */
ASX_API ASX_MUST_USE asx_status asx_supervisor_abort(asx_supervisor_handle sup);

/* Wait, from task `self`'s poll, for the controller to finish (Rust
 * ManagedSupervisorHandle::join): ASX_E_PENDING parks `self` until then.
 * ASX_OK: *out (may be NULL) holds the report. ASX_E_CANCELLED: the
 * controller never ran (cancelled before its first poll, or refused).
 * ASX_E_INVALID_STATE: already joined. A join is not interrupted by the
 * caller's cancellation. */
ASX_API ASX_MUST_USE asx_status asx_supervisor_join(asx_supervisor_handle sup, asx_task_id self,
                                                    asx_supervisor_report *out);

/* The controller's task, ASX_INVALID_ID if its spawn was refused. */
ASX_API asx_task_id asx_supervisor_task(asx_supervisor_handle sup);

/* The supervisor region, ASX_INVALID_ID until the controller opened it. */
ASX_API asx_region_id asx_supervisor_region(asx_supervisor_handle sup);

/* 1 while the controller has not finished. */
ASX_API int asx_supervisor_is_alive(asx_supervisor_handle sup);

/* 1 once the controller published its report: its join then returns
 * ASX_OK, whatever its task's own outcome (a cancel of its region after
 * its first poll does not take the report back). */
ASX_API int asx_supervisor_finished(asx_supervisor_handle sup);

/* Reset all supervisor state (asx_runtime_reset calls it). */
ASX_API void asx_supervisor_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_ACTOR_SUPERVISOR_H */
