//! Runs one asx.scenario.v2 scenario inside asupersync's LabRuntime and
//! projects it into asx.vocab.v2 (docs/SCENARIO_DSL_V2.md,
//! docs/CANONICAL_VOCABULARY_V2.md).
//!
//! Increment 1 implements the driver (setup, script, finish), the control,
//! masking, obligation, spawn/join and child-region steps, trace projection
//! and the snapshot. Channel, sync, group and actor steps are not yet
//! interpreted: a scenario that uses one fails as a harness error, never
//! silently.

use std::collections::HashMap;
use std::future::Future;
use std::pin::Pin;
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::Duration;

use asupersync::cx::{ChildRegion, ChildRegionSpec};
use asupersync::lab::{LabConfig, LabRuntime};
use asupersync::record::task::TaskState;
use asupersync::record::{ObligationAbortReason, ObligationKind};
use asupersync::runtime::obligation_mailbox::ObligationToken;
use asupersync::runtime::{JoinError, TaskHandle};
use asupersync::trace::{TraceData, TraceEventKind};
use asupersync::{Budget, CancelKind, CancelReason, Cx, RegionId, TaskId, Time};
use serde_json::{Value, json};

use crate::canon;

pub type RunResult<T> = Result<T, String>;

/// What a task body returns. Cancellation and panics are not values: the
/// runtime reports them through the task handle (`JoinError`).
#[derive(Debug)]
pub enum Body {
    Ok,
    Err(String),
}

/// State shared between the driver and every interpreted task body.
#[derive(Default)]
struct Shared {
    /// Canonical (admitted) task ids; see `resolve_admissions`.
    task_ids: HashMap<String, TaskId>,
    task_names: HashMap<TaskId, String>,
    /// Children spawned through `cx.spawn` by provisional mailbox id, until
    /// their admission is resolved.
    provisional: HashMap<TaskId, String>,
    /// Task name -> name of the region it runs in.
    task_regions: HashMap<String, String>,
    region_ids: HashMap<String, RegionId>,
    region_names: HashMap<RegionId, String>,
    region_parents: HashMap<String, Option<String>>,
    timer_names: HashMap<u64, String>,
    handles: HashMap<String, TaskHandle<Body>>,
    /// Join results, projected only after the run (`finish_outcomes`): a
    /// cancel reason may name a child whose canonical id is not resolved yet.
    raw_outcomes: HashMap<String, Result<Body, JoinError>>,
    outcomes: HashMap<String, Value>,
    /// (observation index, joined task) for join observations whose value is
    /// the projected outcome.
    outcome_observations: Vec<(usize, String)>,
    observations: Vec<Value>,
    /// Harness errors raised inside task bodies. Any entry fails the run.
    errors: Vec<String>,
}

type SharedRef = Arc<Mutex<Shared>>;

fn lock(shared: &SharedRef) -> MutexGuard<'_, Shared> {
    // A panicking task body (the DSL `return panicked` step) must not make
    // the shared state unusable for the driver.
    shared
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
}

// ---------------------------------------------------------------------------
// Scenario field helpers
// ---------------------------------------------------------------------------

fn str_field<'a>(v: &'a Value, key: &str) -> RunResult<&'a str> {
    v.get(key)
        .and_then(Value::as_str)
        .ok_or_else(|| format!("missing string field {key:?} in {v}"))
}

fn u64_field(v: &Value, key: &str) -> RunResult<u64> {
    v.get(key)
        .and_then(Value::as_u64)
        .ok_or_else(|| format!("missing integer field {key:?} in {v}"))
}

fn budget(v: Option<&Value>) -> RunResult<Budget> {
    let Some(v) = v.filter(|v| !v.is_null()) else {
        return Ok(Budget::INFINITE);
    };
    let mut b = Budget::new();
    if let Some(d) = v.get("deadline_ns").and_then(Value::as_u64) {
        b = b.with_deadline(Time::from_nanos(d));
    }
    if let Some(p) = v.get("poll_quota").and_then(Value::as_u64) {
        b = b.with_poll_quota(
            u32::try_from(p).map_err(|_| format!("poll_quota out of range: {p}"))?,
        );
    }
    if let Some(c) = v.get("cost_quota").and_then(Value::as_u64) {
        b = b.with_cost_quota(c);
    }
    if let Some(p) = v.get("priority").and_then(Value::as_u64) {
        b = b.with_priority(u8::try_from(p).map_err(|_| format!("priority out of range: {p}"))?);
    }
    Ok(b)
}

fn cancel_kind(name: &str) -> RunResult<CancelKind> {
    Ok(match name {
        "User" => CancelKind::User,
        "Timeout" => CancelKind::Timeout,
        "Deadline" => CancelKind::Deadline,
        "PollQuota" => CancelKind::PollQuota,
        "CostBudget" => CancelKind::CostBudget,
        "FailFast" => CancelKind::FailFast,
        "RaceLost" => CancelKind::RaceLost,
        "ParentCancelled" => CancelKind::ParentCancelled,
        "ResourceUnavailable" => CancelKind::ResourceUnavailable,
        "Shutdown" => CancelKind::Shutdown,
        "LinkedExit" => CancelKind::LinkedExit,
        other => return Err(format!("unknown cancel kind {other:?}")),
    })
}

/// A cancel reason with real attribution, built the way asupersync's own
/// request paths build one (`CancelReason::with_origin(kind, region, now)
/// .with_task(task)`, cx/cx.rs:3940). `CancelReason::new` is a testing
/// default (origin `RegionId::testing_default()`, timestamp fixed at 1 s,
/// types/cancel.rs:590-605) and must not reach a fixture.
fn cancel_reason(
    v: &Value,
    origin_region: RegionId,
    origin_task: Option<TaskId>,
    now: Time,
) -> RunResult<CancelReason> {
    let mut reason =
        CancelReason::with_origin(cancel_kind(str_field(v, "kind")?)?, origin_region, now);
    if let Some(task) = origin_task {
        reason = reason.with_task(task);
    }
    Ok(match v.get("message").and_then(Value::as_str) {
        // `with_message` takes `&'static str`; one capture process runs a
        // bounded set of scenarios, so leaking each scenario message is bounded.
        Some(m) => reason.with_message(Box::leak(m.to_owned().into_boxed_str())),
        None => reason,
    })
}

