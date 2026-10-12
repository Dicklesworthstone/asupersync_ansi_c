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

#define ASX_INTERNAL_TRACE_FAMILY_ACCESS 1
#include <asx/runtime/lab.h>
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

/* Replay a recorded schedule in the runs that follow, instead of the seeded
 * picker (asx_lab_forced_schedule_begin/_end around the run): every lab
 * step takes its task from `schedule`, and the run ends exactly where its
 * receipt says or fails with the first divergence, its step and dispatch
 * index in `error`. `names[i]` is the scenario task name of dispatch i (the
 * fixture's "schedule"."dispatches" gives it); a recorded task id maps to
 * the task of the first dispatch that carries it. Both must outlive the
 * runs. NULL clears the replay. */
void asx_conformance_force_schedule(const asx_forced_schedule *schedule, const char *const *names);

#endif /* ASX_CONFORMANCE_INTERPRETER_H */
