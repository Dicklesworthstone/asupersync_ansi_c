/*
 * conformance/interpreter.h — the C side of the Rust-vs-C oracle (bead W1.5,
 * bd-9kll.2.5): runs an asx.scenario.v2 scenario through the asx runtime and
 * projects the run into vocabulary v2, the same way tools/twin_run projects
 * a run of asupersync's LabRuntime.
 *
 * Not part of the public API: the conformance tools and tests only.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_CONFORMANCE_INTERPRETER_H
#define ASX_CONFORMANCE_INTERPRETER_H

#include "json.h"

#include <asx/runtime/profile_compat.h>

#define ASX_INTERNAL_TRACE_FAMILY_ACCESS 1
#include <asx/runtime/lab.h>
#include <asx/runtime/replay.h>
#undef ASX_INTERNAL_TRACE_FAMILY_ACCESS

/* Run the scenario object `scenario` of `in` and build the projection in
 * `out` as an object with the fields an asx.fixture.v2 file carries for
 * comparison: "scenario_id", "trace" (Foata layers), "snapshot",
 * "observations", "dispatches" (the lab's dispatch order, compared with the
 * fixture's "schedule"."dispatches"), "certificate_hash" (16 hex digits)
 * and "decisions" (Rust's ScheduleCertificate and its count, compared with
 * the fixture's "schedule"."certificate_hash" / "decisions"),
 * "trace_digest", "snapshot_digest" and "semantic_digest".
 *
 * Fails closed: an op the interpreter or the C runtime cannot express, a
 * runtime event it cannot project, an exhausted max_steps budget, or a
 * failed must_fail expectation returns a non-OK status with a message in
 * `error`; *out_root is then unset and the run proves nothing.
 *
 * Shuts down its previous scenario runtime before initializing a fresh
 * singleton. An independently active runtime makes initialization fail.
 * `in` must outlive the call. */
ASX_MUST_USE asx_status asx_conformance_run(const asx_json_doc *in, uint32_t scenario,
                                            asx_json_doc *out, uint32_t *out_root, char *error,
                                            size_t error_cap);

/* As asx_conformance_run, but under the runtime's default sweep scheduler
 * instead of lab dispatch: what asx_scheduler_run does for a live user
 * (bd-9kll.4.9). Same fail-closed contract. Only "snapshot" and
 * "observations" of the projection are comparable with a Rust capture: the
 * sweep polls in its own order, so "trace", "dispatches" and the schedule
 * certificate (which it does not produce) are not. */
ASX_MUST_USE asx_status asx_conformance_run_sweep(const asx_json_doc *in, uint32_t scenario,
                                                  asx_json_doc *out, uint32_t *out_root,
                                                  char *error, size_t error_cap);

/* Replay a recorded schedule in the runs that follow, instead of the seeded
 * picker (asx_lab_forced_schedule_begin/_end around the run): every lab
 * step takes its task from `schedule`, and the run ends exactly where its
 * receipt says or fails with the first divergence, its step and dispatch
 * index in `error`. `names[i]` is the scenario task name of dispatch i (the
 * fixture's "schedule"."dispatches" gives it); a recorded task id maps to
 * the task of the first dispatch that carries it, when the scenario has a
 * task of that name. A task the runtime creates (a supervisor's
 * generation, a region finalizer, a controller) is no scenario task: its
 * id resolves through the lab's Rust arena shadow. Both must outlive the
 * runs. NULL clears the replay. */
void asx_conformance_force_schedule(const asx_forced_schedule *schedule, const char *const *names);

/* Record the forced schedule of every lab run that follows (Rust's
 * start/finish_forced_schedule_recording, lab/runtime.rs:2618; bd-9kll.9.3),
 * with Rust arena ids, so the run's artifact can be handed to Rust's
 * run_forced_schedule. `config_hash` is what the artifact carries as the
 * lab's configuration hash (Rust's is a LabConfig hash that C does not
 * compute: the fixture's own artifact has it). Not recorded while a schedule
 * is being replayed (asx_conformance_force_schedule). `on` 0 disarms. */
void asx_conformance_record_schedule(int on, uint64_t config_hash);

/* The last run's recorded schedule: 1 and *out filled (its dispatches live
 * in interpreter storage until the next run), or 0 when nothing was
 * recorded, the run ended with work stuck (no canonical form), or it had
 * more dispatches than the storage holds. */
int asx_conformance_recorded_schedule(asx_forced_schedule *out);

/* Run these kernel-state oracles (docs/LAB_ORACLE_CATALOG.md) at the end of
 * every run that follows, as if each scenario had listed them in its
 * lab.oracles: a comma-separated list of Rust registry names ("task_leak",
 * "region_tree", ...) or "all". A violation fails the run with the oracle's
 * message. NULL clears. `csv` must outlive the runs. */
void asx_conformance_set_oracles(const char *csv);

/* The vocabulary name ("<holder>/o<k>") the last run's projection gave the
 * obligation with this handle (a ghost violation's or a trace event's
 * entity), or NULL when the run never named it. The ghost-checked runner
 * uses it to ask whether Rust's trace expects that obligation to leak. */
const char *asx_conformance_obligation_name(uint64_t entity_id);

/* Whether the scenario object `scenario` of `in` declares a requirement
 * ("requires": ["unbounded_queue"], DSL section 1) that `profile` forbids,
 * so a run in that profile has nothing to prove and the runner skips it
 * (DEC-008). The requirement names map to the overload catalog's forbidden
 * behaviors: "unbounded_queue" to ASX_FORBID_UNBOUNDED_QUEUE. Returns 1
 * and sets *out_name to the first such requirement; 0 and *out_name NULL
 * when the scenario declares none or the profile allows every one it does;
 * -1 (*out_name NULL) for a "requires" that is not an array of known
 * requirement names, or an unknown profile: a declaration the interpreter
 * cannot honor is an error, never a skip. *out_name points into `in`. */
int asx_conformance_requirement_forbidden(const asx_json_doc *in, uint32_t scenario,
                                          asx_profile_id profile, const char **out_name);

#endif /* ASX_CONFORMANCE_INTERPRETER_H */