fn obligation_kind(name: &str) -> RunResult<ObligationKind> {
    Ok(match name {
        "SendPermit" => ObligationKind::SendPermit,
        "Ack" => ObligationKind::Ack,
        "Lease" => ObligationKind::Lease,
        "IoOp" => ObligationKind::IoOp,
        "SemaphorePermit" => ObligationKind::SemaphorePermit,
        "Transaction" => ObligationKind::Transaction,
        other => return Err(format!("unknown obligation kind {other:?}")),
    })
}

fn abort_reason(name: &str) -> RunResult<ObligationAbortReason> {
    Ok(match name {
        "Cancel" => ObligationAbortReason::Cancel,
        "Error" => ObligationAbortReason::Error,
        "Explicit" => ObligationAbortReason::Explicit,
        other => return Err(format!("unknown abort reason {other:?}")),
    })
}

/// Rust error kinds onto C status names (docs/CANONICAL_VOCABULARY_V2.md §5).
/// A kind the table does not map fails the capture: inventing a status here
/// would hand the comparator a plausible value nobody specified.
fn status_of_error(err: &asupersync::error::Error) -> RunResult<String> {
    use asupersync::error::ErrorKind as K;
    Ok(match err.kind() {
        K::Cancelled => "ASX_E_CANCELLED",
        K::CancelTimeout | K::DeadlineExceeded => "ASX_E_TIMED_OUT",
        K::PollQuotaExhausted => "ASX_E_POLL_QUOTA_EXHAUSTED",
        K::CostQuotaExhausted => "ASX_E_COST_QUOTA_EXHAUSTED",
        K::ChannelClosed => "ASX_E_DISCONNECTED",
        K::ChannelFull => "ASX_E_CHANNEL_FULL",
        K::ChannelEmpty => "ASX_E_CHANNEL_EMPTY",
        K::ObligationAlreadyResolved => "ASX_E_OBLIGATION_ALREADY_RESOLVED",
        K::ObligationLeak => "ASX_E_UNRESOLVED_OBLIGATIONS",
        K::RegionClosed | K::RegionFinalized => "ASX_E_REGION_CLOSED",
        K::AdmissionDenied => "ASX_E_ADMISSION_CLOSED",
        K::ThresholdTimeout => "ASX_E_THRESHOLD_TIMEOUT",
        K::InvalidInput => "ASX_E_INVALID_ARGUMENT",
        K::InvalidStateTransition => "ASX_E_INVALID_STATE",
        K::RateLimited => "ASX_E_OVERLOADED",
        K::DuplicateSymbol => "ASX_E_DUPLICATE_SYMBOL",
        K::ObjectMismatch => "ASX_E_OBJECT_MISMATCH",
        K::CorruptedSymbol => "ASX_E_CORRUPTED_SYMBOL",
        other => {
            return Err(format!(
                "Rust ErrorKind::{other:?} has no status in vocabulary §5"
            ));
        }
    }
    .to_string())
}

// ---------------------------------------------------------------------------
// Projection helpers
// ---------------------------------------------------------------------------

fn kind_name(kind: CancelKind) -> String {
    format!("{kind:?}")
}

fn region_name(shared: &Shared, id: RegionId) -> RunResult<String> {
    shared
        .region_names
        .get(&id)
        .cloned()
        .ok_or_else(|| format!("unnamed region {id:?} (harness defect)"))
}

fn task_name(shared: &Shared, id: TaskId) -> RunResult<String> {
    shared
        .task_names
        .get(&id)
        .cloned()
        .ok_or_else(|| format!("unnamed task {id:?} (harness defect)"))
}

fn project_reason(shared: &Shared, reason: &CancelReason) -> RunResult<Value> {
    let mut chain = 0u64;
    let mut cause = reason.cause.as_deref();
    while let Some(c) = cause {
        chain += 1;
        cause = c.cause.as_deref();
    }
    Ok(json!({
        "kind": kind_name(reason.kind),
        "origin_region": region_name(shared, reason.origin_region)?,
        "origin_task": match reason.origin_task {
            Some(t) => Value::String(task_name(shared, t)?),
            None => Value::Null,
        },
        "timestamp_ns": reason.timestamp.as_nanos(),
        "message": reason.message.clone(),
        "cause_chain_len": chain,
        "truncated": reason.truncated,
    }))
}

fn project_outcome(shared: &Shared, result: Result<Body, JoinError>) -> RunResult<Value> {
    Ok(match result {
        Ok(Body::Ok) => json!({"tag": "ok"}),
        Ok(Body::Err(status)) => json!({"tag": "err", "status": status}),
        Err(JoinError::Cancelled(reason)) => {
            json!({"tag": "cancelled", "reason": project_reason(shared, &reason)?})
        }
        Err(JoinError::Panicked(payload)) => {
            json!({"tag": "panicked", "message": payload.message()})
        }
        Err(other) => return Err(format!("unexpected join result {other:?}")),
    })
}

fn observe(shared: &SharedRef, task: &str, step: usize, op: &str, status: &str, value: Value) {
    lock(shared).observations.push(json!({
        "task": task, "step": step, "op": op, "status": status, "value": value,
    }));
}

/// Record a successful join of `target`: its raw result, and an observation
/// whose value `finish_outcomes` fills with the projected outcome.
fn observe_join(
    s: &mut Shared,
    task: &str,
    step: usize,
    op: &str,
    target: &str,
    result: Result<Body, JoinError>,
) {
    s.raw_outcomes.insert(target.to_string(), result);
    s.outcome_observations
        .push((s.observations.len(), target.to_string()));
    s.observations.push(
        json!({"task": task, "step": step, "op": op, "status": "ASX_OK", "value": Value::Null}),
    );
}

/// Project every join result, once all task names are resolved.
fn finish_outcomes(s: &mut Shared) -> RunResult<()> {
    let raw = std::mem::take(&mut s.raw_outcomes);
    for (name, result) in raw {
        let value = project_outcome(s, result)?;
        s.outcomes.insert(name, value);
    }
    for (index, target) in std::mem::take(&mut s.outcome_observations) {
        let value = s
            .outcomes
            .get(&target)
            .cloned()
            .ok_or_else(|| format!("join of {target:?} has no outcome"))?;
        s.observations[index]["value"] = value;
    }
    Ok(())
}

fn harness_error(shared: &SharedRef, msg: String) {
    lock(shared).errors.push(msg);
}

// ---------------------------------------------------------------------------
// The step interpreter
// ---------------------------------------------------------------------------

/// Per-task interpreter state that never leaves the task.
#[derive(Default)]
struct Local {
    tokens: HashMap<String, ObligationToken>,
    regions: HashMap<String, ChildRegion>,
    timers: u32,
}

enum Flow {
    Continue,
    Return(Body),
}

struct TaskCtx {
    shared: SharedRef,
    me: String,
    region: String,
}

fn run_program(
    cx: Cx,
    ctx: Arc<TaskCtx>,
    steps: Vec<Value>,
) -> Pin<Box<dyn Future<Output = Body> + Send>> {
    Box::pin(async move {
        let mut local = Local::default();
        for (i, step) in steps.iter().enumerate() {
            match exec_step(&cx, &ctx, &mut local, i + 1, step).await {
                Ok(Flow::Continue) => {}
                Ok(Flow::Return(body)) => return body,
                Err(err) => {
                    harness_error(
                        &ctx.shared,
                        format!("task {} step {}: {err}", ctx.me, i + 1),
                    );
                    return Body::Err("ASX_E_INVALID_STATE".to_string());
                }
            }
        }
        Body::Ok
    })
}

/// Non-blocking steps; also the only steps allowed inside `masked`.
fn exec_sync(
    cx: &Cx,
    ctx: &Arc<TaskCtx>,
    local: &mut Local,
    idx: usize,
    label: &str,
    step: &Value,
) -> RunResult<Flow> {
    let op = str_field(step, "op")?;
    let me = ctx.me.as_str();
    match op {
        "checkpoint" => {
            let status = match cx.checkpoint() {
                Ok(()) => "ASX_OK".to_string(),
                Err(e) => status_of_error(&e)?,
            };
            observe(&ctx.shared, me, idx, label, &status, Value::Null);
            let on_cancel = step
                .get("on_cancel")
                .and_then(Value::as_str)
                .unwrap_or("return");
            if status == "ASX_E_CANCELLED" && on_cancel == "return" {
                return Ok(Flow::Return(Body::Ok));
            }
        }
        "trace" => {
            cx.trace(str_field(step, "message")?);
            observe(&ctx.shared, me, idx, label, "ASX_OK", Value::Null);
        }
        "reserve" => {
            let kind = obligation_kind(str_field(step, "kind")?)?;
            let name = str_field(step, "as")?.to_string();
            let status = match cx.try_register_obligation_checked(kind, cx.task_id()) {
                Ok(Some(token)) => {
                    local.tokens.insert(name, token);
                    "ASX_OK".to_string()
                }
                Ok(None) => return Err("obligation registration without a runtime".to_string()),
                Err(e) => obligation_admission_status(&e)?,
            };
            observe(&ctx.shared, me, idx, label, &status, Value::Null);
        }
        "commit" | "abort" | "leak" => {
            let name = str_field(step, "obligation")?;
            let token = local
                .tokens
                .remove(name)
                .ok_or_else(|| format!("unknown obligation {name:?}"))?;
            let delivered = match op {
                "commit" => token.commit(),
                "abort" => token.abort(abort_reason(str_field(step, "reason")?)?),
                _ => {
                    drop(token);
                    true
                }
            };
            observe(
                &ctx.shared,
                me,
                idx,
                label,
                if delivered {
                    "ASX_OK"
                } else {
                    "ASX_E_INVALID_STATE"
                },
                Value::Null,
            );
        }
        "spawn" => {
            let child = str_field(step, "as")?.to_string();
            let program = step
                .get("program")
                .and_then(Value::as_array)
                .cloned()
                .ok_or("spawn without program")?;
            let child_region = match step.get("region").and_then(Value::as_str) {
                Some(r) => r.to_string(),
                None => ctx.region.clone(),
            };
            let child_region_name = child_region.clone();
            let child_ctx = Arc::new(TaskCtx {
                shared: ctx.shared.clone(),
                me: child.clone(),
                region: child_region,
            });
            let factory = move |ccx: Cx| run_program(ccx, child_ctx, program);
            let spawned = if let Some(r) = step.get("region").and_then(Value::as_str) {
                let region = local
                    .regions
                    .get(r)
                    .ok_or_else(|| format!("spawn into unknown region {r:?}"))?;
                region.cx().spawn(factory)
            } else if step.get("budget").is_some() {
                let b = budget(step.get("budget"))?;
                cx.spawn_in(&cx.scope_with_budget(b), factory)
            } else {
                cx.spawn(factory)
            };
            match spawned {
                Ok(handle) => {
                    // `handle.task_id()` is the provisional mailbox id here;
                    // resolve_admissions maps it to the canonical id.
                    let mut s = lock(&ctx.shared);
                    s.provisional.insert(handle.task_id(), child.clone());
                    s.task_regions.insert(child.clone(), child_region_name);
                    s.handles.insert(child, handle);
                    drop(s);
                    observe(&ctx.shared, me, idx, label, "ASX_OK", Value::Null);
                }
                Err(e) => observe(&ctx.shared, me, idx, label, &spawn_status(&e)?, Value::Null),
            }
        }
        "try_join" => {
            let target = str_field(step, "task")?;
            let s = &mut *lock(&ctx.shared);
            let handle = s
                .handles
                .get_mut(target)
                .ok_or_else(|| format!("unknown task {target:?}"))?;
            let result = match handle.try_join() {
                Ok(None) => None,
                Ok(Some(body)) => Some(Ok(body)),
                Err(e) => Some(Err(e)),
            };
            match result {
                None => s.observations.push(json!({
                    "task": me, "step": idx, "op": label, "status": "ASX_E_TASK_NOT_COMPLETED", "value": Value::Null,
                })),
                Some(result) => {
                    s.handles.remove(target);
                    observe_join(s, me, idx, label, target, result);
                }
            }
        }
        "abort_task" => {
            let target = str_field(step, "task")?;
            // The requesting task initiates the cancel (cx.cancel_with).
            let reason = cancel_reason(step, cx.region_id(), Some(cx.task_id()), cx.now())?;
            let s = lock(&ctx.shared);
            let handle = s
                .handles
                .get(target)
                .ok_or_else(|| format!("unknown task {target:?}"))?;
            handle.abort_with_reason(reason);
            drop(s);
            observe(&ctx.shared, me, idx, label, "ASX_OK", Value::Null);
        }
        "cancel_region" => {
            let name = str_field(step, "region")?;
            let region = local
                .regions
                .get(name)
                .ok_or_else(|| format!("unknown child region {name:?}"))?;
            let reason = cancel_reason(step, cx.region_id(), Some(cx.task_id()), cx.now())?;
            let status = match region.cancel(reason) {
                Ok(()) => "ASX_OK".to_string(),
                Err(e) => child_region_status(&e)?,
            };
            observe(&ctx.shared, me, idx, label, &status, Value::Null);
        }
        other => return Err(format!("op {other:?} is not interpreted yet (increment 1)")),
    }
    Ok(Flow::Continue)
}

// The three mappers below follow vocabulary §5 row by row; a variant §5 does
// not name fails the capture rather than picking a status.

fn obligation_admission_status(
    err: &asupersync::runtime::obligation_mailbox::ObligationAdmissionError,
) -> RunResult<String> {
    use asupersync::runtime::obligation_mailbox::ObligationAdmissionError as E;
    Ok(match err {
        E::RuntimeUnavailable => "ASX_E_SCHEDULER_UNAVAILABLE",
        E::RegionClosed => "ASX_E_REGION_CLOSED",
        E::LimitReached { .. } => "ASX_E_ADMISSION_LIMIT",
        E::CapacityExhausted => "ASX_E_RESOURCE_EXHAUSTED",
        other => {
            return Err(format!(
                "obligation admission error {other:?} has no status in vocabulary §5"
            ));
        }
    }
    .to_string())
}

fn child_region_status(err: &asupersync::cx::ChildRegionError) -> RunResult<String> {
    use asupersync::cx::ChildRegionError as E;
    use asupersync::runtime::region_table::RegionCreateError as C;
    Ok(match err {
        E::NoRuntimeGateway | E::RuntimeUnavailable => "ASX_E_SCHEDULER_UNAVAILABLE",
        E::Create(C::ParentNotFound(_)) => "ASX_E_REGION_NOT_FOUND",
        E::Create(C::ParentClosed { .. }) => "ASX_E_REGION_CLOSED",
        E::Create(C::ParentAtCapacity { .. }) => "ASX_E_ADMISSION_LIMIT",
        #[allow(unreachable_patterns)]
        other => {
            return Err(format!(
                "child region error {other:?} has no status in vocabulary §5"
            ));
        }
    }
    .to_string())
}

fn spawn_status(err: &asupersync::runtime::state::SpawnError) -> RunResult<String> {
    use asupersync::runtime::state::SpawnError as E;
    Ok(match err {
        E::RuntimeUnavailable | E::LocalSchedulerUnavailable => "ASX_E_SCHEDULER_UNAVAILABLE",
        E::RegionNotFound(_) => "ASX_E_REGION_NOT_FOUND",
        E::RegionClosed(_) => "ASX_E_REGION_CLOSED",
        E::RegionAtCapacity { .. } => "ASX_E_ADMISSION_LIMIT",
        #[allow(unreachable_patterns)]
        other => {
            return Err(format!(
                "spawn error {other:?} has no status in vocabulary §5"
            ));
        }
    }
    .to_string())
}

async fn exec_step(
    cx: &Cx,
    ctx: &Arc<TaskCtx>,
    local: &mut Local,
    idx: usize,
    step: &Value,
) -> RunResult<Flow> {
    let op = str_field(step, "op")?;
    let me = ctx.me.as_str();
    match op {
        "yield" => {
            asupersync::runtime::yield_now().await;
            observe(&ctx.shared, me, idx, op, "ASX_OK", Value::Null);
        }
        "sleep" | "sleep_until" => {
            local.timers += 1;
            let timer = format!("{me}/tm{}", local.timers);
            let deadline = if op == "sleep" {
                Time::from_nanos(cx.now().as_nanos().saturating_add(u64_field(step, "ns")?))
            } else {
                Time::from_nanos(u64_field(step, "at_ns")?)
            };
            let sleep = if op == "sleep" {
                asupersync::time::sleep(cx.now(), Duration::from_nanos(u64_field(step, "ns")?))
            } else {
                asupersync::time::sleep_until(deadline)
            };
            name_timer_on_first_poll(cx, &ctx.shared, timer, sleep).await;
            observe(&ctx.shared, me, idx, op, "ASX_OK", Value::Null);
        }
        "return" => {
            let outcome = step.get("outcome").ok_or("return without outcome")?;
            observe(&ctx.shared, me, idx, op, "ASX_OK", Value::Null);
            return Ok(Flow::Return(match str_field(outcome, "tag")? {
                "ok" => Body::Ok,
                "err" => Body::Err(str_field(outcome, "status")?.to_string()),
                // The DSL step *is* a panic; the runtime catches it and
                // reports JoinError::Panicked with this message.
                "panicked" => std::panic::panic_any(str_field(outcome, "message")?.to_string()),
                other => return Err(format!("unknown outcome tag {other:?}")),
            }));
        }
        "masked" => {
            let steps = step
                .get("steps")
                .and_then(Value::as_array)
                .ok_or("masked without steps")?;
            let result = cx.masked(|| -> RunResult<Flow> {
                for (j, inner) in steps.iter().enumerate() {
                    let label = format!("masked/{}/{}", j + 1, str_field(inner, "op")?);
                    if let Flow::Return(body) = exec_sync(cx, ctx, local, idx, &label, inner)? {
                        return Ok(Flow::Return(body));
                    }
                }
                Ok(Flow::Continue)
            });
            return result;
        }
        "join" => {
            let target = str_field(step, "task")?;
            let handle = lock(&ctx.shared).handles.remove(target);
            let mut handle =
                handle.ok_or_else(|| format!("unknown or already joined task {target:?}"))?;
            let result = handle.join(cx).await;
            observe_join(&mut lock(&ctx.shared), me, idx, op, target, result);
        }
        "open_region" => {
            let name = str_field(step, "as")?.to_string();
            let mut spec = ChildRegionSpec::inherit();
            if step.get("budget").is_some() {
                spec = spec.with_budget(budget(step.get("budget"))?);
            }
            match cx.open_child_region(spec).await {
                Ok(child) => {
                    let mut s = lock(&ctx.shared);
                    s.region_ids.insert(name.clone(), child.region_id());
                    s.region_names.insert(child.region_id(), name.clone());
                    s.region_parents
                        .insert(name.clone(), Some(ctx.region.clone()));
                    drop(s);
                    local.regions.insert(name, child);
                    observe(&ctx.shared, me, idx, op, "ASX_OK", Value::Null);
                }
                Err(e) => observe(
                    &ctx.shared,
                    me,
                    idx,
                    op,
                    &child_region_status(&e)?,
                    Value::Null,
                ),
            }
        }
        "close_region" => {
            let name = str_field(step, "region")?;
            let region = local
                .regions
                .remove(name)
                .ok_or_else(|| format!("unknown child region {name:?}"))?;
            let status = match region.close().await {
                Ok(()) => "ASX_OK".to_string(),
                Err(e) => child_region_status(&e)?,
            };
            observe(&ctx.shared, me, idx, op, &status, Value::Null);
        }
        _ => return exec_sync(cx, ctx, local, idx, op, step),
    }
    Ok(Flow::Continue)
}

/// Await a sleep, naming the timer it schedules on its first poll. The lab
/// trace's Timer events carry no task, so the owner is identified by being
/// the task that is polling when TimerScheduled is recorded (worker_count 1).
async fn name_timer_on_first_poll<F: Future<Output = ()>>(
    cx: &Cx,
    shared: &SharedRef,
    name: String,
    sleep: F,
) {
    let buffer = cx.trace_buffer();
    let before = buffer
        .as_ref()
        .map_or(0, |b| b.snapshot().last().map_or(0, |e| e.seq));
    let mut sleep = std::pin::pin!(sleep);
    let mut first = true;
    std::future::poll_fn(|pcx| {
        let polled = sleep.as_mut().poll(pcx);
        if first {
            first = false;
            if let Some(buffer) = &buffer {
                let mut s = lock(shared);
                for event in buffer.snapshot().iter().filter(|e| e.seq > before) {
                    if let (TraceEventKind::TimerScheduled, TraceData::Timer { timer_id, .. }) =
                        (&event.kind, &event.data)
                    {
                        s.timer_names.insert(*timer_id, name.clone());
                    }
                }
            }
        }
        polled
    })
    .await;
}

// ---------------------------------------------------------------------------
// The driver
// ---------------------------------------------------------------------------

pub fn run_scenario(scenario: &Value) -> RunResult<Value> {
    if scenario.get("schema").and_then(Value::as_str) != Some("asx.scenario.v2") {
        return Err("not an asx.scenario.v2 document".to_string());
    }
    let lab_cfg = scenario.get("lab").ok_or("missing lab")?;
    let config = LabConfig::new(u64_field(scenario, "seed")?)
        .worker_count(1)
        .trace_capacity(1 << 20)
        .max_steps(u64_field(lab_cfg, "max_steps")?)
        .panic_on_leak(
            lab_cfg
                .get("panic_on_leak")
                .and_then(Value::as_bool)
                .unwrap_or(false),
        );
    let mut lab = LabRuntime::new(config);
    lab.start_forced_schedule_recording(1 << 20)
        .map_err(|e| format!("{e:?}"))?;
    let shared: SharedRef = Arc::new(Mutex::new(Shared::default()));

    for (field, _) in [("channels", ()), ("sync", ())] {
        if scenario
            .get(field)
            .and_then(Value::as_array)
            .is_some_and(|a| !a.is_empty())
        {
            return Err(format!(
                "{field} declarations are not interpreted yet (increment 1)"
            ));
        }
    }

    // Setup: root, regions, tasks.
    let root = lab.state.create_root_region(Budget::INFINITE);
    {
        let mut s = lock(&shared);
        s.region_ids.insert("root".into(), root);
        s.region_names.insert(root, "root".into());
        s.region_parents.insert("root".into(), None);
    }
    for r in scenario
        .get("regions")
        .and_then(Value::as_array)
        .into_iter()
        .flatten()
    {
        let name = str_field(r, "name")?.to_string();
        let parent = str_field(r, "parent")?.to_string();
        let parent_id = *lock(&shared)
            .region_ids
            .get(&parent)
            .ok_or_else(|| format!("unknown parent {parent:?}"))?;
        let id = lab
            .state
            .create_child_region(parent_id, budget(r.get("budget"))?)
            .map_err(|e| format!("create region {name}: {e:?}"))?;
        let mut s = lock(&shared);
        s.region_ids.insert(name.clone(), id);
        s.region_names.insert(id, name.clone());
        s.region_parents.insert(name, Some(parent));
    }
    for t in scenario
        .get("tasks")
        .and_then(Value::as_array)
        .into_iter()
        .flatten()
    {
        let name = str_field(t, "name")?.to_string();
        let region = str_field(t, "region")?.to_string();
        let region_id = *lock(&shared)
            .region_ids
            .get(&region)
            .ok_or_else(|| format!("unknown region {region:?}"))?;
        let program = t
            .get("program")
            .and_then(Value::as_array)
            .cloned()
            .ok_or("task without program")?;
        lock(&shared)
            .task_regions
            .insert(name.clone(), region.clone());
        let ctx = Arc::new(TaskCtx {
            shared: shared.clone(),
            me: name.clone(),
            region,
        });
        let body = async move {
            match Cx::current() {
                Some(cx) => run_program(cx, ctx, program).await,
                None => {
                    harness_error(&ctx.shared, format!("task {}: no Cx", ctx.me));
                    Body::Err("ASX_E_INVALID_STATE".to_string())
                }
            }
        };
        let (task_id, handle) = lab
            .state
            .create_task(region_id, budget(t.get("budget"))?, body)
            .map_err(|e| format!("create task {name}: {e:?}"))?;
        lab.scheduler.lock().schedule(task_id, 0);
        let mut s = lock(&shared);
        s.task_ids.insert(name.clone(), task_id);
        s.task_names.insert(task_id, name.clone());
        s.handles.insert(name, handle);
    }

    // Script: at each entry, run to idle, advance time, apply the op.
    for entry in scenario
        .get("script")
        .and_then(Value::as_array)
        .into_iter()
        .flatten()
    {
        lab.run_until_idle();
        let at = u64_field(entry, "at_ns")?;
        if at > lab.now().as_nanos() {
            lab.advance_time_to(Time::from_nanos(at));
        }
        apply_driver_op(
            &mut lab,
            &shared,
            entry.get("op").ok_or("script entry without op")?,
        )?;
    }

    // Finish: run to quiescence with timer auto-advance.
    lab.run_with_auto_advance();

    let errors = std::mem::take(&mut lock(&shared).errors);
    if !errors.is_empty() {
        return Err(format!("harness errors: {}", errors.join("; ")));
    }

    let schedule = lab
        .finish_forced_schedule_recording()
        .map_err(|e| format!("{e:?}"))?;
    let schedule_bytes = schedule
        .to_canonical_bytes()
        .map_err(|e| format!("{e:?}"))?;

    // Harvest the outcomes of tasks nobody joined.
    {
        let s = &mut *lock(&shared);
        let names: Vec<String> = s.handles.keys().cloned().collect();
        for name in names {
            if let Some(mut handle) = s.handles.remove(&name) {
                match handle.try_join() {
                    Ok(Some(body)) => {
                        s.raw_outcomes.insert(name, Ok(body));
                    }
                    Ok(None) => {}
                    Err(e) => {
                        s.raw_outcomes.insert(name, Err(e));
                    }
                }
            }
        }
    }

    resolve_admissions(&lab, &shared)?;
    if let Some(unadmitted) = lock(&shared).provisional.values().next() {
        return Err(format!(
            "spawned task {unadmitted:?} was never admitted (scenario or harness defect)"
        ));
    }
    finish_outcomes(&mut lock(&shared))?;
    let events = project_trace(&lab, &shared)?;
    let trace = canon::canonical_trace(&events.events)?;
    let snapshot = build_snapshot(&lab, &shared, &events)?;
    let mut observations = std::mem::take(&mut lock(&shared).observations);
    observations.sort_by(|a, b| {
        let key = |v: &Value| {
            (
                v["task"].as_str().unwrap_or("").to_string(),
                v["step"].as_u64().unwrap_or(0),
                v["op"].as_str().unwrap_or("").to_string(),
            )
        };
        key(a).cmp(&key(b))
    });
    check_expectation(scenario, &observations)?;

    let trace_value = serde_json::to_value(&trace).map_err(|e| e.to_string())?;
    let semantic = json!({
        "observations": observations, "snapshot": snapshot, "trace": trace_value, "vocabulary": "asx.vocab.v2",
    });
    Ok(json!({
        "schema": "asx.fixture.v2",
        "scenario_id": scenario["id"],
        "scenario": scenario,
        "vocabulary": "asx.vocab.v2",
        "trace": trace_value,
        "snapshot": snapshot,
        "observations": observations,
        "trace_digest": canon::digest(&trace_value)?,
        "snapshot_digest": canon::digest(&snapshot)?,
        "semantic_digest": canon::digest(&semantic)?,
        "schedule": {
            "certificate_hash": format!("{:016x}", lab.certificate().hash()),
            "decisions": lab.certificate().decisions(),
            "forced_schedule": schedule_bytes.iter().map(|b| format!("{b:02x}")).collect::<String>(),
        },
    }))
}

fn apply_driver_op(lab: &mut LabRuntime, shared: &SharedRef, op: &Value) -> RunResult<()> {
    let region_id = |field: &str| -> RunResult<RegionId> {
        let name = str_field(op, field)?;
        lock(shared)
            .region_ids
            .get(name)
            .copied()
            .ok_or_else(|| format!("unknown region {name:?}"))
    };
    match str_field(op, "op")? {
        "cancel_region" | "close_region" => {
            let region = region_id("region")?;
            // No task requests a driver cancel; it originates at the target
            // region (as app.rs:348 attributes a region's own deadline).
            let reason = cancel_reason(op, region, None, lab.now())?;
            let (tasks, wakes) = lab.state.cancel_request(region, &reason, None).into_parts();
            for (task, priority) in tasks {
                lab.scheduler.lock().schedule_cancel(task, priority);
            }
            wakes.dispatch();
            if str_field(op, "op")? == "close_region" {
                for _ in 0..1000 {
                    lab.run_until_idle();
                    lab.state.advance_region_state(region);
                    if lab.state.region_was_closed(region) {
                        return Ok(());
                    }
                }
                return Err(
                    "close_region: region did not close within 1000 idle rounds".to_string()
                );
            }
        }
        "cancel_task" => {
            let name = str_field(op, "task")?;
            resolve_admissions(lab, shared)?;
            let (task, owner) = {
                let s = lock(shared);
                let task = s
                    .task_ids
                    .get(name)
                    .copied()
                    .ok_or_else(|| format!("unknown task {name:?}"))?;
                let region = s
                    .task_regions
                    .get(name)
                    .ok_or_else(|| format!("task {name:?} has no region"))?;
                let owner = s
                    .region_ids
                    .get(region)
                    .copied()
                    .ok_or_else(|| format!("unknown region {region:?}"))?;
                (task, owner)
            };
            // A driver cancel originates at the task's own region.
            let reason = cancel_reason(op, owner, None, lab.now())?;
            let priority = reason.cleanup_budget().priority;
            let (newly, wakes) = lab.state.cancel_task(task, &reason).into_parts();
            if newly {
                lab.scheduler.lock().schedule_cancel(task, priority);
            }
            wakes.dispatch();
        }
        "advance" => lab.advance_time(u64_field(op, "ns")?),
        other => {
            return Err(format!(
                "driver op {other:?} is not interpreted yet (increment 1)"
            ));
        }
    }
    Ok(())
}

// ---------------------------------------------------------------------------
// Trace projection and snapshot
// ---------------------------------------------------------------------------

/// Name the canonical ids of children spawned through `cx.spawn`. The spawn
/// returns a handle carrying a provisional mailbox id; admission assigns the
/// canonical arena id every later trace event uses. The trace pairs the two:
/// `TaskSpawnEnqueued` (provisional) and `TaskAdmitted` (canonical) match in
/// per-region FIFO order (src/trace/event.rs, `task_admitted`). Idempotent:
/// it rescans the trace and moves each newly admitted child out of
/// `provisional`.
fn resolve_admissions(lab: &LabRuntime, shared: &SharedRef) -> RunResult<()> {
    use std::collections::VecDeque;
    let mut pending: HashMap<RegionId, VecDeque<TaskId>> = HashMap::new();
    let mut s = lock(shared);
    for event in lab.trace().snapshot() {
        match (&event.kind, &event.data) {
            (TraceEventKind::TaskSpawnEnqueued, TraceData::Task { task, region }) => {
                pending.entry(*region).or_default().push_back(*task);
            }
            (TraceEventKind::TaskAdmitted, TraceData::Task { task, region }) => {
                let provisional = pending
                    .get_mut(region)
                    .and_then(VecDeque::pop_front)
                    .ok_or_else(|| {
                        format!("TaskAdmitted {task:?} has no pending enqueue in {region:?}")
                    })?;
                if let Some(name) = s.provisional.remove(&provisional) {
                    if let Some(other) = s.task_names.get(task) {
                        return Err(format!(
                            "canonical {task:?} of {name:?} is already named {other:?}"
                        ));
                    }
                    s.task_ids.insert(name.clone(), *task);
                    s.task_names.insert(*task, name);
                }
            }
            _ => {}
        }
    }
    Ok(())
}

struct Projected {
    events: Vec<Value>,
    /// obligation name -> (kind, holder, region, last state, abort reason)
    obligations: Vec<(String, Value)>,
    timers_pending: Vec<Value>,
}

fn project_trace(lab: &LabRuntime, shared: &SharedRef) -> RunResult<Projected> {
    use TraceEventKind as K;
    let s = lock(shared);
    let mut events = Vec::new();
    let mut obligation_names: HashMap<asupersync::ObligationId, String> = HashMap::new();
    let mut per_holder: HashMap<String, u32> = HashMap::new();
    let mut obligations: Vec<(String, Value)> = Vec::new();
    let mut timers: HashMap<String, (u64, bool)> = HashMap::new();
    let mut timer_order: Vec<String> = Vec::new();
    for event in lab.trace().snapshot() {
        let ev = match (&event.kind, &event.data) {
            (K::Spawn, TraceData::Task { task, region }) => {
                json!({"k": "task.spawned", "task": task_name(&s, *task)?, "region": region_name(&s, *region)?})
            }
            (K::Complete, TraceData::Task { task, region }) => {
                json!({"k": "task.completed", "task": task_name(&s, *task)?, "region": region_name(&s, *region)?})
            }
            (K::RegionCreated, TraceData::Region { region, parent }) => json!({
                "k": "region.created", "region": region_name(&s, *region)?,
                "parent": match parent { Some(p) => Value::String(region_name(&s, *p)?), None => Value::Null },
            }),
            (K::RegionCloseBegin, TraceData::Region { region, .. }) => {
                json!({"k": "region.close_begin", "region": region_name(&s, *region)?})
            }
            (K::RegionCloseComplete, TraceData::Region { region, .. }) => {
                json!({"k": "region.closed", "region": region_name(&s, *region)?})
            }
            (K::RegionCancelled, TraceData::RegionCancel { region, reason }) => {
                json!({"k": "region.cancelled", "region": region_name(&s, *region)?, "reason": project_reason(&s, reason)?})
            }
            (
                K::CancelRequest,
                TraceData::Cancel {
                    task,
                    region,
                    reason,
                },
            ) => json!({
                "k": "cancel.requested", "task": task_name(&s, *task)?, "region": region_name(&s, *region)?,
                "reason": project_reason(&s, reason)?,
            }),
            (
                K::ObligationReserve | K::ObligationCommit | K::ObligationAbort | K::ObligationLeak,
                TraceData::Obligation {
                    obligation,
                    task,
                    region,
                    kind,
                    abort_reason,
                    ..
                },
            ) => {
                let holder = task_name(&s, *task)?;
                let name = match obligation_names.get(obligation) {
                    Some(n) => n.clone(),
                    None => {
                        let k = per_holder.entry(holder.clone()).or_insert(0);
                        *k += 1;
                        let n = format!("{holder}/o{k}");
                        obligation_names.insert(*obligation, n.clone());
                        n
                    }
                };
                let (k, state) = match event.kind {
                    K::ObligationReserve => ("obligation.reserved", "Reserved"),
                    K::ObligationCommit => ("obligation.committed", "Committed"),
                    K::ObligationAbort => ("obligation.aborted", "Aborted"),
                    _ => ("obligation.leaked", "Leaked"),
                };
                let mut ev = json!({
                    "k": k, "obligation": name, "task": holder, "region": region_name(&s, *region)?,
                    "kind": format!("{kind:?}"),
                });
                let abort = abort_reason.map(|r| format!("{r:?}"));
                if k == "obligation.aborted" {
                    ev["abort_reason"] = json!(abort);
                }
                let record = json!({
                    "state": state, "kind": format!("{kind:?}"), "holder": ev["task"], "region": ev["region"],
                    "abort_reason": abort,
                });
                match obligations.iter_mut().find(|(n, _)| *n == name) {
                    Some((_, v)) => *v = record,
                    None => obligations.push((name, record)),
                }
                ev
            }
            (
                K::TimerScheduled | K::TimerFired | K::TimerCancelled,
                TraceData::Timer { timer_id, deadline },
            ) => {
                let name = s
                    .timer_names
                    .get(timer_id)
                    .cloned()
                    .ok_or_else(|| format!("unnamed timer {timer_id}"))?;
                match event.kind {
                    K::TimerScheduled => {
                        let d = deadline.map_or(0, |t| t.as_nanos());
                        if !timers.contains_key(&name) {
                            timer_order.push(name.clone());
                        }
                        timers.insert(name.clone(), (d, true));
                        json!({"k": "timer.scheduled", "timer": name, "deadline_ns": d})
                    }
                    K::TimerFired => {
                        if let Some(t) = timers.get_mut(&name) {
                            t.1 = false;
                        }
                        json!({"k": "timer.fired", "timer": name})
                    }
                    _ => {
                        if let Some(t) = timers.get_mut(&name) {
                            t.1 = false;
                        }
                        json!({"k": "timer.cancelled", "timer": name})
                    }
                }
            }
            (K::UserTrace, TraceData::Message(message)) => {
                if message.starts_with("obligation_handoff_v") {
                    return Err("obligation handoff projection is not implemented yet".to_string());
                }
                json!({"k": "user.trace", "message": message})
            }
            // docs/VOCABULARY_EXCLUSIONS.md
            (
                K::Schedule
                | K::Yield
                | K::Wake
                | K::Poll
                | K::CancelAck
                | K::WorkerCancelRequested
                | K::WorkerCancelAcknowledged
                | K::WorkerDrainStarted
                | K::WorkerDrainCompleted
                | K::WorkerFinalizeCompleted
                | K::TaskSpawnEnqueued
                | K::TaskAdmitted
                | K::TimeAdvance
                | K::IoRequested
                | K::IoReady
                | K::IoResult
                | K::IoError
                | K::RngSeed
                | K::RngValue
                | K::Checkpoint
                | K::MonitorCreated
                | K::MonitorDropped
                | K::DownDelivered
                | K::LinkCreated
                | K::LinkDropped
                | K::ExitDelivered
                | K::BudgetInstalled
                | K::BudgetConsumed,
                _,
            ) => continue,
            (K::FuturelockDetected | K::ChaosInjection, _) => {
                return Err(format!(
                    "{:?} in a conformance run (harness failure)",
                    event.kind
                ));
            }
            (kind, data) => {
                return Err(format!("unprojectable trace event {kind:?} with {data:?}"));
            }
        };
        events.push(ev);
    }
    let mut timers_pending: Vec<Value> = timer_order
        .iter()
        .filter_map(|n| {
            timers
                .get(n)
                .filter(|t| t.1)
                .map(|t| json!({"timer": n, "deadline_ns": t.0}))
        })
        .collect();
    timers_pending.sort_by(|a, b| {
        (a["deadline_ns"].as_u64(), a["timer"].as_str())
            .cmp(&(b["deadline_ns"].as_u64(), b["timer"].as_str()))
    });
    Ok(Projected {
        events,
        obligations,
        timers_pending,
    })
}

fn task_state(state: &TaskState) -> (&'static str, Option<&CancelReason>, Option<&Budget>) {
    match state {
        TaskState::Created => ("Created", None, None),
        TaskState::Running => ("Running", None, None),
        TaskState::CancelRequested {
            reason,
            cleanup_budget,
        } => ("CancelRequested", Some(reason), Some(cleanup_budget)),
        TaskState::Cancelling {
            reason,
            cleanup_budget,
        } => ("Cancelling", Some(reason), Some(cleanup_budget)),
        TaskState::Finalizing {
            reason,
            cleanup_budget,
        } => ("Finalizing", Some(reason), Some(cleanup_budget)),
        TaskState::Completed(_) => ("Completed", None, None),
    }
}

fn build_snapshot(lab: &LabRuntime, shared: &SharedRef, projected: &Projected) -> RunResult<Value> {
    let s = lock(shared);
    let mut tasks = serde_json::Map::new();
    for (name, id) in &s.task_ids {
        let (state, reason, cleanup) = match lab.state.task(*id) {
            Some(record) => task_state(&record.state),
            None => ("Completed", None, None),
        };
        let outcome = if state == "Completed" {
            // Every completed task's outcome is harvested (by a join step or
            // the driver's final try_join); a gap is a harness bug.
            s.outcomes
                .get(name)
                .cloned()
                .ok_or_else(|| format!("completed task {name:?} has no harvested outcome"))?
        } else {
            Value::Null
        };
        let cancel_reason = match reason {
            Some(r) => project_reason(&s, r)?,
            None => outcome.get("reason").cloned().unwrap_or(Value::Null),
        };
        tasks.insert(name.clone(), json!({
            "state": state,
            "outcome": outcome,
            "cancel_reason": cancel_reason,
            "cleanup_budget": cleanup.map(|b| json!({"poll_quota": b.poll_quota, "priority": b.priority})),
        }));
    }
    let mut regions = serde_json::Map::new();
    for (name, id) in &s.region_ids {
        let (state, reason) = match lab.state.region(*id) {
            Some(record) => (format!("{:?}", record.state()), record.cancel_reason()),
            None => ("Closed".to_string(), None),
        };
        regions.insert(name.clone(), json!({
            "state": state,
            "parent": s.region_parents.get(name).cloned().flatten(),
            "cancel_reason": match reason { Some(r) => project_reason(&s, &r)?, None => Value::Null },
        }));
    }
    let obligations: serde_json::Map<String, Value> =
        projected.obligations.iter().cloned().collect();
    Ok(json!({
        "now_ns": lab.now().as_nanos(),
        "quiescent": lab.is_quiescent(),
        "tasks": tasks,
        "regions": regions,
        "obligations": obligations,
        "timers_pending": projected.timers_pending,
        "channels": {},
    }))
}

fn check_expectation(scenario: &Value, observations: &[Value]) -> RunResult<()> {
    let expect = scenario.get("expect").ok_or("missing expect")?;
    if str_field(expect, "kind")? != "must_fail" {
        return Ok(());
    }
    let error = expect.get("error").ok_or("must_fail without error")?;
    let task = str_field(error, "task")?;
    let step = u64_field(error, "step")?;
    let status = str_field(error, "status")?;
    let observed = observations
        .iter()
        .find(|o| o["task"] == task && o["step"] == step)
        .ok_or_else(|| format!("must_fail: no observation for {task} step {step}"))?;
    if observed["status"] != status {
        return Err(format!(
            "must_fail: {task} step {step} returned {} (expected {status})",
            observed["status"]
        ));
    }
    Ok(())
}
