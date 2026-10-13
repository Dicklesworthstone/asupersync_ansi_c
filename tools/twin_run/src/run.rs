//! Runs one asx.scenario.v2 scenario inside asupersync's LabRuntime and
//! projects it into asx.vocab.v2 (docs/SCENARIO_DSL_V2.md,
//! docs/CANONICAL_VOCABULARY_V2.md).
//!
//! It interprets the driver (setup, script, finish), the control, masking,
//! obligation, spawn/join and child-region steps, the sync (mutex,
//! semaphore, rwlock, barrier, notify), channel (mpsc, oneshot, broadcast,
//! watch), group and combinator steps and the server and supervisor steps
//! (DSL §3.8), and projects the trace and the snapshot. A step it does not
//! interpret fails the run as a harness error, never silently.

use std::collections::{BTreeMap, HashMap, HashSet};
use std::future::Future;
use std::pin::Pin;
use std::sync::{Arc, Mutex, MutexGuard, Weak};
use std::time::Duration;

use asupersync::channel::{broadcast, mpsc, oneshot, watch};
use asupersync::combinator::first_ok::FirstOkError;
use asupersync::combinator::quorum::QuorumError;
use asupersync::cx::{ChildRegion, ChildRegionSpec};
use asupersync::gen_server::{
    CallError, CastError, CastOverflowPolicy, GenServer, GenServerHandle, Reply, SystemMsg,
};
use asupersync::lab::runtime::{ForcedSchedule, ForcedScheduleDecodeLimits, ForcedScheduleLimits};
use asupersync::lab::{LabConfig, LabRuntime};
use asupersync::link::{Link, LinkRef};
use asupersync::monitor::{DownReason, Monitor, MonitorRef, WatchError, WatchTarget};
use asupersync::record::task::TaskState;
use asupersync::record::{ObligationAbortReason, ObligationKind, RegionLimits};
use asupersync::runtime::obligation_mailbox::ObligationToken;
use asupersync::runtime::{JoinError, TaskHandle};
use asupersync::supervision::{
    ChildSpec, EscalationPolicy, ManagedChildBinding, ManagedGeneration, ManagedRestartMode,
    ManagedSupervisorError, ManagedSupervisorReport, NameCollisionPolicy, NameRegistrationPolicy,
    RestartPolicy, SupervisionConfig, SupervisorBuilder,
};
use asupersync::sync as asx_sync;
use asupersync::time::delay_queue::Key as DelayKey;
use asupersync::time::{DelayQueue, Interval, MissedTickBehavior, interval, interval_at};
use asupersync::trace::{CompactTaskId, TraceData, TraceEvent, TraceEventKind};
use asupersync::{
    Budget, CancelKind, CancelReason, CapabilityBudget, CapabilityBudgetRefusal,
    CapabilityBudgetRequirements, Cx, RegionId, TaskId, Time,
};
use serde_json::{Value, json};

use crate::canon;

pub type RunResult<T> = Result<T, String>;

/// What a task body returns. Cancellation and panics are not values: the
/// runtime reports them through the task handle (`JoinError`). The one
/// exception is a supervised child's body, whose factory future returns an
/// `Outcome` and so can return `Outcome::Cancelled(reason)` itself (DSL
/// §3.8, `return cancelled`).
#[derive(Debug)]
pub enum Body {
    Ok,
    Err(String),
    Cancelled(CancelReason),
}

/// State shared between the driver and every interpreted task body.
#[derive(Default)]
struct Shared {
    /// Canonical (admitted) task ids; see `resolve_admissions`.
    task_ids: HashMap<String, TaskId>,
    task_names: HashMap<TaskId, String>,
    /// Real task-record contexts, including declared tasks not yet polled,
    /// of the tasks a `handoff` names as its destination only.
    task_contexts: HashMap<String, Cx>,
    /// Every task a `handoff` step names as its destination
    /// (`handoff_destinations`).
    handoff_destinations: HashSet<String>,
    /// The mpsc channels declared `"unbounded": true` (DSL §3.6): Rust's
    /// `UnboundedSender` has no `send_evict_oldest` and no `closed`.
    unbounded_channels: HashSet<String>,
    /// The scenario declares `lab.futurelock_max_idle_steps` (DSL §1): the
    /// run has `panic_on_futurelock` off and `FuturelockDetected` is
    /// projected as `futurelock.detected` instead of failing the harness.
    futurelock_declared: bool,
    /// Weak references only: the destination future owns incoming tokens,
    /// including before its first poll, and drops them with its body.
    obligation_tokens: HashMap<String, Weak<Mutex<HashMap<String, ObligationToken>>>>,
    /// Accepted ticket links from real try_transfer calls, checked against
    /// the ownership envelope instead of trusting user annotations.
    handoff_tickets: BTreeMap<(u64, u64), (TaskId, TaskId, RegionId, RegionId)>,
    /// Children spawned through `cx.spawn` by provisional mailbox id, until
    /// their admission is resolved.
    provisional: HashMap<TaskId, String>,
    /// Canonical ids that task bodies recorded under their own names when
    /// they first ran (`run_program`), and the provisional ids already
    /// paired with a canonical one (`resolve_admissions`).
    self_ids: HashMap<String, TaskId>,
    paired: HashSet<TaskId>,
    /// Join results of children an `abort_task` found already refused by
    /// their admission, taken before the abort strengthened their reason
    /// (which would hide why they were refused, `project_denied_spawns`).
    denied: HashMap<String, Result<Body, JoinError>>,
    /// Child name -> index of the observation of the spawn step that
    /// created it (`project_denied_spawns`).
    spawn_observations: HashMap<String, usize>,
    /// Children whose spawn named a region its task did not hold: nothing
    /// was spawned, and steps on them observe `NOT_HELD` (DSL §3).
    unspawned: HashSet<String>,
    /// Task name -> name of the region it runs in.
    task_regions: HashMap<String, String>,
    /// Names of spawned servers (DSL §3.8). A server's snapshot outcome is
    /// its join's (vocabulary §6): taken by `server_stop`, else harvested
    /// from its task record (`harvest_servers`). A server records its own
    /// canonical id in `self_ids` from its hooks (`ServerName`).
    servers: HashSet<String>,
    /// Declared sync objects by name.
    sync: HashMap<String, SyncObj>,
    region_ids: HashMap<String, RegionId>,
    region_names: HashMap<RegionId, String>,
    region_parents: HashMap<String, Option<String>>,
    /// Timer names by the trace seq of each Timer event their sleep recorded
    /// (`NamedSleep`).
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
    /// Members of task groups (DSL §3.5). Rust's combinators consume their
    /// handles and discard losers' results, so their outcomes are reported
    /// only through the group step's observation, and the snapshot leaves
    /// them out (vocabulary §6).
    group_members: HashSet<String>,
    /// (observation index, raw group result), projected with the join
    /// results once every task name is resolved.
    group_values: Vec<(usize, GroupValue)>,
    /// Supervisors (DSL §3.8) in `supervise` order. A supervisor's snapshot
    /// outcome is its join's (vocabulary §6): taken by `supervisor_join`,
    /// else harvested (`harvest_supervisors`).
    supervisors: Vec<SupervisorInfo>,
    /// Controller handles of supervisors whose owner dropped them unjoined
    /// (`Supervisor`'s Drop), kept for the harvest. A TaskHandle has no
    /// Drop, so holding it changes nothing in the run.
    dropped_supervisors: HashMap<String, TaskHandle<()>>,
    /// Generation tasks (`<supervisor>/<child>#<n>`). Like group members,
    /// their results reach the comparison only through the supervisor's
    /// trace and report, and the snapshot leaves them out (vocabulary §6).
    supervised: HashSet<String>,
    /// (observation index, report) of `supervisor_join` steps that got one,
    /// projected once every name is resolved.
    supervisor_values: Vec<(usize, ManagedSupervisorReport<String>)>,
}

/// The report slot a controller publishes into (`ManagedSupervisor::spawn`).
type ReportSlot = Arc<Mutex<Option<ManagedSupervisorReport<String>>>>;

struct SupervisorInfo {
    name: String,
    /// The region its controller runs in: its supervisor region's parent.
    region: String,
    report: ReportSlot,
    /// Its controller's provisional (mailbox) id; once admitted, its handle
    /// reports the canonical one (TaskHandle::task_id).
    provisional: TaskId,
}

/// What a group step returned, before projection (DSL §3.5).
enum GroupValue {
    /// `race_all`: the winner's index (Rust reports it only on success) and
    /// result.
    Race(Option<usize>, Result<Body, JoinError>),
    /// `join_all`: every member's result, in member order.
    JoinAll(Vec<Result<Body, JoinError>>),
}

type SharedRef = Arc<Mutex<Shared>>;

fn lock<T>(shared: &Arc<Mutex<T>>) -> MutexGuard<'_, T> {
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

/// The step's `behavior` (DSL §3.9): burst when absent.
fn missed_tick_behavior(step: &Value) -> RunResult<MissedTickBehavior> {
    match step.get("behavior").and_then(Value::as_str) {
        None | Some("burst") => Ok(MissedTickBehavior::Burst),
        Some("delay") => Ok(MissedTickBehavior::Delay),
        Some("skip") => Ok(MissedTickBehavior::Skip),
        Some(other) => Err(format!("missed-tick behavior {other:?}")),
    }
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

/// DSL §1 capability budget: `memory_bytes`, `cpu_units`, `io_bytes`,
/// `artifact_bytes` and `cleanup_poll_quota` (the cleanup budget: a
/// `Budget::new()` with that poll quota), each optional. Absent or null is
/// `CapabilityBudget::UNSPECIFIED`.
fn capability_budget(v: Option<&Value>) -> RunResult<CapabilityBudget> {
    let mut b = CapabilityBudget::new();
    let Some(v) = v.filter(|v| !v.is_null()) else {
        return Ok(b);
    };
    if let Some(x) = v.get("memory_bytes").and_then(Value::as_u64) {
        b = b.with_memory_bytes(x);
    }
    if let Some(x) = v.get("cpu_units").and_then(Value::as_u64) {
        b = b.with_cpu_units(x);
    }
    if let Some(x) = v.get("io_bytes").and_then(Value::as_u64) {
        b = b.with_io_bytes(x);
    }
    if let Some(x) = v.get("artifact_bytes").and_then(Value::as_u64) {
        b = b.with_artifact_bytes(x);
    }
    if let Some(p) = v.get("cleanup_poll_quota").and_then(Value::as_u64) {
        let polls =
            u32::try_from(p).map_err(|_| format!("cleanup_poll_quota out of range: {p}"))?;
        b = b.with_cleanup_budget(Budget::new().with_poll_quota(polls));
    }
    Ok(b)
}

/// DSL §1 requirements: an array of dimension names (`as_str`).
fn capability_requirements(v: Option<&Value>) -> RunResult<CapabilityBudgetRequirements> {
    let mut r = CapabilityBudgetRequirements::NONE;
    let Some(list) = v.and_then(Value::as_array) else {
        return Ok(r);
    };
    for d in list {
        r = match d.as_str() {
            Some("memory_bytes") => r.require_memory_bytes(),
            Some("cpu_units") => r.require_cpu_units(),
            Some("io_bytes") => r.require_io_bytes(),
            Some("cleanup") => r.require_cleanup(),
            Some("artifact_bytes") => r.require_artifact_bytes(),
            other => return Err(format!("unknown capability dimension {other:?}")),
        };
    }
    Ok(r)
}

/// An envelope as an observation value: the five keys, null where the
/// dimension has none.
fn capability_budget_json(b: &CapabilityBudget) -> Value {
    json!({
        "memory_bytes": b.memory_bytes,
        "cpu_units": b.cpu_units,
        "io_bytes": b.io_bytes,
        "cleanup_poll_quota": b.cleanup_budget.map(|c| c.poll_quota),
        "artifact_bytes": b.artifact_bytes,
    })
}

/// A refusal as an observation value.
fn capability_refusal_json(r: &CapabilityBudgetRefusal) -> Value {
    let (kind, dimension) = match r {
        CapabilityBudgetRefusal::MissingRequired(d) => ("missing_required", d),
        CapabilityBudgetRefusal::Exhausted(d) => ("exhausted", d),
    };
    json!({"kind": kind, "dimension": dimension.as_str()})
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

/// The prefix a refused spawn's message is projected to (vocabulary §4):
/// Rust's full text (SpawnError::RegionAtCapacity, runtime/state.rs:1726)
/// embeds a RegionId, limit and live count the C runtime cannot spell.
const ADMISSION_LIMIT_MESSAGE: &str = "[ASUP-E006] region admission limit reached";

fn project_reason(shared: &Shared, reason: &CancelReason) -> RunResult<Value> {
    let mut chain = 0u64;
    let mut cause = reason.cause.as_deref();
    while let Some(c) = cause {
        chain += 1;
        cause = c.cause.as_deref();
    }
    // A spawn refused at a region's live-task limit resolves its handle with
    // CancelReason::user carrying the error's text (cx/cx.rs:5526-5534):
    // projected with the message's fixed prefix.
    let message = match reason.message.as_deref() {
        Some(m) if reason.kind == CancelKind::User && m.starts_with(ADMISSION_LIMIT_MESSAGE) => {
            Some(ADMISSION_LIMIT_MESSAGE.to_string())
        }
        other => other.map(str::to_string),
    };
    Ok(json!({
        "kind": kind_name(reason.kind),
        "origin_region": region_name(shared, reason.origin_region)?,
        "origin_task": match reason.origin_task {
            Some(t) => Value::String(task_name(shared, t)?),
            None => Value::Null,
        },
        "timestamp_ns": reason.timestamp.as_nanos(),
        "message": message,
        "cause_chain_len": chain,
        "truncated": reason.truncated,
    }))
}

fn project_outcome(shared: &Shared, result: Result<Body, JoinError>) -> RunResult<Value> {
    Ok(match result {
        Ok(Body::Ok) => json!({"tag": "ok"}),
        Ok(Body::Err(status)) => json!({"tag": "err", "status": status}),
        Ok(Body::Cancelled(reason)) | Err(JoinError::Cancelled(reason)) => {
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
    for (index, group) in std::mem::take(&mut s.group_values) {
        let value = match group {
            GroupValue::Race(winner, result) => json!({
                "winner_index": winner,
                "outcome": project_outcome(s, result)?,
            }),
            GroupValue::JoinAll(results) => Value::Array(
                results
                    .into_iter()
                    .map(|r| project_outcome(s, r))
                    .collect::<RunResult<Vec<_>>>()?,
            ),
        };
        s.observations[index]["value"] = value;
    }
    for (index, report) in std::mem::take(&mut s.supervisor_values) {
        let value = project_report(s, report)?;
        s.observations[index]["value"] = value;
    }
    Ok(())
}

/// Every task a `handoff` step anywhere in the scenario names as its
/// destination.
fn handoff_destinations(scenario: &Value) -> HashSet<String> {
    let mut out = HashSet::new();
    let mut stack = vec![scenario];
    while let Some(node) = stack.pop() {
        match node {
            Value::Object(map) => {
                if map.get("op").and_then(Value::as_str) == Some("handoff")
                    && let Some(task) = map.get("task").and_then(Value::as_str)
                {
                    out.insert(task.to_string());
                }
                stack.extend(map.values());
            }
            Value::Array(items) => stack.extend(items.iter()),
            _ => {}
        }
    }
    out
}

fn harness_error(shared: &SharedRef, msg: String) {
    lock(shared).errors.push(msg);
}

// ---------------------------------------------------------------------------
// The step interpreter
// ---------------------------------------------------------------------------

/// Per-task interpreter state that never leaves the task.
///
/// It is dropped when the task body ends (returns, is cancelled or panics),
/// fields in declaration order, and what it still holds is released then,
/// as a Rust body releases its locals: guards and permits are ordered maps
/// so that release order (by name, a name's permits oldest first) is
/// deterministic and the C interpreter can mirror it.
#[derive(Default)]
struct Local {
    _tokens: OwnedTokens,
    /// Child regions still open; an ordered map so the drop backstop's
    /// Close commands (ChildRegion's Drop) queue by name.
    regions: BTreeMap<String, ChildRegion>,
    timers: u32,
    /// Held mutex guards and semaphore permits, by object name (DSL §3.7).
    /// Guards are owned: a borrowed `MutexGuard` is not `Send` (sync/
    /// mutex.rs:708) and a task body is; the owned lock acquires through the
    /// same path (`OwnedMutexGuard::lock`, :913).
    guards: BTreeMap<String, asx_sync::OwnedMutexGuard<()>>,
    permits: BTreeMap<String, Vec<asx_sync::SemaphorePermit<'static>>>,
    /// Held rwlock guards by lock name, oldest first (owned, as for
    /// mutexes: `OwnedRwLockReadGuard::read`, sync/rwlock.rs:1155).
    rw_guards: BTreeMap<String, Vec<RwGuard>>,
    /// Held mpsc send permits by permit name, with their channel (DSL
    /// §3.6). Declared before `cx` and `mpsc_tx` so they drop first: each
    /// borrows its channel's sender and the Cx it was reserved with
    /// (`Reserve<'a>` ties both to the permit, `channel/mpsc.rs:951`).
    send_permits: BTreeMap<String, (String, mpsc::SendPermit<'static, u64>)>,
    /// Senders a `close_sender` closed while a permit on their channel was
    /// held. A Rust permit borrows its sender, so the body can drop the
    /// sender only after the permit is gone: the drop is projected there,
    /// when the last such permit is sent or aborted, or at the body's end
    /// (DSL §3.6). Declared right after `send_permits` so they drop next.
    deferred_tx: BTreeMap<String, Box<mpsc::Sender<u64>>>,
    /// A clone of the task's Cx for permits to borrow (`lend`).
    cx: Option<Box<Cx>>,
    /// The channel endpoints this task owns, by channel name (DSL §3.6).
    /// They drop in this order (each map by name) when the body ends; the C
    /// interpreter closes them in the same order.
    mpsc_tx: BTreeMap<String, Box<mpsc::Sender<u64>>>,
    mpsc_rx: BTreeMap<String, mpsc::Receiver<u64>>,
    /// Receivers this task sealed (`rx.close()`); a second seal is outside
    /// the DSL (§3.6).
    sealed: HashSet<String>,
    oneshot_tx: BTreeMap<String, oneshot::Sender<u64>>,
    oneshot_rx: BTreeMap<String, oneshot::Receiver<u64>>,
    broadcast_tx: BTreeMap<String, broadcast::Sender<u64>>,
    broadcast_rx: BTreeMap<String, broadcast::Receiver<u64>>,
    watch_tx: BTreeMap<String, watch::Sender<u64>>,
    watch_rx: BTreeMap<String, watch::Receiver<u64>>,
    /// Clones of the task's watch receivers (`watch_clone`), by channel and
    /// then by their `as` name (DSL §3.6). A clone drops with the task's
    /// other endpoints; the order between receivers is not observable.
    watch_clones: BTreeMap<String, BTreeMap<String, watch::Receiver<u64>>>,
    /// Intervals and DelayQueues this task made, by name, and the keys the
    /// queues' inserts returned, by (queue, key name) (DSL §3.9). A key's name
    /// stays bound after its entry expired or was removed.
    intervals: BTreeMap<String, Interval>,
    delay_queues: BTreeMap<String, DelayQueue<u64>>,
    /// The queue whose own Sleep stays registered after a wait, until an
    /// empty poll or a `clear` drops it (delay_queue.rs:251, :263). The C
    /// side holds one timer per task, so no other timer step and no second
    /// queue's wait is defined meanwhile (DSL §3.9).
    live_queue: Option<String>,
    delay_keys: BTreeMap<(String, String), DelayKey>,
    /// Monitors this task holds by name, with the monitored task's name
    /// (DSL §3.10); dropping one removes it (Monitor's Drop). The references
    /// of monitors a server holds for this task's `server_monitor` steps,
    /// by name.
    monitors: BTreeMap<String, (Monitor, String)>,
    server_monitors: BTreeMap<String, MonitorRef>,
    /// Links this task holds by name, with the peer's name and whether the
    /// link traps, and the references of the links a server holds for it
    /// (DSL §3.11). Dropping a `Link` does not remove it: only `unlink` does.
    links: BTreeMap<String, (Link, String, bool)>,
    server_links: BTreeMap<String, LinkRef>,
    /// GenServers this task spawned, by name (DSL §3.8).
    servers: BTreeMap<String, Server>,
    /// Supervisors this task spawned, by name, joined or not (DSL §3.8):
    /// each aborts its controller when dropped, in name order.
    supervisors: BTreeMap<String, Supervisor>,
}

#[derive(Default)]
struct OwnedTokens {
    tokens: Arc<Mutex<HashMap<String, ObligationToken>>>,
}

impl Drop for OwnedTokens {
    fn drop(&mut self) {
        // Token drop notifies the real runtime; never hold a scenario
        // mutex while running those destructors.
        let tokens = std::mem::take(&mut *lock(&self.tokens));
        drop(tokens);
    }
}

/// `ManagedSupervisorHandle` (supervision.rs:3402-3450), built the way
/// `ManagedSupervisor::spawn` builds it (:3497-3505), so the harness can see
/// whether the controller published its report.
struct Supervisor {
    name: String,
    task: Option<TaskHandle<()>>,
    report: ReportSlot,
    joined: bool,
    shared: SharedRef,
}

impl Drop for Supervisor {
    fn drop(&mut self) {
        if let Some(task) = self.task.take() {
            task.abort();
            if !self.joined {
                lock(&self.shared)
                    .dropped_supervisors
                    .insert(self.name.clone(), task);
            }
        }
    }
}

fn lock_report(slot: &ReportSlot) -> MutexGuard<'_, Option<ManagedSupervisorReport<String>>> {
    slot.lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
}

/// A supervised child's managed factory (DSL §3.8): each generation runs
/// the child's program in its own task and region, both named
/// `<supervisor>/<child>#<n>`, recorded when the factory runs inside the
/// generation's task (`name_supervision` names a generation that never
/// started from the supervisor's trace). Its program's return is the
/// generation's outcome; a `return panicked` panics inside the factory's
/// future, which the controller catches.
fn generation_binding(
    shared: &SharedRef,
    supervisor: &str,
    child: String,
    mode: ManagedRestartMode,
    program: Vec<Value>,
) -> ManagedChildBinding<String> {
    let shared = Arc::clone(shared);
    let prefix = format!("{supervisor}/{child}");
    ManagedChildBinding::new(
        child,
        mode,
        move |ccx: Cx, generation: ManagedGeneration| {
            let name = format!("{prefix}#{}", generation.number);
            {
                let mut s = lock(&shared);
                let id = ccx.task_id();
                s.self_ids.entry(name.clone()).or_insert(id);
                s.task_ids.insert(name.clone(), id);
                s.task_names.insert(id, name.clone());
                s.task_regions.insert(name.clone(), name.clone());
                s.supervised.insert(name.clone());
                s.region_names
                    .entry(generation.region)
                    .or_insert_with(|| name.clone());
                s.region_ids.insert(name.clone(), generation.region);
            }
            let ctx = Arc::new(TaskCtx {
                shared: Arc::clone(&shared),
                me: name.clone(),
                region: name,
                tokens: Arc::default(),
            });
            // The names the program creates are this generation's own
            // (DSL §3.8): scoped under its name before it runs.
            let program = scope_program(&program, &ctx.me);
            async move {
                match run_program(ccx, ctx, program, Vec::new()).await {
                    Body::Ok => asupersync::Outcome::Ok(()),
                    Body::Err(status) => asupersync::Outcome::Err(status),
                    Body::Cancelled(reason) => asupersync::Outcome::Cancelled(reason),
                }
            }
        },
    )
}

/// Whether a step of this `op` names a task, region, server or supervisor
/// with `as`.
fn creates_name(op: &str) -> bool {
    matches!(op, "spawn" | "open_region" | "server_spawn" | "supervise")
}

/// The names the steps of `program` create (their `as`), into `out`: those
/// of member programs, masked steps and spawned children's programs
/// included, but not those of a nested supervisor's children, which run
/// generations of their own and are scoped by them.
fn collect_created(program: &[Value], out: &mut HashSet<String>) {
    for step in program {
        let op = step.get("op").and_then(Value::as_str).unwrap_or("");
        if creates_name(op)
            && let Some(name) = step.get("as").and_then(Value::as_str)
        {
            out.insert(name.to_string());
        }
        for members in step
            .get("members")
            .and_then(Value::as_array)
            .into_iter()
            .flatten()
        {
            if let Some(member) = members.as_array() {
                collect_created(member, out);
            }
        }
        if let Some(steps) = step.get("steps").and_then(Value::as_array) {
            collect_created(steps, out);
        }
        if op == "spawn"
            && let Some(child) = step.get("program").and_then(Value::as_array)
        {
            collect_created(child, out);
        }
    }
}

/// One step with its names scoped (see [`scope_program`]).
fn scope_step(step: &Value, scope: &str, created: &HashSet<String>) -> Value {
    let Some(object) = step.as_object() else {
        return step.clone();
    };
    let op = step.get("op").and_then(Value::as_str).unwrap_or("");
    let mut out = serde_json::Map::new();
    for (key, value) in object {
        let scoped = match (key.as_str(), value) {
            ("as", Value::String(name)) if creates_name(op) => json!(format!("{scope}/{name}")),
            ("task" | "region" | "server" | "supervisor", Value::String(name))
                if created.contains(name) =>
            {
                json!(format!("{scope}/{name}"))
            }
            ("members", Value::Array(members)) => Value::Array(
                members
                    .iter()
                    .map(|member| match member.as_array() {
                        Some(program) => Value::Array(scope_steps(program, scope, created)),
                        None => member.clone(),
                    })
                    .collect(),
            ),
            ("steps", Value::Array(steps)) => Value::Array(scope_steps(steps, scope, created)),
            ("program", Value::Array(program)) if op == "spawn" => {
                Value::Array(scope_steps(program, scope, created))
            }
            _ => value.clone(),
        };
        out.insert(key.clone(), scoped);
    }
    Value::Object(out)
}

fn scope_steps(program: &[Value], scope: &str, created: &HashSet<String>) -> Vec<Value> {
    program
        .iter()
        .map(|step| scope_step(step, scope, created))
        .collect()
}

/// A supervised generation's program with the names it creates scoped under
/// the generation's name (DSL §3.8): every `as` of a `spawn`, `open_region`,
/// `server_spawn` or `supervise` becomes `<scope>/<as>`, and so does every
/// `task`, `region`, `server` or `supervisor` reference to such a name in
/// the same program. A name the program does not create (a scenario-level
/// region or task) is left alone. The C interpreter's `scope_steps` does the
/// same to the same JSON.
fn scope_program(program: &[Value], scope: &str) -> Vec<Value> {
    let mut created = HashSet::new();
    collect_created(program, &mut created);
    if created.is_empty() {
        return program.to_vec();
    }
    scope_steps(program, scope, &created)
}

/// The ChildStart a managed binding never invokes: the compiled topology
/// needs one per child, and `bind_managed` consumes it unused.
fn unused_child_start(
    _: &asupersync::cx::Scope<'static, asupersync::types::policy::FailFast>,
    _: &mut asupersync::runtime::RuntimeState,
    _: &Cx,
) -> Result<TaskId, asupersync::runtime::SpawnError> {
    unreachable!("a managed supervisor never runs a legacy ChildStart")
}

/// A server's name and the shared state: the server records its canonical
/// id under its name from its own Cx, as `run_program` does for spawned
/// children, since enqueue order alone cannot pair spawns with admissions
/// once one was refused or named another way. It records from `on_start`
/// and `on_stop`, one of which every admitted server runs (terminate runs
/// even when init is skipped), and from its handlers. The hooks stay
/// no-ops otherwise: nothing observable changes.
struct ServerName {
    name: String,
    shared: SharedRef,
}

impl ServerName {
    fn record(&self, cx: &Cx) {
        lock(&self.shared)
            .self_ids
            .entry(self.name.clone())
            .or_insert_with(|| cx.task_id());
    }
}

/// The DSL's built-in GenServer behaviours (§3.8): `counter` keeps a u64
/// that a cast adds to and a call returns; `echo` replies to a call with
/// its request. Both keep the trait's defaults (budgets, Reject overflow,
/// no-op info); their start and stop hooks only record their ids. The
/// other counters of the family differ from `counter` in one trait hook.
struct CounterServer(u64, ServerName, CounterKind);
struct EchoServer(ServerName);

/// Which `counter` behaviour a [`CounterServer`] is (DSL §3.8).
#[derive(Clone, Copy, PartialEq, Eq)]
enum CounterKind {
    /// `counter`: the trait's defaults.
    Plain,
    /// `latest`: `cast_overflow_policy` is `DropOldest`.
    Latest,
    /// `budgets`: `on_start_budget` is 7 polls at priority 200 and
    /// `on_stop_budget` the trait's default (`Budget::MINIMAL`); the start
    /// and stop hooks trace the budget they run under.
    Budgets,
    /// `budgets_stop`: as `budgets`, with `on_stop_budget` 42 polls at
    /// priority 250.
    BudgetsStop,
}

impl CounterKind {
    fn traces_budget(self) -> bool {
        matches!(self, Self::Budgets | Self::BudgetsStop)
    }
}

/// The budget a `budgets` server's hook runs under, as the trace the hook
/// emits: the same text in C's interpreter.
fn probe_trace(cx: &Cx, hook: &str) {
    let budget = cx.budget();
    cx.trace(&format!(
        "probe:{hook} polls={} priority={}",
        budget.poll_quota, budget.priority
    ));
}

/// A DOWN's reason as the DSL names it (DSL §3.10). The error text and the
/// cancel reason are not projected.
fn down_reason_kind(reason: &DownReason) -> &'static str {
    match reason {
        DownReason::Normal => "normal",
        DownReason::Error(_) => "error",
        DownReason::Cancelled(_) => "cancelled",
        DownReason::Panicked(_) => "panicked",
    }
}

/// What a server's `handle_info` leaves for the oracle (DSL §3.10): one user
/// trace per system message, with the monitor reference, the reason and the
/// virtual time of the exit behind it. The C server writes the same text.
fn trace_system_msg(cx: &Cx, msg: &SystemMsg) {
    match msg {
        SystemMsg::Down {
            completion_vt,
            notification,
        } => cx.trace(&format!(
            "down:ref={} reason={} vt={}",
            notification.monitor_ref.id(),
            down_reason_kind(&notification.reason),
            completion_vt.as_nanos()
        )),
        SystemMsg::Exit {
            exit_vt, reason, ..
        } => cx.trace(&format!(
            "exit:reason={} vt={}",
            down_reason_kind(reason),
            exit_vt.as_nanos()
        )),
        SystemMsg::Timeout { .. } => cx.trace("timeout"),
    }
}

/// A monitor opening's failure as a status (vocabulary §5).
fn watch_error_status(err: WatchError) -> &'static str {
    match err {
        WatchError::NotFound => "ASX_E_NOT_FOUND",
        WatchError::WatcherNotFound => "ASX_E_STALE_HANDLE",
        WatchError::Cancelled => "ASX_E_CANCELLED",
        _ => "ASX_E_INVALID_STATE",
    }
}

/// The task a monitor step names: its handle when the harness holds one
/// (so a task spawned a moment ago, not yet admitted, is found once it is),
/// else its id (DSL §3.10).
fn watch_target_of(shared: &SharedRef, name: &str) -> RunResult<WatchTarget> {
    let s = lock(shared);
    if let Some(handle) = s.handles.get(name) {
        return Ok(WatchTarget::from(handle));
    }
    s.task_ids
        .get(name)
        .copied()
        .map(WatchTarget::from)
        .ok_or_else(|| format!("unknown task {name:?}"))
}

impl GenServer for CounterServer {
    type Call = u64;
    type Reply = u64;
    type Cast = u64;
    type Info = SystemMsg;

    fn on_start_budget(&self) -> Budget {
        if self.2.traces_budget() {
            Budget::new().with_poll_quota(7).with_priority(200)
        } else {
            Budget::INFINITE
        }
    }

    fn on_stop_budget(&self) -> Budget {
        match self.2 {
            CounterKind::BudgetsStop => Budget::new().with_poll_quota(42).with_priority(250),
            _ => Budget::MINIMAL,
        }
    }

    fn cast_overflow_policy(&self) -> CastOverflowPolicy {
        match self.2 {
            CounterKind::Latest => CastOverflowPolicy::DropOldest,
            _ => CastOverflowPolicy::Reject,
        }
    }

    fn handle_call(
        &mut self,
        cx: &Cx,
        _request: u64,
        reply: Reply<u64>,
    ) -> Pin<Box<dyn Future<Output = ()> + Send + '_>> {
        self.1.record(cx);
        let value = self.0;
        Box::pin(async move {
            let _ = reply.send(value);
        })
    }

    fn handle_cast(&mut self, cx: &Cx, msg: u64) -> Pin<Box<dyn Future<Output = ()> + Send + '_>> {
        self.1.record(cx);
        self.0 = self.0.wrapping_add(msg);
        Box::pin(async {})
    }

    fn handle_info(
        &mut self,
        cx: &Cx,
        msg: SystemMsg,
    ) -> Pin<Box<dyn Future<Output = ()> + Send + '_>> {
        self.1.record(cx);
        trace_system_msg(cx, &msg);
        Box::pin(async {})
    }

    fn on_start(&mut self, cx: &Cx) -> Pin<Box<dyn Future<Output = ()> + Send + '_>> {
        self.1.record(cx);
        if self.2.traces_budget() {
            probe_trace(cx, "start");
        }
        Box::pin(async {})
    }

    fn on_stop(&mut self, cx: &Cx) -> Pin<Box<dyn Future<Output = ()> + Send + '_>> {
        self.1.record(cx);
        if self.2.traces_budget() {
            probe_trace(cx, "stop");
        }
        Box::pin(async {})
    }
}

impl GenServer for EchoServer {
    type Call = u64;
    type Reply = u64;
    type Cast = u64;
    type Info = SystemMsg;

    fn handle_call(
        &mut self,
        cx: &Cx,
        request: u64,
        reply: Reply<u64>,
    ) -> Pin<Box<dyn Future<Output = ()> + Send + '_>> {
        self.0.record(cx);
        Box::pin(async move {
            let _ = reply.send(request);
        })
    }

    fn handle_cast(&mut self, cx: &Cx, _msg: u64) -> Pin<Box<dyn Future<Output = ()> + Send + '_>> {
        self.0.record(cx);
        Box::pin(async {})
    }

    fn handle_info(
        &mut self,
        cx: &Cx,
        msg: SystemMsg,
    ) -> Pin<Box<dyn Future<Output = ()> + Send + '_>> {
        self.0.record(cx);
        trace_system_msg(cx, &msg);
        Box::pin(async {})
    }

    fn on_start(&mut self, cx: &Cx) -> Pin<Box<dyn Future<Output = ()> + Send + '_>> {
        self.0.record(cx);
        Box::pin(async {})
    }

    fn on_stop(&mut self, cx: &Cx) -> Pin<Box<dyn Future<Output = ()> + Send + '_>> {
        self.0.record(cx);
        Box::pin(async {})
    }
}

enum Server {
    Counter(GenServerHandle<CounterServer>),
    Echo(GenServerHandle<EchoServer>),
}

/// A channel endpoint, moved into its owner's body at setup (DSL §3.6).
enum Endpoint {
    MpscTx(mpsc::Sender<u64>),
    MpscRx(mpsc::Receiver<u64>),
    OneshotTx(oneshot::Sender<u64>),
    OneshotRx(oneshot::Receiver<u64>),
    BroadcastTx(broadcast::Sender<u64>),
    BroadcastRx(broadcast::Receiver<u64>),
    WatchTx(watch::Sender<u64>),
    WatchRx(watch::Receiver<u64>),
}

impl Local {
    fn adopt(&mut self, channel: String, endpoint: Endpoint) {
        match endpoint {
            Endpoint::MpscTx(tx) => {
                self.mpsc_tx.insert(channel, Box::new(tx));
            }
            Endpoint::MpscRx(rx) => {
                self.mpsc_rx.insert(channel, rx);
            }
            Endpoint::OneshotTx(tx) => {
                self.oneshot_tx.insert(channel, tx);
            }
            Endpoint::OneshotRx(rx) => {
                self.oneshot_rx.insert(channel, rx);
            }
            Endpoint::BroadcastTx(tx) => {
                self.broadcast_tx.insert(channel, tx);
            }
            Endpoint::BroadcastRx(rx) => {
                self.broadcast_rx.insert(channel, rx);
            }
            Endpoint::WatchTx(tx) => {
                self.watch_tx.insert(channel, tx);
            }
            Endpoint::WatchRx(rx) => {
                self.watch_rx.insert(channel, rx);
            }
        }
    }

    /// Drop this task's sender (`sender`) or receiver on `channel`, of
    /// whatever type it is (DSL `close_sender` / `close_receiver`).
    fn close(&mut self, channel: &str, sender: bool) -> RunResult<()> {
        let dropped = if sender {
            self.mpsc_tx.remove(channel).map(drop).is_some()
                || self.oneshot_tx.remove(channel).map(drop).is_some()
                || self.broadcast_tx.remove(channel).map(drop).is_some()
                || self.watch_tx.remove(channel).map(drop).is_some()
        } else {
            self.mpsc_rx.remove(channel).map(drop).is_some()
                || self.oneshot_rx.remove(channel).map(drop).is_some()
                || self.broadcast_rx.remove(channel).map(drop).is_some()
                || self.watch_rx.remove(channel).map(drop).is_some()
        };
        if dropped {
            Ok(())
        } else {
            Err(format!(
                "this task does not own the {} of {channel:?}",
                if sender { "sender" } else { "receiver" }
            ))
        }
    }

    /// `'static` borrows of this task's sender on `channel` and of its Cx,
    /// so that a permit reserved with them can be held across steps, as a
    /// Rust body holds one across awaits.
    fn lend(&self, channel: &str) -> RunResult<(&'static mpsc::Sender<u64>, &'static Cx)> {
        let tx = self
            .mpsc_tx
            .get(channel)
            .ok_or_else(|| format!("this task does not own the sender of {channel:?}"))?;
        let cx = self.cx.as_ref().ok_or("task Cx not anchored")?;
        let tx: *const mpsc::Sender<u64> = &**tx;
        let cx: *const Cx = &**cx;
        // SAFETY: both are boxed, so their addresses do not move with the
        // `Local`. The sender box is freed only once no permit on its channel
        // is held (by `close_sender`, or deferred to the last permit's
        // resolution in `deferred_tx`); the Cx box is never replaced; and
        // when `Local` drops, `send_permits` goes first (fields drop in
        // declaration order). A borrow taken for a `send` or `reserve`
        // future lives within one step, and the steps of a task never
        // overlap.
        Ok(unsafe { (&*tx, &*cx) })
    }

    fn receiver(&mut self, channel: &str) -> RunResult<&mut mpsc::Receiver<u64>> {
        self.mpsc_rx
            .get_mut(channel)
            .ok_or_else(|| format!("this task does not own the receiver of {channel:?}"))
    }

    /// The key a DelayQueue step names (DSL §3.9): bound when its insert ran,
    /// and still bound once its entry is dead.
    fn delay_key(&self, queue: &str, key: &str) -> RunResult<DelayKey> {
        self.delay_keys
            .get(&(queue.to_string(), key.to_string()))
            .copied()
            .ok_or_else(|| format!("delay queue {queue:?} has no key {key:?}"))
    }

    /// The watch receiver a step selects (DSL §3.6): the clone its `receiver`
    /// field names, else the receiver the task declared on `channel`.
    fn watch_receiver(&mut self, step: &Value) -> RunResult<&mut watch::Receiver<u64>> {
        let channel = str_field(step, "channel")?;
        match step.get("receiver").and_then(Value::as_str) {
            Some(alias) => self
                .watch_clones
                .get_mut(channel)
                .and_then(|clones| clones.get_mut(alias))
                .ok_or_else(|| format!("this task holds no watch clone {alias:?} of {channel:?}")),
            None => self
                .watch_rx
                .get_mut(channel)
                .ok_or_else(|| format!("this task does not own a watch receiver of {channel:?}")),
        }
    }
}

// Channel errors onto C statuses (vocabulary §5).
fn send_error_status<T>(err: &mpsc::SendError<T>) -> &'static str {
    match err {
        mpsc::SendError::Disconnected(_) => "ASX_E_DISCONNECTED",
        mpsc::SendError::Cancelled(_) => "ASX_E_CANCELLED",
        mpsc::SendError::Full(_) => "ASX_E_CHANNEL_FULL",
    }
}

fn recv_error_status(err: mpsc::RecvError) -> &'static str {
    match err {
        mpsc::RecvError::Disconnected => "ASX_E_DISCONNECTED",
        mpsc::RecvError::Cancelled => "ASX_E_CANCELLED",
        mpsc::RecvError::Empty => "ASX_E_CHANNEL_EMPTY",
    }
}

fn oneshot_send_status<T>(err: &oneshot::SendError<T>) -> &'static str {
    match err {
        oneshot::SendError::Disconnected(_) => "ASX_E_DISCONNECTED",
        oneshot::SendError::Cancelled(_) => "ASX_E_CANCELLED",
    }
}

fn oneshot_recv_status(err: oneshot::RecvError) -> &'static str {
    match err {
        oneshot::RecvError::Closed => "ASX_E_DISCONNECTED",
        oneshot::RecvError::Cancelled => "ASX_E_CANCELLED",
        oneshot::RecvError::PolledAfterCompletion => "ASX_E_INVALID_STATE",
    }
}

fn broadcast_send_status<T>(err: &broadcast::SendError<T>) -> &'static str {
    match err {
        broadcast::SendError::Closed(_) => "ASX_E_DISCONNECTED",
        broadcast::SendError::Cancelled(_) => "ASX_E_CANCELLED",
    }
}

/// A broadcast receive error's status and observation value: `Lagged(n)`
/// carries `n` (vocabulary §5).
fn broadcast_recv_status(err: broadcast::RecvError) -> (&'static str, Value) {
    match err {
        broadcast::RecvError::Lagged(n) => ("ASX_E_LAGGED", json!(n)),
        broadcast::RecvError::Closed => ("ASX_E_DISCONNECTED", Value::Null),
        broadcast::RecvError::Cancelled => ("ASX_E_CANCELLED", Value::Null),
        broadcast::RecvError::PolledAfterCompletion => ("ASX_E_INVALID_STATE", Value::Null),
    }
}

fn watch_recv_status(err: watch::RecvError) -> &'static str {
    match err {
        watch::RecvError::Closed => "ASX_E_DISCONNECTED",
        watch::RecvError::Cancelled => "ASX_E_CANCELLED",
        watch::RecvError::PolledAfterCompletion => "ASX_E_INVALID_STATE",
    }
}

/// A declared sync object (DSL §3.7). Each but the mutex is leaked for the
/// capture's lifetime, one allocation per declaration, so permits can be
/// held across steps the way a task body holds them across awaits; the
/// mutex is shared for its owned guards.
#[derive(Clone)]
enum SyncObj {
    Mutex(Arc<asx_sync::Mutex<()>>),
    RwLock(Arc<asx_sync::RwLock<()>>),
    Semaphore(&'static asx_sync::Semaphore),
    Barrier(&'static asx_sync::Barrier),
    Notify(&'static asx_sync::Notify),
}

/// A held rwlock guard; dropping it releases the lock.
#[expect(dead_code, reason = "a guard is held only for its Drop")]
enum RwGuard {
    Read(asx_sync::OwnedRwLockReadGuard<()>),
    Write(asx_sync::OwnedRwLockWriteGuard<()>),
}

fn sync_obj(shared: &SharedRef, step: &Value, field: &str) -> RunResult<SyncObj> {
    let name = str_field(step, field)?;
    lock(shared)
        .sync
        .get(name)
        .cloned()
        .ok_or_else(|| format!("unknown sync object {name:?}"))
}

// Sync errors onto C statuses (vocabulary §5).
fn lock_error_status(err: asx_sync::LockError) -> &'static str {
    use asx_sync::LockError as E;
    match err {
        E::Cancelled => "ASX_E_CANCELLED",
        E::TimedOut(_) => "ASX_E_TIMED_OUT",
        E::Poisoned | E::PolledAfterCompletion => "ASX_E_INVALID_STATE",
    }
}

fn rwlock_error_status(err: asx_sync::RwLockError) -> &'static str {
    use asx_sync::RwLockError as E;
    match err {
        E::Cancelled => "ASX_E_CANCELLED",
        E::Poisoned | E::PolledAfterCompletion => "ASX_E_INVALID_STATE",
    }
}

fn acquire_error_status(err: asx_sync::AcquireError) -> &'static str {
    use asx_sync::AcquireError as E;
    match err {
        E::Cancelled => "ASX_E_CANCELLED",
        E::Closed => "ASX_E_DISCONNECTED",
        E::PolledAfterCompletion => "ASX_E_INVALID_STATE",
    }
}

fn barrier_error_status(err: asx_sync::BarrierWaitError) -> &'static str {
    use asx_sync::BarrierWaitError as E;
    match err {
        E::Cancelled => "ASX_E_CANCELLED",
        E::PolledAfterCompletion => "ASX_E_INVALID_STATE",
    }
}

enum Flow {
    Continue,
    Return(Body),
}

struct TaskCtx {
    shared: SharedRef,
    me: String,
    region: String,
    tokens: Arc<Mutex<HashMap<String, ObligationToken>>>,
}

fn run_program(
    cx: Cx,
    ctx: Arc<TaskCtx>,
    steps: Vec<Value>,
    endpoints: Vec<(String, Endpoint)>,
) -> Pin<Box<dyn Future<Output = Body> + Send>> {
    // Spawn factories receive the admitted task's real context while
    // constructing its future, before its first poll.
    {
        let mut s = lock(&ctx.shared);
        if s.handoff_destinations.contains(&ctx.me) {
            s.task_contexts.insert(ctx.me.clone(), cx.clone());
        }
        s.obligation_tokens
            .insert(ctx.me.clone(), Arc::downgrade(&ctx.tokens));
    }
    Box::pin(async move {
        // Its canonical id under its own name: a spawn its admission refused
        // has no TaskAdmitted event, so resolve_admissions names admitted
        // children by this rather than by enqueue order alone.
        lock(&ctx.shared)
            .self_ids
            .entry(ctx.me.clone())
            .or_insert_with(|| cx.task_id());
        let mut local = Local {
            _tokens: OwnedTokens {
                tokens: ctx.tokens.clone(),
            },
            cx: Some(Box::new(cx.clone())),
            ..Local::default()
        };
        for (channel, endpoint) in endpoints {
            local.adopt(channel, endpoint);
        }
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

/// A sync finalizer's program (DSL "Region finalizers"): the non-blocking
/// steps, run to their end inside the one poll of the finalizer's task. The
/// closure `register_sync_finalizer` takes is `FnOnce()`; the lab wraps it in
/// `async move { f() }` and spawns that as the region's masked cleanup task
/// (`drain_ready_async_finalizers_in`, state.rs:8612), so the Cx of the polled
/// task is the ambient one.
fn run_sync_program(cx: Cx, ctx: &Arc<TaskCtx>, steps: &[Value]) -> Body {
    let mut local = Local {
        _tokens: OwnedTokens {
            tokens: ctx.tokens.clone(),
        },
        cx: Some(Box::new(cx.clone())),
        ..Local::default()
    };
    for (i, step) in steps.iter().enumerate() {
        let label = match str_field(step, "op") {
            Ok(op) => op.to_string(),
            Err(err) => {
                harness_error(
                    &ctx.shared,
                    format!("finalizer {} step {}: {err}", ctx.me, i + 1),
                );
                return Body::Err("ASX_E_INVALID_STATE".to_string());
            }
        };
        match exec_sync(&cx, ctx, &mut local, i + 1, &label, step) {
            Ok(Flow::Continue) => {}
            Ok(Flow::Return(body)) => return body,
            Err(err) => {
                harness_error(
                    &ctx.shared,
                    format!("finalizer {} step {}: {err}", ctx.me, i + 1),
                );
                return Body::Err("ASX_E_INVALID_STATE".to_string());
            }
        }
    }
    Body::Ok
}

/// The observation of a step naming an obligation, guard, semaphore permit
/// or send permit the task does not hold, because the step that would have
/// acquired it failed or it was already released (DSL §3).
const NOT_HELD: &str = "ASX_E_NOT_FOUND";

/// `depth` nested `Cx::masked` sections around `body` (the DSL's
/// `masked.depth`). A level that would exceed MAX_MASK_DEPTH panics inside
/// `Cx::masked` (cx.rs:3323), unwinding out of every level already entered.
fn masked_nested(
    cx: &Cx,
    depth: u64,
    body: &mut dyn FnMut() -> RunResult<Flow>,
) -> RunResult<Flow> {
    if depth <= 1 {
        cx.masked(&mut *body)
    } else {
        cx.masked(|| masked_nested(cx, depth - 1, body))
    }
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
        "is_cancel_requested" => {
            let requested = cx.is_cancel_requested();
            observe(
                &ctx.shared,
                me,
                idx,
                label,
                "ASX_OK",
                Value::Bool(requested),
            );
        }
        "capability_budget" => {
            // cx.capability_budget() (cx.rs:2516), or a held child region's
            // (ChildRegion::cx(): the envelope its record admitted).
            let (status, value) = match step.get("region").and_then(Value::as_str) {
                None => ("ASX_OK", capability_budget_json(&cx.capability_budget())),
                Some(name) => match local.regions.get(name) {
                    Some(region) => (
                        "ASX_OK",
                        capability_budget_json(&region.cx().capability_budget()),
                    ),
                    None => (NOT_HELD, Value::Null),
                },
            };
            observe(&ctx.shared, me, idx, label, status, value);
        }
        "apply_capability_budget" => {
            // cx.apply_child_capability_budget (cx.rs:2606): the Cx carries
            // the effective envelope afterwards, and is unchanged on a
            // refusal.
            let envelope = capability_budget(step.get("capability_budget"))?;
            let requirements = capability_requirements(step.get("requirements"))?;
            match cx.apply_child_capability_budget(envelope, requirements) {
                Ok(effective) => observe(
                    &ctx.shared,
                    me,
                    idx,
                    label,
                    "ASX_OK",
                    capability_budget_json(&effective),
                ),
                Err(refusal) => observe(
                    &ctx.shared,
                    me,
                    idx,
                    label,
                    "ASX_E_CAPABILITY_BUDGET_REFUSED",
                    capability_refusal_json(&refusal),
                ),
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
                    let old = lock(&ctx.tokens).insert(name, token);
                    drop(old);
                    "ASX_OK".to_string()
                }
                Ok(None) => return Err("obligation registration without a runtime".to_string()),
                Err(e) => obligation_admission_status(&e)?,
            };
            observe(&ctx.shared, me, idx, label, &status, Value::Null);
        }
        "commit" | "abort" | "leak" => {
            let name = str_field(step, "obligation")?;
            let token = lock(&ctx.tokens).remove(name);
            let Some(token) = token else {
                // Its reserve failed, or a step resolved it (DSL §3).
                observe(&ctx.shared, me, idx, label, NOT_HELD, Value::Null);
                return Ok(Flow::Continue);
            };
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
        "handoff" => {
            let name = str_field(step, "obligation")?;
            let destination = str_field(step, "task")?;
            let alias = str_field(step, "as")?;
            if !lock(&ctx.tokens).contains_key(name) {
                observe(&ctx.shared, me, idx, label, NOT_HELD, Value::Null);
                return Ok(Flow::Continue);
            }
            let (destination_cx, destination_tokens) = {
                let s = lock(&ctx.shared);
                let destination_cx = s.task_contexts.get(destination).cloned();
                let destination_tokens =
                    s.obligation_tokens.get(destination).and_then(Weak::upgrade);
                (destination_cx, destination_tokens)
            };
            if destination != me
                && destination_tokens
                    .as_ref()
                    .is_some_and(|tokens| lock(tokens).contains_key(alias))
            {
                return Err(format!(
                    "handoff destination token {destination}/{alias} exists"
                ));
            }
            let token = lock(&ctx.tokens).remove(name);
            let Some(token) = token else {
                observe(&ctx.shared, me, idx, label, NOT_HELD, Value::Null);
                return Ok(Flow::Continue);
            };
            let source_ticket = token.ticket();
            let source_holder = token.holder();
            let source_region = token.region();
            let (status, token, token_name, token_store) = match destination_cx {
                Some(destination_cx) => match token.try_transfer(&destination_cx) {
                    Ok(next) => {
                        let store = destination_tokens.ok_or_else(|| {
                            format!("live destination {destination} has no token store")
                        })?;
                        lock(&ctx.shared).handoff_tickets.insert(
                            (source_ticket, next.ticket()),
                            (source_holder, next.holder(), source_region, next.region()),
                        );
                        ("ASX_OK".to_string(), next, alias, store)
                    }
                    Err(failure) => {
                        let (reason, original) = failure.into_parts();
                        (
                            obligation_transfer_status(&reason)?,
                            original,
                            name,
                            ctx.tokens.clone(),
                        )
                    }
                },
                None => (NOT_HELD.to_string(), token, name, ctx.tokens.clone()),
            };
            lock(&token_store).insert(token_name.to_string(), token);
            observe(&ctx.shared, me, idx, label, &status, Value::Null);
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
                tokens: Arc::default(),
            });
            let factory = move |ccx: Cx| run_program(ccx, child_ctx, program, Vec::new());
            let spawned = if let Some(r) = step.get("region").and_then(Value::as_str) {
                let Some(region) = local.regions.get(r) else {
                    // A region the task does not hold (its open failed, or
                    // it was closed): nothing is spawned, and the child's
                    // later steps observe that too (DSL §3).
                    lock(&ctx.shared).unspawned.insert(child);
                    observe(&ctx.shared, me, idx, label, NOT_HELD, Value::Null);
                    return Ok(Flow::Continue);
                };
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
                    let at = s.observations.len();
                    s.spawn_observations.insert(child.clone(), at);
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
            if s.unspawned.contains(target) {
                s.observations.push(json!({
                    "task": me, "step": idx, "op": label, "status": NOT_HELD, "value": Value::Null,
                }));
                return Ok(Flow::Continue);
            }
            let handle = s
                .handles
                .get_mut(target)
                .ok_or_else(|| format!("unknown task {target:?}"))?;
            let result = match handle.try_join() {
                Ok(None) => None,
                Ok(Some(body)) => Some(Ok(body)),
                Err(e) => Some(Err(e)),
            };
            let result = match s.denied.remove(target) {
                Some(denied) => Some(denied),
                None => result,
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
            let mut s = lock(&ctx.shared);
            if s.unspawned.contains(target) {
                drop(s);
                observe(&ctx.shared, me, idx, label, NOT_HELD, Value::Null);
                return Ok(Flow::Continue);
            }
            let ran = s.self_ids.contains_key(target);
            // An earlier abort already took the refusal out of the handle
            // (try_join consumes it): a second abort must not replace it
            // with PolledAfterCompletion (bd-c129; gen-275-53 aborted
            // t.3.c1 twice).
            let refused_already = s.denied.contains_key(target);
            let handle = s
                .handles
                .get_mut(target)
                .ok_or_else(|| format!("unknown task {target:?}"))?;
            // Finished without ever running: its admission refused it.
            let denied = if !ran && !refused_already && handle.is_finished() {
                handle.try_join().err()
            } else {
                None
            };
            handle.abort_with_reason(reason);
            if let Some(err) = denied {
                s.denied.insert(target.to_string(), Err(err));
            }
            drop(s);
            observe(&ctx.shared, me, idx, label, "ASX_OK", Value::Null);
        }
        "cancel_region" => {
            let name = str_field(step, "region")?;
            let reason = cancel_reason(step, cx.region_id(), Some(cx.task_id()), cx.now())?;
            let Some(region) = local.regions.get(name) else {
                observe(&ctx.shared, me, idx, label, NOT_HELD, Value::Null);
                return Ok(Flow::Continue);
            };
            let status = match region.cancel(reason) {
                Ok(()) => "ASX_OK".to_string(),
                Err(e) => child_region_status(&e)?,
            };
            observe(&ctx.shared, me, idx, label, &status, Value::Null);
        }
        // Non-blocking sync steps (DSL §3.7).
        "mutex_unlock" => {
            let name = str_field(step, "mutex")?;
            let status = match local.guards.remove(name) {
                Some(guard) => {
                    drop(guard);
                    "ASX_OK"
                }
                None => NOT_HELD, // its mutex_lock failed (DSL §3)
            };
            observe(&ctx.shared, me, idx, label, status, Value::Null);
        }
        "rwlock_unlock" => {
            // Drops the task's most recent guard on the lock (read or write).
            let name = str_field(step, "rwlock")?;
            let status = match local.rw_guards.get_mut(name).and_then(Vec::pop) {
                Some(guard) => {
                    drop(guard);
                    "ASX_OK"
                }
                None => NOT_HELD, // its rwlock_read / rwlock_write failed (DSL §3)
            };
            observe(&ctx.shared, me, idx, label, status, Value::Null);
        }
        "rwlock_downgrade" => {
            // The task's most recent write guard on the lock becomes a read
            // guard in place (OwnedRwLockWriteGuard::downgrade,
            // sync/rwlock.rs:1287).
            let name = str_field(step, "rwlock")?;
            let held = local.rw_guards.get_mut(name).and_then(|guards| {
                let i = guards
                    .iter()
                    .rposition(|g| matches!(g, RwGuard::Write(_)))?;
                Some((guards, i))
            });
            let status = match held {
                Some((guards, i)) => {
                    let RwGuard::Write(write) = guards.remove(i) else {
                        return Err("rposition found no write guard".to_string());
                    };
                    guards.insert(i, RwGuard::Read(write.downgrade()));
                    "ASX_OK"
                }
                None => NOT_HELD, // no write guard held on it (DSL §3)
            };
            observe(&ctx.shared, me, idx, label, status, Value::Null);
        }
        "sem_forget" => {
            // Forgets the task's latest permit on the semaphore: its
            // permits never return (SemaphorePermit::forget,
            // sync/semaphore.rs:1041).
            let name = str_field(step, "semaphore")?;
            let status = match local.permits.get_mut(name).and_then(Vec::pop) {
                Some(permit) => {
                    permit.forget();
                    "ASX_OK"
                }
                None => NOT_HELD, // its sem_acquire failed (DSL §3)
            };
            observe(&ctx.shared, me, idx, label, status, Value::Null);
        }
        "sem_add_permits" => {
            let SyncObj::Semaphore(s) = sync_obj(&ctx.shared, step, "semaphore")? else {
                return Err("sem_add_permits on a non-semaphore".to_string());
            };
            let count =
                usize::try_from(u64_field(step, "count")?).map_err(|_| "count out of range")?;
            s.add_permits(count);
            observe(&ctx.shared, me, idx, label, "ASX_OK", Value::Null);
        }
        "sem_release" => {
            let name = str_field(step, "semaphore")?;
            let status = match local.permits.get_mut(name).and_then(Vec::pop) {
                Some(permit) => {
                    drop(permit);
                    "ASX_OK"
                }
                None => NOT_HELD, // its sem_acquire failed (DSL §3)
            };
            observe(&ctx.shared, me, idx, label, status, Value::Null);
        }
        "notify_one" | "notify_all" => {
            let SyncObj::Notify(n) = sync_obj(&ctx.shared, step, "notify")? else {
                return Err(format!("{op} on a non-notify"));
            };
            if op == "notify_one" {
                // Whether a waiter was woken (else a permit is stored) is
                // not observable in the vocabulary.
                let _woken = n.notify_one();
            } else {
                n.notify_waiters();
            }
            observe(&ctx.shared, me, idx, label, "ASX_OK", Value::Null);
        }
        // Non-blocking mpsc steps (DSL §3.6).
        "permit_send" | "permit_abort" => {
            let name = str_field(step, "permit")?;
            let Some((channel, permit)) = local.send_permits.remove(name) else {
                // Its reserve_send failed (DSL §3).
                observe(&ctx.shared, me, idx, label, NOT_HELD, Value::Null);
                return Ok(Flow::Continue);
            };
            let status = if op == "permit_send" {
                match permit.send(u64_field(step, "value")?) {
                    asupersync::Outcome::Ok(()) => "ASX_OK",
                    asupersync::Outcome::Err(e) => send_error_status(&e),
                    other => return Err(format!("permit send outcome {other:?}")),
                }
            } else {
                permit.abort();
                "ASX_OK"
            };
            // The permit's sender, closed while it was held, is dropped now
            // that no permit of the channel is left.
            if !local.send_permits.values().any(|(c, _)| *c == channel) {
                local.deferred_tx.remove(&channel);
            }
            observe(&ctx.shared, me, idx, label, status, Value::Null);
        }
        "try_send" => {
            let (tx, _) = local.lend(str_field(step, "channel")?)?;
            let status = match tx.try_send(u64_field(step, "value")?) {
                Ok(()) => "ASX_OK",
                Err(e) => send_error_status(&e),
            };
            observe(&ctx.shared, me, idx, label, status, Value::Null);
        }
        "send_evict_oldest" => {
            // Never parks; the value is the evicted message, if any.
            let channel = str_field(step, "channel")?;
            if lock(&ctx.shared).unbounded_channels.contains(channel) {
                return Err(format!(
                    "send_evict_oldest on the unbounded channel {channel:?} (UnboundedSender has none)"
                ));
            }
            let (tx, _) = local.lend(channel)?;
            match tx.send_evict_oldest(u64_field(step, "value")?) {
                Ok(evicted) => observe(&ctx.shared, me, idx, label, "ASX_OK", json!(evicted)),
                Err(e) => observe(
                    &ctx.shared,
                    me,
                    idx,
                    label,
                    send_error_status(&e),
                    Value::Null,
                ),
            }
        }
        "try_recv" => {
            let rx = local.receiver(str_field(step, "channel")?)?;
            match rx.try_recv() {
                Ok(v) => observe(&ctx.shared, me, idx, label, "ASX_OK", json!(v)),
                Err(e) => observe(
                    &ctx.shared,
                    me,
                    idx,
                    label,
                    recv_error_status(e),
                    Value::Null,
                ),
            }
        }
        "close_sender" => {
            let ch = str_field(step, "channel")?;
            if local.send_permits.values().any(|(c, _)| c == ch) {
                // A permit borrows its sender (SendPermit<'a>), so a Rust
                // body drops the sender only after the permit is gone: the
                // drop is deferred to the last permit of the channel. The
                // sender is out of the task's hands from here on.
                let tx = local
                    .mpsc_tx
                    .remove(ch)
                    .ok_or_else(|| format!("this task does not own the sender of {ch:?}"))?;
                local.deferred_tx.insert(ch.to_string(), tx);
            } else {
                local.close(ch, true)?;
            }
            observe(&ctx.shared, me, idx, label, "ASX_OK", Value::Null);
        }
        "close_receiver" => {
            let ch = str_field(step, "channel")?;
            match step.get("receiver").and_then(Value::as_str) {
                // A watch clone made by watch_clone (DSL §3.6).
                Some(alias) => {
                    let clone = local
                        .watch_clones
                        .get_mut(ch)
                        .and_then(|clones| clones.remove(alias));
                    if clone.is_none() {
                        return Err(format!(
                            "this task holds no watch clone {alias:?} of {ch:?}"
                        ));
                    }
                }
                None => local.close(ch, false)?,
            }
            observe(&ctx.shared, me, idx, label, "ASX_OK", Value::Null);
        }
        // rx.close() (mpsc.rs:1698): unlike close_receiver (the drop), the
        // receiver stays held and the queue receivable.
        "seal" => {
            let ch = str_field(step, "channel")?;
            if local.sealed.contains(ch) {
                return Err(format!("a second seal of {ch:?} (outside DSL §3.6)"));
            }
            local.receiver(ch)?.close();
            local.sealed.insert(ch.to_string());
            observe(&ctx.shared, me, idx, label, "ASX_OK", Value::Null);
        }
        // Non-blocking oneshot, broadcast and watch sends (DSL §3.6).
        "oneshot_is_closed" | "oneshot_is_ready" => {
            let ch = str_field(step, "channel")?;
            let end = if op == "oneshot_is_ready" {
                "receiver"
            } else {
                str_field(step, "end")?
            };
            let value = match end {
                "sender" => local
                    .oneshot_tx
                    .get(ch)
                    .ok_or_else(|| format!("this task does not own the oneshot sender of {ch:?}"))?
                    .is_closed(),
                "receiver" => {
                    let rx = local.oneshot_rx.get(ch).ok_or_else(|| {
                        format!("this task does not own the oneshot receiver of {ch:?}")
                    })?;
                    if op == "oneshot_is_ready" {
                        rx.is_ready()
                    } else {
                        rx.is_closed()
                    }
                }
                other => return Err(format!("unknown oneshot end {other:?}")),
            };
            observe(&ctx.shared, me, idx, label, "ASX_OK", Value::Bool(value));
        }
        "oneshot_send" => {
            let ch = str_field(step, "channel")?;
            let tx = local
                .oneshot_tx
                .remove(ch)
                .ok_or_else(|| format!("this task does not own the oneshot sender of {ch:?}"))?;
            let status = match tx.send(cx, u64_field(step, "value")?) {
                Ok(()) => "ASX_OK",
                Err(e) => oneshot_send_status(&e),
            };
            observe(&ctx.shared, me, idx, label, status, Value::Null);
        }
        "broadcast_send" => {
            let ch = str_field(step, "channel")?;
            let tx = local
                .broadcast_tx
                .get(ch)
                .ok_or_else(|| format!("this task does not own the broadcast sender of {ch:?}"))?;
            // The value is the number of receivers reached (broadcast.rs:494).
            match tx.send(cx, u64_field(step, "value")?) {
                Ok(n) => observe(&ctx.shared, me, idx, label, "ASX_OK", json!(n)),
                Err(e) => observe(
                    &ctx.shared,
                    me,
                    idx,
                    label,
                    broadcast_send_status(&e),
                    Value::Null,
                ),
            }
        }
        "watch_send" => {
            let ch = str_field(step, "channel")?;
            let tx = local
                .watch_tx
                .get(ch)
                .ok_or_else(|| format!("this task does not own the watch sender of {ch:?}"))?;
            let status = match tx.send(u64_field(step, "value")?) {
                Ok(()) => "ASX_OK",
                Err(watch::SendError::Closed(_)) => "ASX_E_DISCONNECTED",
            };
            observe(&ctx.shared, me, idx, label, status, Value::Null);
        }
        // The watch steps that never suspend (DSL §3.6).
        "watch_send_modify" => {
            let ch = str_field(step, "channel")?;
            let add = u64_field(step, "add")?;
            let tx = local
                .watch_tx
                .get(ch)
                .ok_or_else(|| format!("this task does not own the watch sender of {ch:?}"))?;
            let status = match tx.send_modify(|v| *v = v.wrapping_add(add)) {
                Ok(()) => "ASX_OK",
                Err(_) => "ASX_E_DISCONNECTED",
            };
            observe(&ctx.shared, me, idx, label, status, Value::Null);
        }
        "watch_receiver_count" => {
            let ch = str_field(step, "channel")?;
            let tx = local
                .watch_tx
                .get(ch)
                .ok_or_else(|| format!("this task does not own the watch sender of {ch:?}"))?;
            let count = tx.receiver_count() as u64;
            observe(&ctx.shared, me, idx, label, "ASX_OK", json!(count));
        }
        "watch_borrow" | "watch_borrow_and_update" | "watch_mark_seen" | "watch_has_changed" => {
            let rx = local.watch_receiver(step)?;
            let value = match op {
                "watch_borrow" => json!(*rx.borrow()),
                "watch_borrow_and_update" => json!(*rx.borrow_and_update()),
                "watch_mark_seen" => {
                    rx.mark_seen();
                    Value::Null
                }
                _ => json!(rx.has_changed()),
            };
            observe(&ctx.shared, me, idx, label, "ASX_OK", value);
        }
        "watch_is_closed" => {
            let ch = str_field(step, "channel")?;
            let closed = match str_field(step, "end")? {
                "sender" => local
                    .watch_tx
                    .get(ch)
                    .ok_or_else(|| format!("this task does not own the watch sender of {ch:?}"))?
                    .is_closed(),
                "receiver" => local.watch_receiver(step)?.is_closed(),
                other => return Err(format!("watch_is_closed end {other:?}")),
            };
            observe(&ctx.shared, me, idx, label, "ASX_OK", json!(closed));
        }
        "watch_clone" => {
            let ch = str_field(step, "channel")?;
            let alias = str_field(step, "as")?.to_string();
            if local
                .watch_clones
                .get(ch)
                .is_some_and(|clones| clones.contains_key(&alias))
            {
                return Err(format!("watch clone {alias:?} of {ch:?} is named twice"));
            }
            let clone = local.watch_receiver(step)?.clone();
            local
                .watch_clones
                .entry(ch.to_string())
                .or_default()
                .insert(alias, clone);
            observe(&ctx.shared, me, idx, label, "ASX_OK", Value::Null);
        }
        // Interval and DelayQueue (DSL §3.9): held by the task under a name,
        // none of them suspends or registers a timer.
        "interval_new" => {
            let name = str_field(step, "as")?.to_string();
            let period_ns = u64_field(step, "period_ns")?;
            if period_ns == 0 {
                return Err("interval_new with a zero period (Rust panics)".to_string());
            }
            let period = Duration::from_nanos(period_ns);
            let mut iv = match step.get("start_ns").and_then(Value::as_u64) {
                Some(start) => interval_at(Time::from_nanos(start), period),
                None => interval(cx.now(), period),
            };
            iv.set_missed_tick_behavior(missed_tick_behavior(step)?);
            if local.intervals.insert(name.clone(), iv).is_some() {
                return Err(format!("interval {name:?} is named twice"));
            }
            observe(&ctx.shared, me, idx, label, "ASX_OK", Value::Null);
        }
        "interval_tick" => {
            let name = str_field(step, "interval")?;
            let iv = local
                .intervals
                .get_mut(name)
                .ok_or_else(|| format!("this task holds no interval {name:?}"))?;
            // `Some(t)` is a due tick at its own deadline; `None` leaves the
            // deadline unmoved and is reported as `due: false`.
            let (time, due) = match iv.poll_tick(cx.now()) {
                Some(t) => (t, true),
                None => (iv.deadline(), false),
            };
            let value = json!({"time_ns": time.as_nanos(), "due": due});
            observe(&ctx.shared, me, idx, label, "ASX_OK", value);
        }
        // The rest of Interval's surface (time/interval.rs:205, :228, :309,
        // :320, :342, :360, :378): each reads the clock once.
        "interval_set_behavior"
        | "interval_deadline"
        | "interval_remaining"
        | "interval_is_ready"
        | "interval_reset"
        | "interval_reset_at"
        | "interval_reset_after" => {
            let name = str_field(step, "interval")?;
            let now = cx.now();
            let iv = local
                .intervals
                .get_mut(name)
                .ok_or_else(|| format!("this task holds no interval {name:?}"))?;
            let value = match op {
                "interval_set_behavior" => {
                    iv.set_missed_tick_behavior(missed_tick_behavior(step)?);
                    Value::Null
                }
                "interval_deadline" => json!(iv.deadline().as_nanos()),
                "interval_remaining" => {
                    json!(u64::try_from(iv.remaining(now).as_nanos()).unwrap_or(u64::MAX))
                }
                "interval_is_ready" => json!(iv.is_ready(now)),
                "interval_reset" => {
                    iv.reset(now);
                    Value::Null
                }
                "interval_reset_at" => {
                    iv.reset_at(Time::from_nanos(u64_field(step, "at_ns")?));
                    Value::Null
                }
                _ => {
                    iv.reset_after(now, Duration::from_nanos(u64_field(step, "after_ns")?));
                    Value::Null
                }
            };
            observe(&ctx.shared, me, idx, label, "ASX_OK", value);
        }
        "delay_queue_new" => {
            let name = str_field(step, "as")?.to_string();
            if local
                .delay_queues
                .insert(name.clone(), DelayQueue::new())
                .is_some()
            {
                return Err(format!("delay queue {name:?} is named twice"));
            }
            observe(&ctx.shared, me, idx, label, "ASX_OK", Value::Null);
        }
        "delay_queue_insert" | "delay_queue_insert_at" => {
            let queue = str_field(step, "queue")?.to_string();
            let key_name = str_field(step, "as")?.to_string();
            let value = u64_field(step, "value")?;
            if local
                .delay_keys
                .contains_key(&(queue.clone(), key_name.clone()))
            {
                return Err(format!(
                    "key {key_name:?} of queue {queue:?} is named twice"
                ));
            }
            let q = local
                .delay_queues
                .get_mut(&queue)
                .ok_or_else(|| format!("this task holds no delay queue {queue:?}"))?;
            let key = if op == "delay_queue_insert" {
                q.insert(value, Duration::from_nanos(u64_field(step, "timeout_ns")?))
            } else {
                q.insert_at(value, Time::from_nanos(u64_field(step, "deadline_ns")?))
            };
            local.delay_keys.insert((queue, key_name), key);
            observe(&ctx.shared, me, idx, label, "ASX_OK", Value::Null);
        }
        "delay_queue_reset" | "delay_queue_reset_at" => {
            let queue = str_field(step, "queue")?;
            let key = local.delay_key(queue, str_field(step, "key")?)?;
            let q = local
                .delay_queues
                .get_mut(queue)
                .ok_or_else(|| format!("this task holds no delay queue {queue:?}"))?;
            if !q.contains(&key) {
                return Err("delay_queue_reset of a dead key (Rust panics)".to_string());
            }
            if op == "delay_queue_reset" {
                q.reset(&key, Duration::from_nanos(u64_field(step, "timeout_ns")?));
            } else {
                q.reset_at(&key, Time::from_nanos(u64_field(step, "deadline_ns")?));
            }
            observe(&ctx.shared, me, idx, label, "ASX_OK", Value::Null);
        }
        "delay_queue_remove" => {
            let queue = str_field(step, "queue")?;
            let key = local.delay_key(queue, str_field(step, "key")?)?;
            let q = local
                .delay_queues
                .get_mut(queue)
                .ok_or_else(|| format!("this task holds no delay queue {queue:?}"))?;
            match q.try_remove(&key) {
                Some(expired) => {
                    let value = json!({"value": *expired.get_ref(), "deadline_ns": expired.deadline().as_nanos()});
                    observe(&ctx.shared, me, idx, label, "ASX_OK", value);
                }
                None => observe(&ctx.shared, me, idx, label, "ASX_E_NOT_FOUND", Value::Null),
            }
        }
        "delay_queue_contains" | "delay_queue_deadline" | "delay_queue_len" => {
            let queue = str_field(step, "queue")?;
            let value = if op == "delay_queue_len" {
                let q = local
                    .delay_queues
                    .get(queue)
                    .ok_or_else(|| format!("this task holds no delay queue {queue:?}"))?;
                json!(q.len() as u64)
            } else {
                let key = local.delay_key(queue, str_field(step, "key")?)?;
                let q = local
                    .delay_queues
                    .get(queue)
                    .ok_or_else(|| format!("this task holds no delay queue {queue:?}"))?;
                if op == "delay_queue_contains" {
                    json!(q.contains(&key))
                } else if q.contains(&key) {
                    json!(q.deadline(&key).as_nanos())
                } else {
                    return Err("delay_queue_deadline of a dead key (Rust panics)".to_string());
                }
            };
            observe(&ctx.shared, me, idx, label, "ASX_OK", value);
        }
        "delay_queue_clear" => {
            let queue = str_field(step, "queue")?;
            local
                .delay_queues
                .get_mut(queue)
                .ok_or_else(|| format!("this task holds no delay queue {queue:?}"))?
                .clear();
            if local.live_queue.as_deref() == Some(queue) {
                local.live_queue = None;
            }
            observe(&ctx.shared, me, idx, label, "ASX_OK", Value::Null);
        }
        // Dropping the Monitor (monitor.rs:1203) queues a Demonitor command
        // unless the DOWN was delivered (DSL §3.10).
        "demonitor" => {
            let name = str_field(step, "monitor")?;
            if local.monitors.remove(name).is_none() {
                return Err(format!("this task holds no monitor {name:?}"));
            }
            observe(&ctx.shared, me, idx, label, "ASX_OK", Value::Null);
        }
        // Link::unlink(self) (link.rs:891, DSL §3.11): an Unlink command; the
        // Link is consumed.
        "unlink" => {
            let name = str_field(step, "link")?;
            let (link, _, _) = local
                .links
                .remove(name)
                .ok_or_else(|| format!("this task holds no link {name:?}"))?;
            link.unlink();
            observe(&ctx.shared, me, idx, label, "ASX_OK", Value::Null);
        }
        // GenServerHandle::unlink(cx, ref) (gen_server.rs:2012).
        "server_unlink" => {
            let server = str_field(step, "server")?;
            let name = str_field(step, "link")?;
            let link_ref = local
                .server_links
                .remove(name)
                .ok_or_else(|| format!("this task holds no server link {name:?}"))?;
            match local
                .servers
                .get(server)
                .ok_or_else(|| format!("unknown server {server:?}"))?
            {
                Server::Counter(h) => h.unlink(cx, link_ref),
                Server::Echo(h) => h.unlink(cx, link_ref),
            }
            observe(&ctx.shared, me, idx, label, "ASX_OK", Value::Null);
        }
        // GenServerHandle::demonitor(cx, ref) (gen_server.rs:1978).
        "server_demonitor" => {
            let server = str_field(step, "server")?;
            let name = str_field(step, "monitor")?;
            let reference = local
                .server_monitors
                .remove(name)
                .ok_or_else(|| format!("this task holds no server monitor {name:?}"))?;
            match local
                .servers
                .get(server)
                .ok_or_else(|| format!("unknown server {server:?}"))?
            {
                Server::Counter(h) => h.demonitor(cx, reference),
                Server::Echo(h) => h.demonitor(cx, reference),
            }
            observe(&ctx.shared, me, idx, label, "ASX_OK", Value::Null);
        }
        // A masked step inside masked nests; each inner observation's op is
        // its path, e.g. masked/2/masked/1/checkpoint (DSL §3.2).
        "masked" => {
            let steps = step
                .get("steps")
                .and_then(Value::as_array)
                .ok_or("masked without steps")?;
            // `depth` nests the steps in that many masks (DSL §3.2). Rust
            // asserts `mask_depth < MAX_MASK_DEPTH` on each entry
            // (cx.rs:3323), so the 65th level panics before any step runs.
            let depth = match step.get("depth") {
                None => 1,
                Some(v) => v
                    .as_u64()
                    .filter(|d| (1..=80).contains(d))
                    .ok_or("masked depth must be 1 to 80")?,
            };
            let mut body = || -> RunResult<Flow> {
                for (j, inner) in steps.iter().enumerate() {
                    let path = format!("{label}/{}/{}", j + 1, str_field(inner, "op")?);
                    if let Flow::Return(body) = exec_sync(cx, ctx, local, idx, &path, inner)? {
                        return Ok(Flow::Return(body));
                    }
                }
                Ok(Flow::Continue)
            };
            return masked_nested(cx, depth, &mut body);
        }
        other => {
            return Err(format!("op {other:?} is not interpreted yet (increment 3)"));
        }
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

fn obligation_transfer_status(
    err: &asupersync::runtime::obligation_mailbox::ObligationTransferError,
) -> RunResult<String> {
    use asupersync::runtime::obligation_mailbox::{
        ObligationAdmissionError as A, ObligationTransferError as E,
    };
    Ok(match err {
        E::SameHolder => "ASX_E_INVALID_ARGUMENT",
        E::DestinationCancelled => "ASX_E_CANCELLED",
        E::SourceNotChecked | E::SourceHolderNotLive => "ASX_E_INVALID_STATE",
        E::SourceResolved | E::SourceSettlementStarted => "ASX_E_OBLIGATION_ALREADY_RESOLVED",
        E::RuntimeUnavailable => "ASX_E_SCHEDULER_UNAVAILABLE",
        E::Destination(A::HolderNotLive) => "ASX_E_INVALID_STATE",
        E::Destination(reason) => return obligation_admission_status(reason),
        other => return Err(format!("unmapped obligation transfer error {other:?}")),
    }
    .to_string())
}

fn child_region_status(err: &asupersync::cx::ChildRegionError) -> RunResult<String> {
    use asupersync::cx::ChildRegionError as E;
    use asupersync::runtime::region_table::RegionCreateError as C;
    Ok(match err {
        E::NoRuntimeGateway | E::RuntimeUnavailable => "ASX_E_SCHEDULER_UNAVAILABLE",
        E::Create(C::ParentNotFound(_)) => "ASX_E_NOT_FOUND",
        E::Create(C::ParentClosed { .. }) => "ASX_E_REGION_CLOSED",
        E::Create(C::ParentAtCapacity { .. }) => "ASX_E_ADMISSION_LIMIT",
        E::Create(C::CapabilityBudgetRefused { .. }) => "ASX_E_CAPABILITY_BUDGET_REFUSED",
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
        E::RegionNotFound(_) => "ASX_E_NOT_FOUND",
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
    let arms_timer = matches!(op, "sleep" | "sleep_until" | "mutex_lock_until" | "timeout")
        || (op == "race" && step.get("deadline_ns").is_some());
    if arms_timer && local.live_queue.is_some() {
        return Err("a timer step while a DelayQueue's own timer is live (DSL §3.9)".to_string());
    }
    match op {
        // Blocking mpsc steps (DSL §3.6).
        "reserve_send" => {
            let ch = str_field(step, "channel")?.to_string();
            let name = str_field(step, "as")?.to_string();
            // The lent Cx is a clone of `cx`: the same task and state.
            let (tx, lent_cx) = local.lend(&ch)?;
            let status = match tx.reserve(lent_cx).await {
                Ok(permit) => {
                    if local.send_permits.insert(name, (ch, permit)).is_some() {
                        return Err("reserve_send reuses a held permit name".to_string());
                    }
                    "ASX_OK"
                }
                Err(e) => send_error_status(&e),
            };
            observe(&ctx.shared, me, idx, op, status, Value::Null);
        }
        "send" => {
            let (tx, _) = local.lend(str_field(step, "channel")?)?;
            let status = match tx.send(cx, u64_field(step, "value")?).await {
                Ok(()) => "ASX_OK",
                Err(e) => send_error_status(&e),
            };
            observe(&ctx.shared, me, idx, op, status, Value::Null);
        }
        "recv" => {
            let rx = local.receiver(str_field(step, "channel")?)?;
            match rx.recv(cx).await {
                Ok(v) => observe(&ctx.shared, me, idx, op, "ASX_OK", json!(v)),
                Err(e) => observe(&ctx.shared, me, idx, op, recv_error_status(e), Value::Null),
            }
        }
        "recv_many" => {
            let limit = usize::try_from(u64_field(step, "limit")?)
                .map_err(|_| "recv_many limit does not fit usize".to_string())?;
            let rx = local.receiver(str_field(step, "channel")?)?;
            let mut batch = Vec::new();
            match rx.recv_many(cx, &mut batch, limit).await {
                Ok(_) => observe(&ctx.shared, me, idx, op, "ASX_OK", json!(batch)),
                Err(e) => observe(&ctx.shared, me, idx, op, recv_error_status(e), Value::Null),
            }
        }
        "closed" => {
            // No Cx: closure, not cancellation, ends the wait (mpsc_closed.rs:39).
            let channel = str_field(step, "channel")?;
            if lock(&ctx.shared).unbounded_channels.contains(channel) {
                return Err(format!(
                    "closed on the unbounded channel {channel:?} (UnboundedSender has none)"
                ));
            }
            let (tx, _) = local.lend(channel)?;
            tx.closed().await;
            observe(&ctx.shared, me, idx, op, "ASX_OK", Value::Null);
        }
        // Blocking oneshot, broadcast and watch receives (DSL §3.6).
        "oneshot_poll_closed" => {
            // No Cx: a cancel does not end the wait (oneshot.rs:619, :1373).
            let ch = str_field(step, "channel")?;
            match str_field(step, "end")? {
                "sender" => {
                    let tx = local.oneshot_tx.get_mut(ch).ok_or_else(|| {
                        format!("this task does not own the oneshot sender of {ch:?}")
                    })?;
                    std::future::poll_fn(|c| tx.poll_closed(c)).await;
                }
                "receiver" => {
                    let rx = local.oneshot_rx.get_mut(ch).ok_or_else(|| {
                        format!("this task does not own the oneshot receiver of {ch:?}")
                    })?;
                    std::future::poll_fn(|c| rx.poll_closed(c)).await;
                }
                other => return Err(format!("unknown oneshot end {other:?}")),
            }
            observe(&ctx.shared, me, idx, op, "ASX_OK", Value::Null);
        }
        "oneshot_recv" => {
            let ch = str_field(step, "channel")?;
            let rx = local
                .oneshot_rx
                .get_mut(ch)
                .ok_or_else(|| format!("this task does not own the oneshot receiver of {ch:?}"))?;
            match rx.recv(cx).await {
                Ok(v) => observe(&ctx.shared, me, idx, op, "ASX_OK", json!(v)),
                Err(e) => observe(
                    &ctx.shared,
                    me,
                    idx,
                    op,
                    oneshot_recv_status(e),
                    Value::Null,
                ),
            }
        }
        "broadcast_recv" => {
            let ch = str_field(step, "channel")?;
            let rx = local
                .broadcast_rx
                .get_mut(ch)
                .ok_or_else(|| format!("this task does not own a broadcast receiver of {ch:?}"))?;
            match rx.recv(cx).await {
                Ok(v) => observe(&ctx.shared, me, idx, op, "ASX_OK", json!(v)),
                Err(e) => {
                    let (status, value) = broadcast_recv_status(e);
                    observe(&ctx.shared, me, idx, op, status, value);
                }
            }
        }
        "watch_changed" => {
            let rx = local.watch_receiver(step)?;
            match rx.changed(cx).await {
                Ok(()) => {
                    let v = *rx.borrow_and_update();
                    observe(&ctx.shared, me, idx, op, "ASX_OK", json!(v));
                }
                Err(e) => observe(&ctx.shared, me, idx, op, watch_recv_status(e), Value::Null),
            }
        }
        // rx.wait_for(&cx, |v| *v <cmp> value) (watch.rs:858): the accepted
        // value is the observation's, and only that version is marked seen.
        "watch_wait_for" => {
            let want = u64_field(step, "value")?;
            let holds: fn(u64, u64) -> bool = match str_field(step, "cmp")? {
                "eq" => |v, w| v == w,
                "ne" => |v, w| v != w,
                "lt" => |v, w| v < w,
                "le" => |v, w| v <= w,
                "gt" => |v, w| v > w,
                "ge" => |v, w| v >= w,
                other => return Err(format!("watch_wait_for cmp {other:?}")),
            };
            let rx = local.watch_receiver(step)?;
            let accepted = rx.wait_for(cx, |v| holds(*v, want)).await.map(|v| *v);
            match accepted {
                Ok(v) => observe(&ctx.shared, me, idx, op, "ASX_OK", json!(v)),
                Err(e) => observe(&ctx.shared, me, idx, op, watch_recv_status(e), Value::Null),
            }
        }
        // Cx::monitor(target).await (monitor.rs:1090, DSL §3.10): the
        // request goes through the region-command lane, so the opening
        // resolves a scheduler step later; the Monitor is held by name.
        "monitor" => {
            let target = str_field(step, "task")?.to_string();
            let name = str_field(step, "as")?.to_string();
            if local.monitors.contains_key(&name) {
                return Err(format!("monitor {name:?} is named twice"));
            }
            let watch_target = watch_target_of(&ctx.shared, &target)?;
            match cx.monitor(watch_target).await {
                Ok(monitor) => {
                    let reference = monitor.monitor_ref().id();
                    local.monitors.insert(name, (monitor, target));
                    observe(&ctx.shared, me, idx, op, "ASX_OK", json!(reference));
                }
                Err(e) => observe(&ctx.shared, me, idx, op, watch_error_status(e), Value::Null),
            }
        }
        // Monitor::down(&cx).await (:1170): the DOWN's task, reason kind and
        // reference; Cancelled if this task is cancelled first.
        "await_down" => {
            let name = str_field(step, "monitor")?;
            let (monitor, target) = local
                .monitors
                .get(name)
                .ok_or_else(|| format!("this task holds no monitor {name:?}"))?;
            match monitor.down(cx).await {
                Ok(down) => {
                    let value = json!({
                        "monitored": target,
                        "reason": down_reason_kind(&down.reason),
                        "ref": down.monitor_ref.id(),
                    });
                    observe(&ctx.shared, me, idx, op, "ASX_OK", value);
                }
                Err(e) => observe(&ctx.shared, me, idx, op, watch_error_status(e), Value::Null),
            }
        }
        // GenServerHandle::monitor(cx, target).await (gen_server.rs:1946):
        // the server holds the monitor and gets the DOWN in handle_info.
        "server_monitor" => {
            let server = str_field(step, "server")?;
            let target = str_field(step, "task")?.to_string();
            let name = str_field(step, "as")?.to_string();
            if local.server_monitors.contains_key(&name) {
                return Err(format!("server monitor {name:?} is named twice"));
            }
            let watch_target = watch_target_of(&ctx.shared, &target)?;
            let opening = match local
                .servers
                .get(server)
                .ok_or_else(|| format!("unknown server {server:?}"))?
            {
                Server::Counter(h) => h.monitor(cx, watch_target),
                Server::Echo(h) => h.monitor(cx, watch_target),
            };
            match opening.await {
                Ok(reference) => {
                    local.server_monitors.insert(name, reference);
                    observe(&ctx.shared, me, idx, op, "ASX_OK", json!(reference.id()));
                }
                Err(e) => observe(&ctx.shared, me, idx, op, watch_error_status(e), Value::Null),
            }
        }
        // Cx::link(peer).await / Cx::link_trapping(peer).await (link.rs:915,
        // :923, DSL §3.11): through the region-command lane like a monitor.
        // The reference is a process-wide counter in Rust, so it is not
        // observed.
        "link" | "link_trapping" => {
            let peer = str_field(step, "task")?.to_string();
            let name = str_field(step, "as")?.to_string();
            if local.links.contains_key(&name) {
                return Err(format!("link {name:?} is named twice"));
            }
            let watch_target = watch_target_of(&ctx.shared, &peer)?;
            let opening = if op == "link" {
                cx.link(watch_target)
            } else {
                cx.link_trapping(watch_target)
            };
            match opening.await {
                Ok(link) => {
                    local
                        .links
                        .insert(name, (link, peer, op == "link_trapping"));
                    observe(&ctx.shared, me, idx, op, "ASX_OK", Value::Null);
                }
                Err(e) => observe(&ctx.shared, me, idx, op, watch_error_status(e), Value::Null),
            }
        }
        // Link::exit(&cx).await (link.rs:882): the exit signal of a
        // trapping link, as the peer's name and the reason kind. On a
        // propagating link Rust's wait ends in the holder's own
        // cancellation, which C does not model: a harness error.
        "await_exit" => {
            let name = str_field(step, "link")?;
            let (link, peer, trapping) = local
                .links
                .get(name)
                .ok_or_else(|| format!("this task holds no link {name:?}"))?;
            if !trapping {
                return Err(format!(
                    "await_exit of the link {name:?}, which does not trap"
                ));
            }
            match link.exit(cx).await {
                Ok(signal) => {
                    let value = json!({
                        "from": peer,
                        "reason": down_reason_kind(&signal.reason),
                    });
                    observe(&ctx.shared, me, idx, op, "ASX_OK", value);
                }
                Err(e) => observe(&ctx.shared, me, idx, op, watch_error_status(e), Value::Null),
            }
        }
        // GenServerHandle::link / link_trapping (gen_server.rs:1991, :2003).
        "server_link" | "server_link_trapping" => {
            let server = str_field(step, "server")?;
            let peer = str_field(step, "task")?.to_string();
            let name = str_field(step, "as")?.to_string();
            if local.server_links.contains_key(&name) {
                return Err(format!("server link {name:?} is named twice"));
            }
            let watch_target = watch_target_of(&ctx.shared, &peer)?;
            let trapping = op == "server_link_trapping";
            let opening = match local
                .servers
                .get(server)
                .ok_or_else(|| format!("unknown server {server:?}"))?
            {
                Server::Counter(h) if trapping => h.link_trapping(cx, watch_target),
                Server::Counter(h) => h.link(cx, watch_target),
                Server::Echo(h) if trapping => h.link_trapping(cx, watch_target),
                Server::Echo(h) => h.link(cx, watch_target),
            };
            match opening.await {
                Ok(link_ref) => {
                    local.server_links.insert(name, link_ref);
                    observe(&ctx.shared, me, idx, op, "ASX_OK", Value::Null);
                }
                Err(e) => observe(&ctx.shared, me, idx, op, watch_error_status(e), Value::Null),
            }
        }
        // DelayQueue::poll_expired awaited (time/delay_queue.rs:260, DSL
        // §3.9): a due entry, the end of an empty queue, or Pending on the
        // queue's own Sleep for the earliest deadline, which the task waits
        // out. That Sleep's Timer events are named as a sleep's: the first
        // schedule of this step takes the owner's next timer number.
        "delay_queue_poll" => {
            let queue = str_field(step, "queue")?;
            let q = local
                .delay_queues
                .get_mut(queue)
                .ok_or_else(|| format!("this task holds no delay queue {queue:?}"))?;
            let waited = std::sync::atomic::AtomicBool::new(false);
            let waited_ref = &waited;
            let polled = NamedSleep {
                sleep: Some(Box::pin(std::future::poll_fn(move |task_cx| {
                    let r = q.poll_expired(task_cx);
                    if r.is_pending() {
                        waited_ref.store(true, std::sync::atomic::Ordering::Relaxed);
                    }
                    r
                }))),
                buffer: cx.trace_buffer(),
                shared: ctx.shared.clone(),
                owner: me,
                counter: &mut local.timers,
                name: None,
            }
            .await;
            let did_wait = waited.load(std::sync::atomic::Ordering::Relaxed);
            if did_wait {
                if cx.is_cancel_requested() {
                    return Err("delay_queue_poll waited under a cancel request".to_string());
                }
                if local
                    .live_queue
                    .as_deref()
                    .is_some_and(|live| live != queue)
                {
                    return Err(
                        "a second DelayQueue waits while another's timer is live".to_string()
                    );
                }
            }
            match polled {
                Some(expired) => {
                    if did_wait {
                        local.live_queue = Some(queue.to_string());
                    }
                    let value = json!({
                        "value": *expired.get_ref(),
                        "deadline_ns": expired.deadline().as_nanos(),
                    });
                    observe(&ctx.shared, me, idx, op, "ASX_OK", value);
                }
                None => {
                    if local.live_queue.as_deref() == Some(queue) {
                        local.live_queue = None;
                    }
                    observe(&ctx.shared, me, idx, op, "ASX_E_CHANNEL_EMPTY", Value::Null);
                }
            }
        }
        // Blocking sync steps (DSL §3.7): `timeout` wraps the same function.
        "mutex_lock" | "rwlock_read" | "rwlock_write" | "sem_acquire" | "barrier_wait"
        | "notify_wait" => exec_sync_wait(cx, ctx, local, idx, step).await?,
        "mutex_lock_until" => {
            let name = str_field(step, "mutex")?.to_string();
            let SyncObj::Mutex(m) = sync_obj(&ctx.shared, step, "mutex")? else {
                return Err(format!("{name:?} is not a mutex"));
            };
            let deadline = Time::from_nanos(u64_field(step, "deadline_ns")?);
            // Its deadline Sleep's Timer events are named as a sleep's. The
            // borrowed guard is not Send: it is dropped in the poll that
            // takes it (DSL §3.7).
            let locked = NamedSleep {
                sleep: Some(Box::pin(m.lock_until(cx, deadline))),
                buffer: cx.trace_buffer(),
                shared: ctx.shared.clone(),
                owner: me,
                counter: &mut local.timers,
                name: None,
            }
            .await;
            let status = match locked {
                Ok(guard) => {
                    drop(guard);
                    "ASX_OK"
                }
                Err(e) => lock_error_status(e),
            };
            observe(&ctx.shared, me, idx, op, status, Value::Null);
        }
        // A standalone `time::timeout` around one blocking sync wait (DSL
        // §3.9). The inner step observes its own outcome; an Elapsed drops
        // it (its waiter leaves the line) and is this step's observation.
        // The deadline Sleep's Timer events are named as a sleep's. The
        // inner step holds `local`, so the timer counter is carried out and
        // back; a wrapped wait takes no timer number of its own.
        "timeout" => {
            let inner = step.get("inner").ok_or("timeout without inner")?;
            let duration = Duration::from_nanos(u64_field(step, "ns")?);
            let mut timers = local.timers;
            let timed = NamedSleep {
                sleep: Some(Box::pin(asupersync::time::timeout(
                    cx.now(),
                    duration,
                    exec_sync_wait(cx, ctx, local, idx, inner),
                ))),
                buffer: cx.trace_buffer(),
                shared: ctx.shared.clone(),
                owner: me,
                counter: &mut timers,
                name: None,
            }
            .await;
            local.timers = timers;
            match timed {
                Ok(done) => {
                    done?;
                }
                Err(_elapsed) => {
                    observe(&ctx.shared, me, idx, op, "ASX_E_TIMED_OUT", Value::Null);
                }
            }
        }
        "yield" => {
            asupersync::runtime::yield_now().await;
            observe(&ctx.shared, me, idx, op, "ASX_OK", Value::Null);
        }
        "sleep" | "sleep_until" => {
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
            NamedSleep {
                sleep: Some(Box::pin(sleep)),
                buffer: cx.trace_buffer(),
                shared: ctx.shared.clone(),
                owner: me,
                counter: &mut local.timers,
                name: None,
            }
            .await;
            observe(&ctx.shared, me, idx, op, "ASX_OK", Value::Null);
        }
        "return" => {
            let outcome = step.get("outcome").ok_or("return without outcome")?;
            observe(&ctx.shared, me, idx, op, "ASX_OK", Value::Null);
            let supervised = lock(&ctx.shared).supervised.contains(me);
            return Ok(Flow::Return(match str_field(outcome, "tag")? {
                "ok" => Body::Ok,
                "err" => {
                    let status = str_field(outcome, "status")?;
                    // The C body ends ASX_E_CANCELLED for Outcome::Cancelled,
                    // so the two cannot be told apart there (DSL §3.8).
                    if supervised && status == "ASX_E_CANCELLED" {
                        return Err(
                            "a supervised child returns Cancelled with tag cancelled, not err"
                                .to_string(),
                        );
                    }
                    Body::Err(status.to_string())
                }
                // The factory future's Outcome::Cancelled(reason): only a
                // supervised generation's body returns one (DSL §3.8). Its
                // reason is cx.cancel_reason() when a cancel reached the
                // generation, else a User reason attributed to it (§4).
                "cancelled" => {
                    if !supervised {
                        return Err(
                            "return cancelled outside a supervised child's program".to_string()
                        );
                    }
                    let reason = match cx.cancel_reason() {
                        Some(reason) => reason,
                        None => cancel_reason(
                            &json!({"kind": "User"}),
                            cx.region_id(),
                            Some(cx.task_id()),
                            cx.now(),
                        )?,
                    };
                    Body::Cancelled(reason)
                }
                // The DSL step *is* a panic; the runtime catches it and
                // reports JoinError::Panicked with this message.
                "panicked" => std::panic::panic_any(str_field(outcome, "message")?.to_string()),
                other => return Err(format!("unknown outcome tag {other:?}")),
            }));
        }
        // Task groups (DSL §3.5).
        "race" | "join_all" => {
            let deadline = match step.get("deadline_ns") {
                None => None,
                Some(_) if op != "race" => {
                    return Err("deadline_ns is only defined for race".to_string());
                }
                Some(d) => Some(d.as_u64().ok_or("deadline_ns must be an integer")?),
            };
            if let Some(d) = deadline {
                let Some(group) = race_with_deadline(cx, ctx, local, idx, step, d).await? else {
                    // Vocabulary §5: TimeoutError -> ASX_E_TIMED_OUT.
                    observe(&ctx.shared, me, idx, op, "ASX_E_TIMED_OUT", Value::Null);
                    return Ok(Flow::Continue);
                };
                let mut s = lock(&ctx.shared);
                let index = s.observations.len();
                s.group_values.push((index, group));
                s.observations.push(json!({
                    "task": me, "step": idx, "op": op, "status": "ASX_OK", "value": Value::Null,
                }));
                return Ok(Flow::Continue);
            }
            let handles = spawn_members(cx, ctx, idx, step)?;
            let group = if op == "race" {
                match cx.scope().race_all(cx, handles).await {
                    Ok((body, winner)) => GroupValue::Race(Some(winner), Ok(body)),
                    // Rust reports a failed winner without its index.
                    Err(e) => GroupValue::Race(None, Err(e)),
                }
            } else {
                GroupValue::JoinAll(cx.scope().join_all(cx, handles).await)
            };
            let mut s = lock(&ctx.shared);
            let index = s.observations.len();
            s.group_values.push((index, group));
            s.observations.push(json!({
                "task": me, "step": idx, "op": op, "status": "ASX_OK", "value": Value::Null,
            }));
        }
        "quorum" => {
            let needed =
                usize::try_from(u64_field(step, "needed")?).map_err(|_| "needed out of range")?;
            let members = group_programs(step)?;
            let buffer = cx.trace_buffer().ok_or("quorum needs the lab trace")?;
            let before = buffer.snapshot().last().map_or(0, |e| e.seq);
            let branches: Vec<_> = members
                .iter()
                .enumerate()
                .map(|(i, program)| {
                    let child_ctx = Arc::new(TaskCtx {
                        shared: ctx.shared.clone(),
                        me: member_name(me, idx, i),
                        region: ctx.region.clone(),
                        tokens: Arc::default(),
                    });
                    let program = program.clone();
                    move |ccx: Cx| async move {
                        match run_program(ccx, child_ctx, program, Vec::new()).await {
                            Body::Ok => Ok(()),
                            Body::Err(status) => Err(status),
                            // Only a supervised generation returns it.
                            Body::Cancelled(_) => Err("ASX_E_CANCELLED".to_string()),
                        }
                    }
                })
                .collect();
            let result = cx.scope().quorum(cx, needed, branches).await;
            // An impossible threshold is refused before anything is spawned.
            if let Err(QuorumError::InvalidQuorum { .. }) = result {
                observe(
                    &ctx.shared,
                    me,
                    idx,
                    op,
                    "ASX_E_INVALID_ARGUMENT",
                    Value::Null,
                );
                return Ok(Flow::Continue);
            }
            // The branches were spawned inside quorum, out of sight: name
            // them by their spawn enqueues, in order (resolve_admissions then
            // maps them to their canonical ids).
            let spawned: Vec<TaskId> = buffer
                .snapshot()
                .iter()
                .filter(|e| e.seq > before)
                .filter_map(|e| match (&e.kind, &e.data) {
                    (TraceEventKind::TaskSpawnEnqueued, TraceData::Task { task, region })
                        if *region == cx.region_id() =>
                    {
                        Some(*task)
                    }
                    _ => None,
                })
                .take(members.len())
                .collect();
            if spawned.len() != members.len() {
                return Err(format!(
                    "quorum spawned {} branches, the trace shows {}",
                    members.len(),
                    spawned.len()
                ));
            }
            {
                let mut s = lock(&ctx.shared);
                for (i, task) in spawned.into_iter().enumerate() {
                    let name = member_name(me, idx, i);
                    s.provisional.insert(task, name.clone());
                    s.task_regions.insert(name.clone(), ctx.region.clone());
                    s.group_members.insert(name);
                }
            }
            // Vocabulary §5: InsufficientSuccesses -> the first error (by
            // index); Cancelled -> ASX_E_CANCELLED; Panicked ->
            // ASX_E_INVALID_STATE. No value.
            match result {
                Ok(successes) => {
                    observe(&ctx.shared, me, idx, op, "ASX_OK", json!(successes.len()))
                }
                Err(QuorumError::InsufficientSuccesses { errors, .. }) => {
                    let status = errors
                        .into_iter()
                        .next()
                        .ok_or("quorum InsufficientSuccesses without errors")?;
                    observe(&ctx.shared, me, idx, op, &status, Value::Null);
                }
                Err(QuorumError::Cancelled(_)) => {
                    observe(&ctx.shared, me, idx, op, "ASX_E_CANCELLED", Value::Null)
                }
                Err(QuorumError::Panicked(_)) => {
                    observe(&ctx.shared, me, idx, op, "ASX_E_INVALID_STATE", Value::Null)
                }
                Err(QuorumError::InvalidQuorum { .. }) => {
                    return Err("quorum InvalidQuorum after spawning".to_string());
                }
            }
        }
        "first_ok" => {
            let members = group_programs(step)?;
            // The attempts are spawned one at a time inside first_ok, out of
            // sight, while other tasks may be spawning too: each names
            // itself when its factory runs, with the admitted task's own
            // (canonical) id.
            let factories: Vec<_> = members
                .iter()
                .enumerate()
                .map(|(i, program)| {
                    let child_ctx = Arc::new(TaskCtx {
                        shared: ctx.shared.clone(),
                        me: member_name(me, idx, i),
                        region: ctx.region.clone(),
                        tokens: Arc::default(),
                    });
                    let program = program.clone();
                    move |ccx: Cx| {
                        {
                            let mut s = lock(&child_ctx.shared);
                            let id = ccx.task_id();
                            s.task_ids.insert(child_ctx.me.clone(), id);
                            s.task_names.insert(id, child_ctx.me.clone());
                            s.task_regions
                                .insert(child_ctx.me.clone(), child_ctx.region.clone());
                            s.group_members.insert(child_ctx.me.clone());
                        }
                        async move {
                            match run_program(ccx, child_ctx, program, Vec::new()).await {
                                Body::Ok => Ok(()),
                                Body::Err(status) => Err(status),
                                // Only a supervised generation returns it.
                                Body::Cancelled(_) => Err("ASX_E_CANCELLED".to_string()),
                            }
                        }
                    }
                })
                .collect();
            let result = cx.scope().first_ok(cx, factories).await;
            // Vocabulary §5: Ok -> ASX_OK; AllFailed -> the first attempt's
            // error; Cancelled -> ASX_E_CANCELLED; Panicked ->
            // ASX_E_INVALID_STATE; Empty -> ASX_E_INVALID_ARGUMENT. No value.
            let status = match result {
                Ok(()) => "ASX_OK".to_string(),
                Err(FirstOkError::AllFailed { errors, .. }) => errors
                    .into_iter()
                    .next()
                    .ok_or("first_ok AllFailed without errors")?,
                Err(FirstOkError::Cancelled { .. }) => "ASX_E_CANCELLED".to_string(),
                Err(FirstOkError::Panicked(_)) => "ASX_E_INVALID_STATE".to_string(),
                Err(FirstOkError::Empty) => "ASX_E_INVALID_ARGUMENT".to_string(),
            };
            observe(&ctx.shared, me, idx, op, &status, Value::Null);
        }
        // Actors (DSL §3.8).
        "server_spawn" => {
            let name = str_field(step, "as")?.to_string();
            let mailbox =
                usize::try_from(u64_field(step, "mailbox")?).map_err(|e| e.to_string())?;
            let named = ServerName {
                name: name.clone(),
                shared: Arc::clone(&ctx.shared),
            };
            let behavior = str_field(step, "behavior")?;
            let counter_kind = match behavior {
                "counter" => Some(CounterKind::Plain),
                "latest" => Some(CounterKind::Latest),
                "budgets" => Some(CounterKind::Budgets),
                "budgets_stop" => Some(CounterKind::BudgetsStop),
                _ => None,
            };
            let spawned = match (counter_kind, behavior) {
                (Some(kind), _) => cx
                    .spawn_gen_server(CounterServer(0, named, kind), mailbox)
                    .map(|h| (h.task_id(), Server::Counter(h))),
                (None, "echo") => cx
                    .spawn_gen_server(EchoServer(named), mailbox)
                    .map(|h| (h.task_id(), Server::Echo(h))),
                (None, other) => return Err(format!("unknown behavior {other:?}")),
            };
            let (task, server) = spawned.map_err(|e| format!("server_spawn failed: {e:?}"))?;
            {
                let mut s = lock(&ctx.shared);
                s.provisional.insert(task, name.clone());
                s.task_regions.insert(name.clone(), ctx.region.clone());
                s.servers.insert(name.clone());
            }
            local.servers.insert(name, server);
            observe(&ctx.shared, me, idx, op, "ASX_OK", Value::Null);
        }
        "cast" => {
            let name = str_field(step, "server")?;
            let value = u64_field(step, "value")?;
            let result = match local
                .servers
                .get(name)
                .ok_or_else(|| format!("unknown server {name:?}"))?
            {
                Server::Counter(h) => h.cast(cx, value).await,
                Server::Echo(h) => h.cast(cx, value).await,
            };
            let status = match result {
                Ok(()) => "ASX_OK",
                Err(CastError::Full) => "ASX_E_CHANNEL_FULL",
                Err(CastError::ServerStopped) => "ASX_E_DISCONNECTED",
                Err(CastError::Cancelled(_)) => "ASX_E_CANCELLED",
            };
            observe(&ctx.shared, me, idx, op, status, Value::Null);
        }
        "try_cast" => {
            // `h.try_cast(msg)` (gen_server.rs:1305): never waits, never
            // checks cancellation, and applies the server's overflow policy.
            let name = str_field(step, "server")?;
            let value = u64_field(step, "value")?;
            let result = match local
                .servers
                .get(name)
                .ok_or_else(|| format!("unknown server {name:?}"))?
            {
                Server::Counter(h) => h.try_cast(value),
                Server::Echo(h) => h.try_cast(value),
            };
            let status = match result {
                Ok(()) => "ASX_OK",
                Err(CastError::Full) => "ASX_E_CHANNEL_FULL",
                Err(CastError::ServerStopped) => "ASX_E_DISCONNECTED",
                Err(CastError::Cancelled(_)) => "ASX_E_CANCELLED",
            };
            observe(&ctx.shared, me, idx, op, status, Value::Null);
        }
        "server_evicted" => {
            // `h.evicted_count()` (gen_server.rs:1153): the observation's
            // value is the number of casts evicted under DropOldest.
            let name = str_field(step, "server")?;
            let evicted = match local
                .servers
                .get(name)
                .ok_or_else(|| format!("unknown server {name:?}"))?
            {
                Server::Counter(h) => h.evicted_count(),
                Server::Echo(h) => h.evicted_count(),
            };
            observe(&ctx.shared, me, idx, op, "ASX_OK", json!(evicted));
        }
        "call" => {
            let name = str_field(step, "server")?;
            let request = u64_field(step, "request")?;
            if ctx.region == "root" {
                return Err("call from a root-region task (DSL §3.8)".to_string());
            }
            let result = match local
                .servers
                .get(name)
                .ok_or_else(|| format!("unknown server {name:?}"))?
            {
                Server::Counter(h) => h.call(cx, request).await,
                Server::Echo(h) => h.call(cx, request).await,
            };
            match result {
                Ok(v) => observe(&ctx.shared, me, idx, op, "ASX_OK", json!(v)),
                Err(e) => {
                    let status = match e {
                        CallError::ServerStopped => "ASX_E_DISCONNECTED",
                        CallError::NoReply => "ASX_E_INVALID_STATE",
                        CallError::Cancelled(_) => "ASX_E_CANCELLED",
                    };
                    observe(&ctx.shared, me, idx, op, status, Value::Null);
                }
            }
        }
        "server_stop" => {
            let name = str_field(step, "server")?;
            let mut server = local
                .servers
                .remove(name)
                .ok_or_else(|| format!("unknown server {name:?}"))?;
            let result = match &mut server {
                Server::Counter(h) => {
                    h.stop();
                    h.join(cx).await.map(|_| ())
                }
                Server::Echo(h) => {
                    h.stop();
                    h.join(cx).await.map(|_| ())
                }
            };
            let status = match &result {
                Ok(()) => "ASX_OK",
                Err(JoinError::Cancelled(_)) => "ASX_E_CANCELLED",
                Err(_) => "ASX_E_INVALID_STATE",
            };
            // The server task's outcome for the snapshot: its join's
            // (vocabulary §6).
            lock(&ctx.shared)
                .raw_outcomes
                .insert(name.to_string(), result.map(|()| Body::Ok));
            observe(&ctx.shared, me, idx, op, status, Value::Null);
        }
        "supervise" => {
            let name = str_field(step, "as")?.to_string();
            let policy = match str_field(step, "policy")? {
                "one_for_one" => RestartPolicy::OneForOne,
                "one_for_all" => RestartPolicy::OneForAll,
                "rest_for_one" => RestartPolicy::RestForOne,
                other => return Err(format!("unknown policy {other:?}")),
            };
            let escalation = match step.get("escalation").and_then(Value::as_str) {
                None | Some("stop") => EscalationPolicy::Stop,
                Some("escalate") => EscalationPolicy::Escalate,
                Some(other) => return Err(format!("unknown escalation {other:?}")),
            };
            let max_restarts =
                u32::try_from(u64_field(step, "max_restarts")?).map_err(|e| e.to_string())?;
            let window = Duration::from_nanos(u64_field(step, "window_ns")?);
            let children = step
                .get("children")
                .and_then(Value::as_array)
                .ok_or("supervise without children")?;
            let mut builder = SupervisorBuilder::new(name.clone()).with_restart_policy(policy);
            let mut bindings = Vec::with_capacity(children.len());
            let mut registered = false;
            for child in children {
                let child_name = str_field(child, "name")?.to_string();
                let mode = match str_field(child, "mode")? {
                    "permanent" => ManagedRestartMode::Permanent,
                    "transient" => ManagedRestartMode::Transient,
                    "temporary" => ManagedRestartMode::Temporary,
                    other => return Err(format!("unknown child mode {other:?}")),
                };
                let program = child
                    .get("program")
                    .and_then(Value::as_array)
                    .ok_or("supervised child without a program")?
                    .clone();
                let mut spec = ChildSpec::new(child_name.clone(), unused_child_start);
                // `depends_on` and `required` (DSL §3.8): a child starts
                // after the children it names, and a restart that finds one
                // of them not running stops the supervisor with
                // DependencyUnavailable when the child is required (the
                // default), else leaves the child stopped.
                for dependency in child
                    .get("depends_on")
                    .and_then(Value::as_array)
                    .into_iter()
                    .flatten()
                {
                    let dependency = dependency
                        .as_str()
                        .ok_or("a depends_on entry is a child name")?;
                    spec = spec.depends_on(dependency.to_string());
                }
                if let Some(required) = child.get("required") {
                    spec =
                        spec.with_required(required.as_bool().ok_or("required must be a boolean")?);
                }
                // `shutdown_budget` (DSL §3.8): the ceiling each of the
                // child's generations is drained with
                // (`ChildSpec::with_shutdown_budget`, supervision.rs:680);
                // absent or null is Budget::INFINITE, Rust's default.
                spec = spec.with_shutdown_budget(budget(child.get("shutdown_budget"))?);
                // `registration`: the name each of the child's generations
                // takes in the supervisor's registry before its factory runs
                // (DSL §3.8). `wait` has no DSL form: its 1 ms grant checks
                // are Sleeps made inside the library, which this harness
                // cannot name.
                if let Some(registration) = child.get("registration") {
                    let key = str_field(registration, "name")?.to_string();
                    let collision = match str_field(registration, "collision")? {
                        "fail" => NameCollisionPolicy::Fail,
                        "replace" => NameCollisionPolicy::Replace,
                        other => return Err(format!("unknown collision policy {other:?}")),
                    };
                    spec = spec.with_registration(NameRegistrationPolicy::Register {
                        name: key,
                        collision,
                    });
                    registered = true;
                    // Each generation's region also runs a cleanup task that
                    // retains the name, "<generation>/reg" (name_supervision).
                    let mut created = HashSet::new();
                    collect_created(&program, &mut created);
                    if created.contains("reg") {
                        return Err(format!(
                            "supervise {name:?}: child {child_name:?} registers a name, so \
                             its program cannot create `reg`, its retention task's name"
                        ));
                    }
                }
                builder = builder.child(spec);
                bindings.push(generation_binding(
                    &ctx.shared,
                    &name,
                    child_name,
                    mode,
                    program,
                ));
            }
            let compiled = builder
                .compile()
                .map_err(|e| format!("supervise {name:?}: {e}"))?;
            let mut config = SupervisionConfig::new(max_restarts, window)
                .with_restart_policy(policy)
                .with_escalation(escalation);
            // `storm_threshold_milli`: restarts per second times 1000, an
            // integer because DSL numbers are (DSL §3.8). Rust refuses a
            // threshold that is not above zero by panicking.
            if let Some(value) = step.get("storm_threshold_milli") {
                let milli = value
                    .as_u64()
                    .filter(|m| *m > 0)
                    .ok_or("storm_threshold_milli is not a positive integer")?;
                #[allow(clippy::cast_precision_loss)]
                let threshold = milli as f64 / 1000.0;
                config = config.with_storm_threshold(threshold);
            }
            // A supervisor with a registered child gets its own registry
            // (bind_managed_with_registry, supervision.rs:2117): the
            // children's names are shared among its generations only.
            let supervisor = if registered {
                compiled.bind_managed_with_registry(bindings, config, Arc::new(Default::default()))
            } else {
                compiled.bind_managed(bindings, config)
            }
            .map_err(|e| format!("supervise {name:?}: {e:?}"))?;
            // ManagedSupervisor::spawn (supervision.rs:3497-3505); NamedTimers
            // only names the controller's backoff timers.
            let report: ReportSlot = Arc::new(Mutex::new(None));
            let publication = Arc::clone(&report);
            let shared = Arc::clone(&ctx.shared);
            let owner = name.clone();
            let task = cx
                .spawn(move |controller_cx| async move {
                    // Its canonical id under its name, as run_program
                    // records a child's (resolve_admissions).
                    lock(&shared)
                        .self_ids
                        .entry(owner.clone())
                        .or_insert_with(|| controller_cx.task_id());
                    let buffer = controller_cx.trace_buffer();
                    let result = NamedTimers {
                        inner: Box::pin(supervisor.run(&controller_cx)),
                        buffer,
                        shared,
                        owner,
                        counter: 0,
                        active: None,
                    }
                    .await;
                    *lock_report(&publication) = Some(result);
                })
                .map_err(|e| format!("supervise spawn failed: {e:?}"))?;
            {
                let mut s = lock(&ctx.shared);
                s.provisional.insert(task.task_id(), name.clone());
                s.task_regions.insert(name.clone(), ctx.region.clone());
                s.supervisors.push(SupervisorInfo {
                    name: name.clone(),
                    region: ctx.region.clone(),
                    report: Arc::clone(&report),
                    provisional: task.task_id(),
                });
            }
            let previous = local.supervisors.insert(
                name.clone(),
                Supervisor {
                    name,
                    task: Some(task),
                    report,
                    joined: false,
                    shared: Arc::clone(&ctx.shared),
                },
            );
            if previous.is_some() {
                return Err("supervise reuses a supervisor name".to_string());
            }
            observe(&ctx.shared, me, idx, op, "ASX_OK", Value::Null);
        }
        "supervisor_join" => {
            let name = str_field(step, "supervisor")?;
            let supervisor = local
                .supervisors
                .get_mut(name)
                .filter(|s| !s.joined)
                .ok_or_else(|| format!("unknown or already joined supervisor {name:?}"))?;
            // ManagedSupervisorHandle::join (supervision.rs:3429-3443).
            let task = supervisor
                .task
                .as_mut()
                .ok_or("supervisor without a task")?;
            let terminal = std::future::poll_fn(|c| task.poll_join(c)).await;
            supervisor.joined = true;
            // Joined, the handle knows its controller's canonical id (one
            // that never ran recorded none itself).
            let id = task.task_id();
            {
                let mut s = lock(&ctx.shared);
                if s.supervisors
                    .iter()
                    .any(|i| i.name == name && i.provisional != id)
                {
                    s.self_ids.entry(name.to_string()).or_insert(id);
                }
            }
            let result = match lock_report(&supervisor.report).take() {
                Some(mut report) => {
                    if let Err(JoinError::Panicked(payload)) = &terminal {
                        report.outcome = asupersync::Outcome::Panicked(payload.clone());
                    }
                    Ok(report)
                }
                None => match terminal {
                    Err(error) => Err(error),
                    Ok(()) => return Err("managed controller omitted its report".to_string()),
                },
            };
            let status = match &result {
                Ok(_) => "ASX_OK",
                Err(JoinError::Cancelled(_)) => "ASX_E_CANCELLED",
                Err(_) => "ASX_E_INVALID_STATE",
            };
            let index = {
                let mut s = lock(&ctx.shared);
                // The controller's outcome for the snapshot: its join's
                // (vocabulary §6).
                let outcome = match &result {
                    Ok(_) => Ok(Body::Ok),
                    Err(JoinError::Cancelled(reason)) => Err(JoinError::Cancelled(reason.clone())),
                    Err(JoinError::Panicked(payload)) => Err(JoinError::Panicked(payload.clone())),
                    Err(other) => return Err(format!("supervisor join gave {other:?}")),
                };
                s.raw_outcomes.insert(name.to_string(), outcome);
                s.observations.len()
            };
            observe(&ctx.shared, me, idx, op, status, Value::Null);
            if let Ok(report) = result {
                lock(&ctx.shared).supervisor_values.push((index, report));
            }
        }
        "join" => {
            let target = str_field(step, "task")?;
            if lock(&ctx.shared).unspawned.contains(target) {
                observe(&ctx.shared, me, idx, op, NOT_HELD, Value::Null);
                return Ok(Flow::Continue);
            }
            let (handle, denied) = {
                let mut s = lock(&ctx.shared);
                (s.handles.remove(target), s.denied.remove(target))
            };
            let mut handle =
                handle.ok_or_else(|| format!("unknown or already joined task {target:?}"))?;
            let result = match denied {
                Some(result) => result,
                None => handle.join(cx).await,
            };
            observe_join(&mut lock(&ctx.shared), me, idx, op, target, result);
        }
        "open_region" => {
            let name = str_field(step, "as")?.to_string();
            let mut spec = ChildRegionSpec::inherit();
            if step.get("budget").is_some() {
                spec = spec.with_budget(budget(step.get("budget"))?);
            }
            // The envelope and requirements of the spec (child_region.rs:55);
            // the request meets the Cx's own into them (cx.rs:4514-4516).
            if step.get("capability_budget").is_some_and(|v| !v.is_null()) {
                spec.capability_budget = Some(capability_budget(step.get("capability_budget"))?);
            }
            spec.requirements = capability_requirements(step.get("requirements"))?;
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
                Err(e) => {
                    let value = match &e {
                        asupersync::cx::ChildRegionError::Create(
                            asupersync::runtime::region_table::RegionCreateError::CapabilityBudgetRefused {
                                reason,
                                ..
                            },
                        ) => capability_refusal_json(reason),
                        _ => Value::Null,
                    };
                    observe(&ctx.shared, me, idx, op, &child_region_status(&e)?, value);
                }
            }
        }
        "close_region" => {
            let name = str_field(step, "region")?;
            let Some(region) = local.regions.remove(name) else {
                observe(&ctx.shared, me, idx, op, NOT_HELD, Value::Null);
                return Ok(Flow::Continue);
            };
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

/// One blocking sync wait (DSL §3.7), observed under its own op: the steps
/// the task body runs directly and the one a `timeout` wraps. Its own
/// function, not `exec_step` again: a recursive `exec_step` is an async
/// recursion whose `Send` check overflows.
async fn exec_sync_wait(
    cx: &Cx,
    ctx: &Arc<TaskCtx>,
    local: &mut Local,
    idx: usize,
    step: &Value,
) -> RunResult<()> {
    let op = str_field(step, "op")?;
    let me = ctx.me.as_str();
    match op {
        "mutex_lock" => {
            let name = str_field(step, "mutex")?.to_string();
            let SyncObj::Mutex(m) = sync_obj(&ctx.shared, step, "mutex")? else {
                return Err(format!("{name:?} is not a mutex"));
            };
            let status = match asx_sync::OwnedMutexGuard::lock(m, cx).await {
                Ok(guard) => {
                    local.guards.insert(name, guard);
                    "ASX_OK"
                }
                Err(e) => lock_error_status(e),
            };
            observe(&ctx.shared, me, idx, op, status, Value::Null);
        }
        "rwlock_read" | "rwlock_write" => {
            let name = str_field(step, "rwlock")?.to_string();
            let SyncObj::RwLock(l) = sync_obj(&ctx.shared, step, "rwlock")? else {
                return Err(format!("{name:?} is not an rwlock"));
            };
            let acquired = if op == "rwlock_read" {
                asx_sync::OwnedRwLockReadGuard::read(l, cx)
                    .await
                    .map(RwGuard::Read)
            } else {
                asx_sync::OwnedRwLockWriteGuard::write(l, cx)
                    .await
                    .map(RwGuard::Write)
            };
            let status = match acquired {
                Ok(guard) => {
                    local.rw_guards.entry(name).or_default().push(guard);
                    "ASX_OK"
                }
                Err(e) => rwlock_error_status(e),
            };
            observe(&ctx.shared, me, idx, op, status, Value::Null);
        }
        "sem_acquire" => {
            let name = str_field(step, "semaphore")?.to_string();
            let SyncObj::Semaphore(s) = sync_obj(&ctx.shared, step, "semaphore")? else {
                return Err(format!("{name:?} is not a semaphore"));
            };
            let count =
                usize::try_from(u64_field(step, "count")?).map_err(|_| "count out of range")?;
            let status = match s.acquire(cx, count).await {
                Ok(permit) => {
                    local.permits.entry(name).or_default().push(permit);
                    "ASX_OK"
                }
                Err(e) => acquire_error_status(e),
            };
            observe(&ctx.shared, me, idx, op, status, Value::Null);
        }
        "barrier_wait" => {
            let SyncObj::Barrier(b) = sync_obj(&ctx.shared, step, "barrier")? else {
                return Err("barrier_wait on a non-barrier".to_string());
            };
            let status = match b.wait(cx).await {
                Ok(_) => "ASX_OK",
                Err(e) => barrier_error_status(e),
            };
            observe(&ctx.shared, me, idx, op, status, Value::Null);
        }
        "notify_wait" => {
            let SyncObj::Notify(n) = sync_obj(&ctx.shared, step, "notify")? else {
                return Err("notify_wait on a non-notify".to_string());
            };
            // Not cancel-aware (DSL §3.7): a cancelled waiter stays parked
            // until notified.
            n.notified().await;
            observe(&ctx.shared, me, idx, op, "ASX_OK", Value::Null);
        }
        other => return Err(format!("{other:?} is not a blocking sync step")),
    }
    Ok(())
}

/// Name of member `i` (0-based) of the group step at 1-based index `step`
/// of `owner`'s program (vocabulary §2).
fn member_name(owner: &str, step: usize, i: usize) -> String {
    format!("{owner}/g{step}.{}", i + 1)
}

fn group_programs(step: &Value) -> RunResult<Vec<Vec<Value>>> {
    step.get("members")
        .and_then(Value::as_array)
        .ok_or("group without members")?
        .iter()
        .map(|m| {
            m.as_array()
                .cloned()
                .ok_or_else(|| "group member is not a program".to_string())
        })
        .collect()
}

/// Spawn a group's members with `cx.spawn`, in member order, named for the
/// trace; they stay out of the snapshot (`Shared::group_members`).
fn spawn_members(
    cx: &Cx,
    ctx: &Arc<TaskCtx>,
    step_idx: usize,
    step: &Value,
) -> RunResult<Vec<TaskHandle<Body>>> {
    let mut handles = Vec::new();
    for (i, program) in group_programs(step)?.into_iter().enumerate() {
        let name = member_name(&ctx.me, step_idx, i);
        let child_ctx = Arc::new(TaskCtx {
            shared: ctx.shared.clone(),
            me: name.clone(),
            region: ctx.region.clone(),
            tokens: Arc::default(),
        });
        let handle = cx
            .spawn(move |ccx: Cx| run_program(ccx, child_ctx, program, Vec::new()))
            .map_err(|e| format!("group member spawn failed: {e:?}"))?;
        let mut s = lock(&ctx.shared);
        s.provisional.insert(handle.task_id(), name.clone());
        s.task_regions.insert(name.clone(), ctx.region.clone());
        s.group_members.insert(name);
        handles.push(handle);
    }
    Ok(handles)
}

/// A `race` with `deadline_ns` (DSL §3.5), as Rust's `Scope::timeout`
/// (cx/scope.rs:2130) runs it: the race runs in a wrapper task spawned into
/// the owner's region, which names itself `{owner}/g{step}.timeout`; its
/// members keep their plain-race names. Like the members, the wrapper is
/// internal to the combinator: the trace shows it, the snapshot does not
/// list it. At the deadline the owner aborts
/// the wrapper with `CancelReason::timeout()` and joins it. `None` is
/// `TimedResult::TimedOut`: the wrapper never acknowledged the
/// cancellation.
async fn race_with_deadline(
    cx: &Cx,
    ctx: &Arc<TaskCtx>,
    local: &mut Local,
    idx: usize,
    step: &Value,
    deadline_ns: u64,
) -> RunResult<Option<GroupValue>> {
    use asupersync::Outcome;
    use asupersync::combinator::timeout::TimedResult;

    let wrapper = Arc::new(TaskCtx {
        shared: ctx.shared.clone(),
        me: format!("{}/g{idx}.timeout", ctx.me),
        region: ctx.region.clone(),
        tokens: Arc::default(),
    });
    let owner = ctx.clone();
    let race = step.clone();
    let operation = move |wcx: Cx| {
        {
            let mut s = lock(&wrapper.shared);
            let id = wcx.task_id();
            s.task_ids.insert(wrapper.me.clone(), id);
            s.task_names.insert(id, wrapper.me.clone());
            s.task_regions
                .insert(wrapper.me.clone(), wrapper.region.clone());
            s.group_members.insert(wrapper.me.clone());
        }
        async move {
            match spawn_members(&wcx, &owner, idx, &race) {
                Ok(handles) => wcx.scope().race_all(&wcx, handles).await,
                Err(e) => {
                    // The recorded harness error fails the run.
                    harness_error(
                        &owner.shared,
                        format!("race member spawn in the timeout wrapper: {e}"),
                    );
                    Err(JoinError::Cancelled(CancelReason::user(
                        "race member spawn failed",
                    )))
                }
            }
        }
    };
    let timed = NamedSleep {
        sleep: Some(Box::pin(cx.scope().timeout(
            cx,
            Duration::from_nanos(deadline_ns),
            operation,
        ))),
        buffer: cx.trace_buffer(),
        shared: ctx.shared.clone(),
        owner: &ctx.me,
        counter: &mut local.timers,
        name: None,
    }
    .await
    .map_err(|e| format!("race timeout wrapper spawn refused: {e:?}"))?;
    let outcome = match timed {
        TimedResult::TimedOut(_) => return Ok(None),
        TimedResult::Completed(outcome) => outcome,
    };
    // A failed race (or a wrapper that ended cancelled or panicked) reports
    // its result without a winner index, as a plain race does.
    Ok(Some(match outcome {
        Outcome::Ok((body, winner)) => GroupValue::Race(Some(winner), Ok(body)),
        Outcome::Err(e) => GroupValue::Race(None, Err(e)),
        Outcome::Cancelled(reason) => GroupValue::Race(None, Err(JoinError::Cancelled(reason))),
        Outcome::Panicked(p) => GroupValue::Race(None, Err(JoinError::Panicked(p))),
    }))
}

/// A sleep that names the Timer events it records. Those events carry no
/// task, and their timer id is a wheel slab index that is reused once the
/// timer fires (time/wheel.rs:554): a sleep records its TimerFired when it
/// is next polled (time/sleep.rs:688), by which time another sleep may have
/// been scheduled under the same id. So every Timer event is named by the
/// sleep that records it, the one being polled or dropped at that moment
/// (worker_count 1): its first TimerScheduled takes the owner's next timer
/// number (a sleep that schedules none takes no number, as C counts them),
/// and a re-arm, the fire and a cancel keep that name. A `race` with a
/// deadline wraps its `Scope::timeout` future the same way: the only timer
/// that future records is its deadline sleep (cx/scope.rs:2146).
struct NamedSleep<'a, F> {
    sleep: Option<Pin<Box<F>>>,
    buffer: Option<asupersync::trace::TraceBufferHandle>,
    shared: SharedRef,
    owner: &'a str,
    counter: &'a mut u32,
    name: Option<String>,
}

impl<F> NamedSleep<'_, F> {
    fn last_seq(&self) -> Option<u64> {
        self.buffer.as_ref()?.snapshot().last().map(|e| e.seq)
    }

    /// Name the Timer events recorded after `after`.
    fn name_events(&mut self, after: Option<u64>) {
        let Some(events) = self.buffer.as_ref().map(|b| b.snapshot()) else {
            return;
        };
        let mut s = lock(&self.shared);
        for event in events.iter().filter(|e| after.is_none_or(|a| e.seq > a)) {
            if !matches!(event.data, TraceData::Timer { .. }) {
                continue;
            }
            let name = match (&event.kind, &self.name) {
                (_, Some(name)) => name.clone(),
                (TraceEventKind::TimerScheduled, None) => {
                    *self.counter += 1;
                    let name = format!("{}/tm{}", self.owner, *self.counter);
                    self.name = Some(name.clone());
                    name
                }
                // Fired or cancelled before this sleep scheduled anything:
                // not its event (project_trace fails it as unnamed).
                (_, None) => continue,
            };
            s.timer_names.insert(event.seq, name);
        }
    }
}

impl<F: Future> Future for NamedSleep<'_, F> {
    type Output = F::Output;

    fn poll(self: Pin<&mut Self>, pcx: &mut std::task::Context<'_>) -> std::task::Poll<F::Output> {
        let this = self.get_mut();
        let before = this.last_seq();
        // Awaited once, so never polled again after it completes.
        let Some(sleep) = this.sleep.as_mut() else {
            return std::task::Poll::Pending;
        };
        let polled = sleep.as_mut().poll(pcx);
        if polled.is_ready() {
            this.sleep = None;
        }
        this.name_events(before);
        polled
    }
}

impl<F> Drop for NamedSleep<'_, F> {
    /// A sleep dropped while pending (its task cancelled) records the
    /// cancel of its timer as it drops.
    fn drop(&mut self) {
        if let Some(sleep) = self.sleep.take() {
            let before = self.last_seq();
            drop(sleep);
            self.name_events(before);
        }
    }
}

/// `NamedSleep` for a future that sleeps more than once, one sleep at a
/// time: a supervisor's controller, whose only timers are its restart
/// backoffs (supervision.rs:2984-3013). Each new sleep takes the owner's
/// next timer number; a re-arm (the sleep cancels its timer and schedules
/// another in the same poll, time/sleep.rs:880-998), the fire and a cancel
/// keep that sleep's name.
struct NamedTimers<F> {
    inner: Pin<Box<F>>,
    buffer: Option<asupersync::trace::TraceBufferHandle>,
    shared: SharedRef,
    owner: String,
    counter: u32,
    /// The current sleep's timer id and name.
    active: Option<(u64, String)>,
}

impl<F> NamedTimers<F> {
    fn last_seq(&self) -> Option<u64> {
        self.buffer.as_ref()?.snapshot().last().map(|e| e.seq)
    }

    fn name_events(&mut self, after: Option<u64>) {
        let Some(events) = self.buffer.as_ref().map(|b| b.snapshot()) else {
            return;
        };
        let mut s = lock(&self.shared);
        // A sleep that cancelled its timer in this poll: the next schedule
        // is its re-arm.
        let mut rearming: Option<String> = None;
        for event in events.iter().filter(|e| after.is_none_or(|a| e.seq > a)) {
            let TraceData::Timer { timer_id, .. } = event.data else {
                continue;
            };
            let name = match event.kind {
                TraceEventKind::TimerScheduled => {
                    let name = rearming.take().unwrap_or_else(|| {
                        self.counter += 1;
                        format!("{}/tm{}", self.owner, self.counter)
                    });
                    self.active = Some((timer_id, name.clone()));
                    name
                }
                _ => match self.active.take() {
                    Some((id, name)) if id == timer_id => {
                        if event.kind == TraceEventKind::TimerCancelled {
                            rearming = Some(name.clone());
                        }
                        name
                    }
                    other => {
                        // Not this future's timer.
                        self.active = other;
                        continue;
                    }
                },
            };
            s.timer_names.insert(event.seq, name);
        }
    }
}

impl<F: Future> Future for NamedTimers<F> {
    type Output = F::Output;

    fn poll(self: Pin<&mut Self>, pcx: &mut std::task::Context<'_>) -> std::task::Poll<F::Output> {
        let this = self.get_mut();
        let before = this.last_seq();
        let polled = this.inner.as_mut().poll(pcx);
        this.name_events(before);
        polled
    }
}

// ---------------------------------------------------------------------------
// The driver
// ---------------------------------------------------------------------------

pub fn run_scenario(scenario: &Value) -> RunResult<Value> {
    run_scenario_with(scenario, None)
}

/// As [`run_scenario`], also appending the raw lab trace (seq, kind, data)
/// to `raw` for diagnosing a divergence (`twin_run trace`).
pub fn run_scenario_with(scenario: &Value, raw: Option<&mut Vec<String>>) -> RunResult<Value> {
    run_scenario_inner(scenario, raw, None)
}

/// The prefix of the error of a scenario whose forced replay is not
/// available (rather than refused).
pub const FORCED_UNAVAILABLE: &str = "forced replay unavailable: ";
/// The prefix of the error of an artifact Rust's `run_forced_schedule`
/// refused or could not decode.
pub const FORCED_REFUSED: &str = "forced replay refused: ";

/// bd-9kll.9.3, C to Rust: run the scenario on a fresh lab runtime whose
/// scheduler takes every dispatch from `artifact`, the canonical bytes of a
/// forced schedule the C runtime recorded for it
/// (`asx-conformance schedule`), through Rust's own `run_forced_schedule`:
/// it revalidates the seed and configuration, every task generation, worker,
/// lane, step and time, and the terminal certificate, and fails at the first
/// dispatch that is not the one the artifact says. Returns the fixture the
/// run projects, as `run_scenario` does, with the schedule section the
/// artifact's.
///
/// Rust's runner executes a whole schedule in one call, so a scenario whose
/// script steps the lab or changes its state between dispatches cannot be
/// replayed: its error starts with [`FORCED_UNAVAILABLE`]. A refusal starts
/// with [`FORCED_REFUSED`].
pub fn replay_forced_schedule(scenario: &Value, artifact: &[u8]) -> RunResult<Value> {
    run_scenario_inner(scenario, None, Some(artifact))
}

fn run_scenario_inner(
    scenario: &Value,
    raw: Option<&mut Vec<String>>,
    replay: Option<&[u8]>,
) -> RunResult<Value> {
    if scenario.get("schema").and_then(Value::as_str) != Some("asx.scenario.v2") {
        return Err("not an asx.scenario.v2 document".to_string());
    }
    if replay.is_some()
        && scenario
            .get("script")
            .and_then(Value::as_array)
            .is_some_and(|script| !script.is_empty())
    {
        return Err(format!(
            "{FORCED_UNAVAILABLE}the scenario's script runs the lab between dispatches, which \
             run_forced_schedule cannot interleave"
        ));
    }
    let lab_cfg = scenario.get("lab").ok_or("missing lab")?;
    let mut config = LabConfig::new(u64_field(scenario, "seed")?)
        .worker_count(1)
        .trace_capacity(1 << 20)
        .max_steps(u64_field(lab_cfg, "max_steps")?)
        .panic_on_leak(
            lab_cfg
                .get("panic_on_leak")
                .and_then(Value::as_bool)
                .unwrap_or(false),
        );
    // DSL §1: a declared futurelock threshold runs with the panic off, so the
    // FuturelockDetected events are the run's report; without it the lab
    // keeps its defaults (10000, panic on) and a futurelock fails the run.
    let futurelock_max_idle_steps = match lab_cfg.get("futurelock_max_idle_steps") {
        None => None,
        Some(v) => match v.as_u64() {
            Some(n) if n >= 1 => Some(n),
            _ => return Err("lab.futurelock_max_idle_steps must be an integer >= 1".to_string()),
        },
    };
    if let Some(n) = futurelock_max_idle_steps {
        config = config
            .futurelock_max_idle_steps(n)
            .panic_on_futurelock(false);
    }
    let mut lab = LabRuntime::new(config);
    if replay.is_none() {
        lab.start_forced_schedule_recording(1 << 20)
            .map_err(|e| format!("{e:?}"))?;
    }
    let shared: SharedRef = Arc::new(Mutex::new(Shared::default()));
    lock(&shared).futurelock_declared = futurelock_max_idle_steps.is_some();
    // Only a handoff's destination needs its Cx held (to try_transfer to
    // it). Holding any other task's would keep its CxInner alive after it
    // completes, so an abort of the finished task would still queue a
    // cancel command and spend a lab step a plain program does not.
    lock(&shared).handoff_destinations = handoff_destinations(scenario);

    // Channels (DSL §3.6): each endpoint goes to its owner's body.
    let mut endpoints: HashMap<String, Vec<(String, Endpoint)>> = HashMap::new();
    for decl in scenario
        .get("channels")
        .and_then(Value::as_array)
        .into_iter()
        .flatten()
    {
        let name = str_field(decl, "name")?.to_string();
        let mut give = |owner: &str, endpoint: Endpoint| {
            endpoints
                .entry(owner.to_string())
                .or_default()
                .push((name.clone(), endpoint));
        };
        // Broadcast and watch: the first subscriber takes the channel's own
        // receiver, the others subscribe at setup, before any send.
        let subscribers = || -> RunResult<Vec<String>> {
            let list = decl
                .get("subscribers")
                .and_then(Value::as_array)
                .filter(|a| !a.is_empty())
                .ok_or("broadcast/watch channel without subscribers")?;
            list.iter()
                .map(|v| {
                    v.as_str()
                        .map(str::to_string)
                        .ok_or_else(|| "subscriber is not a task name".to_string())
                })
                .collect()
        };
        let capacity = || -> RunResult<usize> {
            usize::try_from(u64_field(decl, "capacity")?)
                .map_err(|_| "capacity out of range".to_string())
        };
        // An mpsc's optional `senders`: more tasks, each given a clone of the
        // sender at setup (Sender::clone, mpsc.rs:1231), distinct from the
        // declared sender and from each other.
        let extra_senders = || -> RunResult<Vec<String>> {
            let Some(list) = decl.get("senders") else {
                return Ok(Vec::new());
            };
            let list = list.as_array().ok_or("senders is not a list")?;
            let declared = str_field(decl, "sender")?;
            let mut names: Vec<String> = Vec::new();
            for v in list {
                let n = v.as_str().ok_or("a sender is not a task name")?;
                if n == declared || names.iter().any(|m| m == n) {
                    return Err(format!("sender {n:?} listed twice"));
                }
                names.push(n.to_string());
            }
            Ok(names)
        };
        if decl.get("senders").is_some()
            && !matches!(str_field(decl, "type")?, "mpsc" | "broadcast")
        {
            return Err("only an mpsc or a broadcast channel takes senders".to_string());
        }
        if decl.get("unbounded").is_some() && str_field(decl, "type")? != "mpsc" {
            return Err("only an mpsc channel can be unbounded".to_string());
        }
        match str_field(decl, "type")? {
            "mpsc" => {
                // `"unbounded": true` is mpsc::unbounded_channel() (mpsc.rs:599),
                // which is channel(usize::MAX): the same Sender and Receiver, so
                // every mpsc step applies and a send never waits.
                let unbounded = match decl.get("unbounded") {
                    None => false,
                    Some(Value::Bool(true)) => true,
                    Some(_) => return Err("unbounded is not the constant true".to_string()),
                };
                let slots = if unbounded {
                    if decl.get("capacity").is_some() {
                        return Err("an unbounded mpsc channel has no capacity".to_string());
                    }
                    lock(&shared).unbounded_channels.insert(name.clone());
                    usize::MAX
                } else {
                    capacity()?
                };
                let (tx, rx) = mpsc::channel::<u64>(slots);
                for extra in extra_senders()? {
                    give(&extra, Endpoint::MpscTx(tx.clone()));
                }
                give(str_field(decl, "sender")?, Endpoint::MpscTx(tx));
                give(str_field(decl, "receiver")?, Endpoint::MpscRx(rx));
            }
            "oneshot" => {
                let (tx, rx) = oneshot::channel::<u64>();
                give(str_field(decl, "sender")?, Endpoint::OneshotTx(tx));
                give(str_field(decl, "receiver")?, Endpoint::OneshotRx(rx));
            }
            "broadcast" => {
                let (tx, rx) = broadcast::channel::<u64>(capacity()?);
                let subs = subscribers()?;
                for sub in &subs[1..] {
                    give(sub, Endpoint::BroadcastRx(tx.subscribe()));
                }
                give(&subs[0], Endpoint::BroadcastRx(rx));
                for extra in extra_senders()? {
                    give(&extra, Endpoint::BroadcastTx(tx.clone()));
                }
                give(str_field(decl, "sender")?, Endpoint::BroadcastTx(tx));
            }
            "watch" => {
                let (tx, rx) = watch::channel::<u64>(u64_field(decl, "initial")?);
                let subs = subscribers()?;
                for sub in &subs[1..] {
                    give(sub, Endpoint::WatchRx(tx.subscribe()));
                }
                give(&subs[0], Endpoint::WatchRx(rx));
                give(str_field(decl, "sender")?, Endpoint::WatchTx(tx));
            }
            other => return Err(format!("unknown channel type {other:?}")),
        }
    }
    // Sync objects (DSL §3.7), leaked for the capture's lifetime.
    for decl in scenario
        .get("sync")
        .and_then(Value::as_array)
        .into_iter()
        .flatten()
    {
        let name = str_field(decl, "name")?.to_string();
        let count = |field: &str| -> RunResult<usize> {
            usize::try_from(u64_field(decl, field)?).map_err(|_| format!("{field} out of range"))
        };
        let obj = match str_field(decl, "type")? {
            "mutex" => SyncObj::Mutex(Arc::new(asx_sync::Mutex::new(()))),
            "rwlock" => SyncObj::RwLock(Arc::new(asx_sync::RwLock::new(()))),
            "semaphore" => SyncObj::Semaphore(Box::leak(Box::new(asx_sync::Semaphore::new(
                count("permits")?,
            )))),
            "barrier" => SyncObj::Barrier(Box::leak(Box::new(asx_sync::Barrier::new(count(
                "parties",
            )?)))),
            "notify" => SyncObj::Notify(Box::leak(Box::new(asx_sync::Notify::new()))),
            other => return Err(format!("unknown sync type {other:?}")),
        };
        if lock(&shared).sync.insert(name.clone(), obj).is_some() {
            return Err(format!("duplicate sync object {name:?}"));
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
        // Async finalizers (DSL "Region finalizers"): registered in array
        // order right after the region exists and before any task, so the
        // last declared runs first. A finalizer is an ordinary task program
        // run by a future that takes no Cx: the lab installs the polled
        // task's, which the future reads on its first poll and where it
        // records the finalizer's task under its declared name (the
        // runtime creates that task, so no spawn pairs it with a name).
        for f in r
            .get("finalizers")
            .and_then(Value::as_array)
            .into_iter()
            .flatten()
        {
            let fname = str_field(f, "name")?.to_string();
            let program = f
                .get("program")
                .and_then(Value::as_array)
                .cloned()
                .ok_or("finalizer without program")?;
            let ctx = Arc::new(TaskCtx {
                shared: shared.clone(),
                me: fname.clone(),
                region: name.clone(),
                tokens: Arc::default(),
            });
            {
                let mut s = lock(&shared);
                s.task_regions.insert(fname.clone(), name.clone());
                s.obligation_tokens
                    .insert(fname.clone(), Arc::downgrade(&ctx.tokens));
            }
            // A sync finalizer (`"sync": true`, Scope::defer_sync) is a
            // closure over the non-blocking steps: the lab promotes it to
            // the same kind of cleanup task as an async one, which polls it
            // once, so it never suspends.
            let sync = f.get("sync").and_then(Value::as_bool).unwrap_or(false);
            let begin = move |ctx: &TaskCtx| -> Option<Cx> {
                let Some(cx) = Cx::current() else {
                    harness_error(&ctx.shared, format!("finalizer {}: no Cx", ctx.me));
                    return None;
                };
                let mut s = lock(&ctx.shared);
                let task = cx.task_id();
                s.self_ids.entry(ctx.me.clone()).or_insert(task);
                s.task_ids.insert(ctx.me.clone(), task);
                s.task_names.insert(task, ctx.me.clone());
                drop(s);
                Some(cx)
            };
            // The finalizer's task is masked for its whole run, so its
            // outcome is its body's: a finalizer future returns `()`.
            let registered = if sync {
                lab.state.register_sync_finalizer(id, move || {
                    let Some(cx) = begin(&ctx) else { return };
                    let body = run_sync_program(cx, &ctx, &program);
                    lock(&ctx.shared)
                        .raw_outcomes
                        .insert(ctx.me.clone(), Ok(body));
                })
            } else {
                lab.state.register_async_finalizer(id, async move {
                    let Some(cx) = begin(&ctx) else { return };
                    let body = run_program(cx, Arc::clone(&ctx), program, Vec::new()).await;
                    lock(&ctx.shared)
                        .raw_outcomes
                        .insert(ctx.me.clone(), Ok(body));
                })
            };
            if !registered {
                return Err(format!(
                    "finalizer {fname:?} was refused by region {name:?} (closing or closed)"
                ));
            }
        }
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
            tokens: Arc::default(),
        });
        lock(&shared)
            .obligation_tokens
            .insert(name.clone(), Arc::downgrade(&ctx.tokens));
        let mine = endpoints.remove(&name).unwrap_or_default();
        let body = async move {
            match Cx::current() {
                Some(cx) => run_program(cx, ctx, program, mine).await,
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
        let task_cx = lab
            .state
            .task(task_id)
            .and_then(|record| record.cx.clone())
            .ok_or_else(|| format!("task {name} has no runtime context"))?;
        lab.scheduler.lock().schedule(task_id, 0);
        let mut s = lock(&shared);
        if s.handoff_destinations.contains(&name) {
            s.task_contexts.insert(name.clone(), task_cx);
        }
        s.task_ids.insert(name.clone(), task_id);
        s.task_names.insert(task_id, name.clone());
        s.handles.insert(name, handle);
    }

    if let Some(owner) = endpoints.keys().next() {
        return Err(format!(
            "channel endpoint owner {owner:?} is not a declared top-level task"
        ));
    }

    // Script: at each entry, run to idle, advance time, apply the op.
    for (entry_index, entry) in scenario
        .get("script")
        .and_then(Value::as_array)
        .into_iter()
        .flatten()
        .enumerate()
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
            entry_index + 1,
        )?;
    }

    // Finish: run to quiescence with timer auto-advance, or (bd-9kll.9.3)
    // replay the schedule the C runtime recorded, exactly.
    let mut step_limit_reached = false;
    let mut replayed = None;
    if let Some(bytes) = replay {
        let max_steps = u64_field(lab_cfg, "max_steps")?;
        let decode = ForcedScheduleDecodeLimits::new(bytes.len().max(1), 1 << 20, 1 << 26);
        let artifact = ForcedSchedule::try_from_canonical_bytes(bytes, decode)
            .map_err(|e| format!("{FORCED_REFUSED}the artifact does not decode: {e:?}"))?;
        let limits = ForcedScheduleLimits::new(artifact.dispatches().len().max(1), max_steps);
        lab.run_forced_schedule(&artifact, limits)
            .map_err(|e| format!("{FORCED_REFUSED}{e:?}"))?;
        replayed = Some(artifact);
    } else {
        let report = lab.run_with_auto_advance();
        step_limit_reached = matches!(
            report.termination,
            asupersync::lab::AutoAdvanceTermination::StepLimitReached
        );
    }
    // Before anything can fail the run: a failing scenario is the one to
    // diagnose.
    if let Some(raw) = raw {
        for e in lab.trace().snapshot().iter() {
            raw.push(format!("{} {:?} {:?}", e.seq, e.kind, e.data));
        }
    }

    let errors = std::mem::take(&mut lock(&shared).errors);
    if !errors.is_empty() {
        return Err(format!("harness errors: {}", errors.join("; ")));
    }
    // Exceeding lab.max_steps is a harness failure (DSL §2), as the C
    // interpreter reports it; the lab itself just stops there.
    if step_limit_reached {
        return Err("lab.max_steps exceeded: run_with_auto_advance".to_string());
    }

    let schedule = match replayed {
        Some(artifact) => artifact,
        None => lab
            .finish_forced_schedule_recording()
            .map_err(|e| format!("{e:?}"))?,
    };
    // A forced schedule only replays a run that reached quiescence (its
    // canonical form refuses a partial source, lab/runtime.rs:1587); a
    // scenario that ends with work stuck has none, and its snapshot says so.
    let forced_schedule = if schedule.terminal_quiescent() {
        let bytes = schedule
            .to_canonical_bytes()
            .map_err(|e| format!("{e:?}"))?;
        Value::String(bytes.iter().map(|b| format!("{b:02x}")).collect())
    } else {
        Value::Null
    };

    // Harvest the outcomes of tasks nobody joined.
    {
        let s = &mut *lock(&shared);
        let names: Vec<String> = s.handles.keys().cloned().collect();
        for name in names {
            if let Some(result) = s.denied.remove(&name) {
                s.handles.remove(&name);
                s.raw_outcomes.insert(name, result);
                continue;
            }
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
    name_supervision(&lab, &shared)?;
    harvest_servers(&lab, &mut lock(&shared));
    harvest_supervisors(&lab, &mut lock(&shared))?;
    project_denied_spawns(&mut lock(&shared))?;
    finish_outcomes(&mut lock(&shared))?;

    // The lab's dispatches in order, one "<step>@<ns> <task> <lane>" string
    // each; asx-conformance compares C's with them (bd-9kll.4.8).
    let dispatches = {
        let s = lock(&shared);
        let names: HashMap<u64, &String> = s
            .task_names
            .iter()
            .map(|(id, name)| (CompactTaskId::from(*id).0, name))
            .collect();
        schedule
            .dispatches()
            .iter()
            .map(|d| {
                let name = names.get(&d.task().0).ok_or_else(|| {
                    format!(
                        "dispatch of unnamed task {:?} (harness defect)",
                        d.task().unpack()
                    )
                })?;
                let lane = format!("{:?}", d.lane()).to_lowercase();
                Ok(Value::String(format!(
                    "{}@{} {name} {lane}",
                    d.at_step(),
                    d.at_nanos()
                )))
            })
            .collect::<RunResult<Vec<Value>>>()?
    };
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
    check_expectation(scenario, &observations, &events.events)?;

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
            "forced_schedule": forced_schedule,
            "dispatches": dispatches,
        },
    }))
}

/// One script entry's operation; `step` is the entry's 1-based index, the
/// step number of an observation a driver operation makes (task `driver`).
fn apply_driver_op(
    lab: &mut LabRuntime,
    shared: &SharedRef,
    op: &Value,
    step: usize,
) -> RunResult<()> {
    let region_id = |field: &str| -> RunResult<RegionId> {
        let name = str_field(op, field)?;
        lock(shared)
            .region_ids
            .get(name)
            .copied()
            .ok_or_else(|| format!("unknown region {name:?}"))
    };
    match str_field(op, "op")? {
        // The aggregated close outcome of a live or recently closed region
        // (RuntimeState::region_close_outcome, state.rs:3972), observed
        // under the task name `driver`: none yet is ASX_E_NOT_FOUND.
        "region_close_outcome" => {
            let region = region_id("region")?;
            // A reason's origin task may be one that was admitted and has not
            // run: name the admissions first, as `cancel_task` does
            // (bd-9kll.2.10: gen-9-14, an unnamed TaskId(3:0)).
            resolve_admissions(lab, shared)?;
            let (status, value) = match lab.state.region_close_outcome(region) {
                None => ("ASX_E_NOT_FOUND", Value::Null),
                Some(outcome) => {
                    let s = lock(shared);
                    let value = match outcome {
                        asupersync::Outcome::Ok(()) => json!({"tag": "ok"}),
                        asupersync::Outcome::Err(error) => {
                            json!({"tag": "err", "status": status_of_error(&error)?})
                        }
                        asupersync::Outcome::Cancelled(reason) => {
                            json!({"tag": "cancelled", "reason": project_reason(&s, &reason)?})
                        }
                        asupersync::Outcome::Panicked(payload) => {
                            json!({"tag": "panicked", "message": payload.message()})
                        }
                    };
                    ("ASX_OK", value)
                }
            };
            observe(
                shared,
                "driver",
                step,
                "region_close_outcome",
                status,
                value,
            );
        }
        "finish_region" => {
            let region = region_id("region")?;
            // Execute Scope's body-return branch against the real runtime.
            // This primitive witness does not execute a typed Scope factory
            // or project its returned body payload as a child outcome.
            let reason = match str_field(op, "body")? {
                "ok" => None,
                "cancelled" => Some(cancel_reason(op, region, None, lab.now())?),
                "err" | "panicked" => Some(CancelReason::fail_fast().with_region(region)),
                other => return Err(format!("unknown region body outcome {other:?}")),
            };
            if let Some(reason) = reason {
                let (tasks, wakes) = lab.state.cancel_request(region, &reason, None).into_parts();
                for (task, priority) in tasks {
                    lab.scheduler.lock().schedule_cancel(task, priority);
                }
                wakes.dispatch();
            }
            if let Some(record) = lab.state.region(region) {
                record.begin_close(None);
            }
            lab.state.advance_region_state(region);
        }
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
        "region_limits" => {
            // Admission limits on live tasks, live child regions and
            // pending obligations (record/region.rs:208); an omitted or
            // null field is unlimited.
            let region = region_id("region")?;
            let limit = |field: &str| -> RunResult<Option<usize>> {
                match op.get(field) {
                    None | Some(Value::Null) => Ok(None),
                    Some(v) => v
                        .as_u64()
                        .and_then(|n| usize::try_from(n).ok())
                        .map(Some)
                        .ok_or_else(|| format!("region_limits: {field} must be a count")),
                }
            };
            let limits = RegionLimits {
                max_children: limit("max_children")?,
                max_tasks: limit("max_tasks")?,
                max_obligations: limit("max_obligations")?,
                ..RegionLimits::UNLIMITED
            };
            // A region already gone has nothing left to limit (DSL §4;
            // fuzz gen-129-90, gen-136-92, gen-116-186).
            let _applied = lab.state.set_region_limits(region, limits);
        }
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

/// The outcome of each completed server no `server_stop` joined, as its join
/// would report it (vocabulary §6): an admitted server always runs, so it
/// publishes its final state (ok) unless it panicked. Its task's own
/// outcome is not observable (`GenServerHandle` keeps the task handle).
/// A record the lab already reaped (its region closed) belongs to a
/// finished server; the built-in behaviors cannot panic (every call is
/// replied to), so it joins ok.
fn harvest_servers(lab: &LabRuntime, s: &mut Shared) {
    let mut names: Vec<String> = s.servers.iter().cloned().collect();
    names.sort();
    for name in names {
        if s.raw_outcomes.contains_key(&name) {
            continue;
        }
        let Some(id) = s.task_ids.get(&name) else {
            continue; // never admitted: not in the snapshot
        };
        let result = match lab.state.task(*id).map(|record| &record.state) {
            Some(TaskState::Completed(asupersync::Outcome::Panicked(payload))) => {
                Err(JoinError::Panicked(payload.clone()))
            }
            Some(TaskState::Completed(_)) | None => Ok(Body::Ok),
            Some(_) => continue, // still running
        };
        s.raw_outcomes.insert(name, result);
    }
}

/// The outcome of each supervisor nobody joined: its join's (vocabulary
/// §6). A controller that published its report is `ok`; one that never ran
/// is its task's own (cancelled before its first poll).
fn harvest_supervisors(lab: &LabRuntime, s: &mut Shared) -> RunResult<()> {
    let supervisors: Vec<(String, ReportSlot)> = s
        .supervisors
        .iter()
        .map(|i| (i.name.clone(), Arc::clone(&i.report)))
        .collect();
    for (name, report) in supervisors {
        if s.raw_outcomes.contains_key(&name) {
            continue;
        }
        let Some(id) = s.task_ids.get(&name).copied() else {
            continue; // never admitted: not in the snapshot
        };
        if lock_report(&report).is_some() {
            s.raw_outcomes.insert(name, Ok(Body::Ok));
            continue;
        }
        if let Some(mut task) = s.dropped_supervisors.remove(&name) {
            match task.try_join() {
                Ok(Some(())) => {
                    return Err(format!("supervisor {name:?} finished without its report"));
                }
                Ok(None) => {}
                Err(error) => {
                    s.raw_outcomes.insert(name, Err(error));
                }
            }
            continue;
        }
        let result = match lab.state.task(id).map(|record| &record.state) {
            Some(TaskState::Completed(asupersync::Outcome::Panicked(payload))) => {
                Err(JoinError::Panicked(payload.clone()))
            }
            Some(TaskState::Completed(asupersync::Outcome::Cancelled(reason))) => {
                Err(JoinError::Cancelled(reason.clone()))
            }
            Some(TaskState::Completed(_)) => {
                return Err(format!("supervisor {name:?} finished without its report"));
            }
            Some(_) => continue, // still running
            None => return Err(format!("supervisor {name:?} finished out of sight")),
        };
        s.raw_outcomes.insert(name, result);
    }
    Ok(())
}

/// One `managed_supervisor_v1` user trace of a supervisor's controller
/// (supervision.rs:2458-2480), split into its fields. Its region and task
/// are raw ids; the projection replaces them with names.
struct SupervisorTrace<'a> {
    action: &'a str,
    supervisor: &'a str,
    child: &'a str,
    generation: u64,
    region: &'a str,
    task: &'a str,
    outcome: &'a str,
}

const SUPERVISOR_TRACE: &str = "managed_supervisor_v1 ";

/// Every region and task id the trace mentions, by its Debug spelling,
/// with each created region's parent and the first task enqueued and the
/// first admitted in each region (a generation's region gets its
/// generation's task first, before anything that task spawns).
#[derive(Default)]
struct IdsSeen {
    regions: HashMap<String, RegionId>,
    parents: HashMap<RegionId, Option<RegionId>>,
    created: Vec<RegionId>,
    tasks: HashMap<String, TaskId>,
    first_in: HashMap<RegionId, Vec<TaskId>>,
    /// The canonical id of every task a region spawned (a direct spawn or
    /// an admission, not a provisional mailbox id), in trace order.
    canonical_in: HashMap<RegionId, Vec<TaskId>>,
}

impl IdsSeen {
    fn from_trace(lab: &LabRuntime) -> Self {
        Self::from_events(&lab.trace().snapshot())
    }

    fn from_events(events: &[TraceEvent]) -> Self {
        use TraceEventKind as K;
        let mut ids = Self::default();
        for event in events {
            match (&event.kind, &event.data) {
                (K::RegionCreated, TraceData::Region { region, parent }) => {
                    ids.regions.insert(format!("{region:?}"), *region);
                    ids.parents.insert(*region, *parent);
                    ids.created.push(*region);
                }
                (
                    K::Spawn | K::TaskSpawnEnqueued | K::TaskAdmitted,
                    TraceData::Task { task, region },
                ) => {
                    ids.tasks.insert(format!("{task:?}"), *task);
                    if matches!(event.kind, K::Spawn | K::TaskAdmitted) {
                        let all = ids.canonical_in.entry(*region).or_default();
                        if !all.contains(task) {
                            all.push(*task);
                        }
                    }
                    let first = ids.first_in.entry(*region).or_default();
                    // The first enqueue (its provisional id) and the first
                    // admission (its canonical id).
                    if first.len() < 2
                        && matches!(event.kind, K::TaskSpawnEnqueued | K::TaskAdmitted)
                        && !first.contains(task)
                    {
                        first.push(*task);
                    }
                }
                _ => {}
            }
        }
        ids
    }
}

fn parse_supervisor_trace(message: &str) -> RunResult<Option<SupervisorTrace<'_>>> {
    let Some(rest) = message.strip_prefix(SUPERVISOR_TRACE) else {
        return Ok(None);
    };
    let mut fields: HashMap<&str, &str> = HashMap::new();
    for field in rest.split(' ') {
        let (key, value) = field
            .split_once('=')
            .ok_or_else(|| format!("malformed supervisor trace {message:?}"))?;
        fields.insert(key, value);
    }
    let get = |key: &str| {
        fields
            .get(key)
            .copied()
            .ok_or_else(|| format!("supervisor trace without {key}: {message:?}"))
    };
    // ChildName's Debug is its str's: quoted, and DSL names need no escapes.
    let unquote = |key: &str| {
        get(key).and_then(|v| {
            v.strip_prefix('"')
                .and_then(|v| v.strip_suffix('"'))
                .ok_or_else(|| format!("unquoted {key} in supervisor trace {message:?}"))
        })
    };
    Ok(Some(SupervisorTrace {
        action: get("action")?,
        supervisor: unquote("supervisor")?,
        child: unquote("child")?,
        generation: get("generation")?
            .parse()
            .map_err(|_| format!("bad generation in {message:?}"))?,
        region: get("region")?,
        task: get("task")?,
        outcome: get("outcome")?,
    }))
}

/// Name what supervisors created (vocabulary §2) before anything is
/// projected. Each generation's region and task are
/// `<supervisor>/<child>#<n>`; the controller's traces carry every
/// generation's identity, so one whose factory never ran (cancelled before
/// its first poll) is named too, with the id its handle held. A
/// supervisor's region is the parent of its generations' regions. For one
/// that started no generation it is the supervisor's name when it is the
/// one unnamed region created under its controller's region and no other
/// such supervisor runs there; else the trace cannot tell whose is whose,
/// and every unnamed region created there is `<region>/supervisor<k>`, k in
/// creation order. A region opened for a generation that never started is
/// `<supervisor region>/unstarted<k>`.
fn name_supervision(lab: &LabRuntime, shared: &SharedRef) -> RunResult<()> {
    use TraceEventKind as K;
    let mut s = lock(shared);
    if s.supervisors.is_empty() {
        return Ok(());
    }
    let ids = IdsSeen::from_trace(lab);
    let mut generations: Vec<(String, String, String, String)> = Vec::new();
    for event in lab.trace().snapshot() {
        if let (K::UserTrace, TraceData::Message(message)) = (&event.kind, &event.data)
            && let Some(t) = parse_supervisor_trace(message)?
        {
            generations.push((
                t.supervisor.to_string(),
                format!("{}/{}#{}", t.supervisor, t.child, t.generation),
                t.region.to_string(),
                t.task.to_string(),
            ));
        }
    }
    let owner_regions: HashMap<String, String> = s
        .supervisors
        .iter()
        .map(|i| (i.name.clone(), i.region.clone()))
        .collect();
    for (supervisor, name, region_token, task_token) in generations {
        let region = *ids
            .regions
            .get(&region_token)
            .ok_or_else(|| format!("supervisor trace names an unknown {region_token}"))?;
        name_region(&mut s, region, &name)?;
        let parent = ids
            .parents
            .get(&region)
            .copied()
            .flatten()
            .ok_or_else(|| format!("generation region {name:?} has no parent"))?;
        name_region(&mut s, parent, &supervisor)?;
        s.region_parents
            .insert(name.clone(), Some(supervisor.clone()));
        let owner = owner_regions
            .get(&supervisor)
            .ok_or_else(|| format!("trace of an unknown supervisor {supervisor:?}"))?;
        s.region_parents
            .insert(supervisor.clone(), Some(owner.clone()));
        let mut tasks: Vec<TaskId> = ids.first_in.get(&region).cloned().unwrap_or_default();
        if let Some(task) = ids.tasks.get(&task_token) {
            tasks.push(*task);
        }
        for task in tasks {
            name_task(&mut s, task, &name)?;
        }
        s.supervised.insert(name.clone());
        s.task_regions.insert(name.clone(), name);
    }
    // Supervisors whose region holds no generation.
    let mut fallback: Vec<(RegionId, String)> = Vec::new();
    let unnamed: Vec<(String, String)> = s
        .supervisors
        .iter()
        .filter(|i| !s.region_ids.contains_key(&i.name))
        .map(|i| (i.name.clone(), i.region.clone()))
        .collect();
    for (supervisor, owner) in &unnamed {
        let Some(owner_id) = s.region_ids.get(owner).copied() else {
            continue;
        };
        let candidates: Vec<RegionId> = ids
            .created
            .iter()
            .copied()
            .filter(|r| ids.parents.get(r).copied().flatten() == Some(owner_id))
            .filter(|r| !s.region_names.contains_key(r))
            .collect();
        let siblings = unnamed.iter().filter(|(_, o)| o == owner).count();
        match candidates.as_slice() {
            [] => {}
            [region] if siblings == 1 => {
                name_region(&mut s, *region, supervisor)?;
                s.region_parents
                    .insert(supervisor.clone(), Some(owner.clone()));
            }
            _ => {
                // Several: the trace cannot tell whose is whose, so they are
                // named after their parent, in creation order.
                for (k, region) in candidates.iter().enumerate() {
                    let name = format!("{owner}/supervisor{}", k + 1);
                    name_region(&mut s, *region, &name)?;
                    s.region_parents.insert(name.clone(), Some(owner.clone()));
                    fallback.push((*region, name));
                }
            }
        }
    }
    // A region a controller opened for a generation it then did not start
    // (it was cancelled first) appears in no trace of it:
    // "<supervisor>/unstarted<k>", k in creation order.
    let supervisor_regions: HashMap<RegionId, String> = s
        .supervisors
        .iter()
        .filter_map(|i| s.region_ids.get(&i.name).map(|r| (*r, i.name.clone())))
        .chain(fallback)
        .collect();
    let mut unstarted: HashMap<String, u32> = HashMap::new();
    for region in &ids.created {
        if s.region_names.contains_key(region) {
            continue;
        }
        let Some(supervisor) = ids
            .parents
            .get(region)
            .copied()
            .flatten()
            .and_then(|p| supervisor_regions.get(&p))
        else {
            continue;
        };
        let k = unstarted.entry(supervisor.clone()).or_insert(0);
        *k += 1;
        let name = format!("{supervisor}/unstarted{k}");
        name_region(&mut s, *region, &name)?;
        s.region_parents.insert(name, Some(supervisor.clone()));
    }
    // The cleanup task of a registered generation's region (DSL §3.8): Rust
    // promotes the sync finalizer that retains the name to a masked task,
    // spawned when the region finalizes (state.rs:8612), which no spawn
    // names. It is "<region>/reg" for each region a supervisor opened, the
    // one task created there that is still unnamed, and like the
    // generation's own task it is left out of the snapshot.
    for region in &ids.created {
        let Some(parent) = ids.parents.get(region).copied().flatten() else {
            continue;
        };
        if !supervisor_regions.contains_key(&parent) {
            continue;
        }
        let Some(generation) = s.region_names.get(region).cloned() else {
            continue;
        };
        let unnamed: Vec<TaskId> = ids
            .canonical_in
            .get(region)
            .into_iter()
            .flatten()
            .copied()
            .filter(|task| !s.task_names.contains_key(task))
            .collect();
        let name = format!("{generation}/reg");
        match unnamed.as_slice() {
            [] => {}
            [task] => {
                if s.task_ids.contains_key(&name) || s.region_ids.contains_key(&name) {
                    return Err(format!(
                        "{name:?} is the retention task of {generation:?}, but the \
                         scenario created that name"
                    ));
                }
                name_task(&mut s, *task, &name)?;
                s.task_regions.insert(name.clone(), generation);
                s.supervised.insert(name);
            }
            _ => {
                return Err(format!(
                    "generation region {generation:?} spawned {} unnamed tasks",
                    unnamed.len()
                ));
            }
        }
    }
    Ok(())
}

fn name_region(s: &mut Shared, region: RegionId, name: &str) -> RunResult<()> {
    if let Some(other) = s.region_names.get(&region)
        && other != name
    {
        return Err(format!(
            "region {region:?} of {name:?} is already named {other:?}"
        ));
    }
    s.region_names.insert(region, name.to_string());
    s.region_ids.insert(name.to_string(), region);
    Ok(())
}

fn name_task(s: &mut Shared, task: TaskId, name: &str) -> RunResult<()> {
    if let Some(other) = s.task_names.get(&task)
        && other != name
    {
        return Err(format!(
            "task {task:?} of {name:?} is already named {other:?}"
        ));
    }
    s.task_names.insert(task, name.to_string());
    s.task_ids.entry(name.to_string()).or_insert(task);
    Ok(())
}

/// A supervisor's join report (DSL §3.8): its outcome, its counters and
/// each child's latest completed generation, in start order.
fn project_report(s: &Shared, report: ManagedSupervisorReport<String>) -> RunResult<Value> {
    use asupersync::Outcome;
    let outcome = match report.outcome {
        Outcome::Ok(()) => json!({"tag": "ok"}),
        Outcome::Err(error) => {
            let (kind, child) = match &error {
                ManagedSupervisorError::Region(_) => ("Region", None),
                ManagedSupervisorError::Spawn(_) => ("Spawn", None),
                ManagedSupervisorError::ChildNotStarted { child, .. } => {
                    ("ChildNotStarted", Some(child))
                }
                ManagedSupervisorError::DependencyUnavailable { child, .. } => {
                    ("DependencyUnavailable", Some(child))
                }
                ManagedSupervisorError::RestartLimit { child, .. } => ("RestartLimit", Some(child)),
                ManagedSupervisorError::Registration { child, .. } => ("Registration", Some(child)),
                ManagedSupervisorError::GenerationExhausted(child) => {
                    ("GenerationExhausted", Some(child))
                }
                ManagedSupervisorError::Cleanup { child, .. } => ("Cleanup", Some(child)),
                ManagedSupervisorError::SupervisorCleanup(_) => ("SupervisorCleanup", None),
                ManagedSupervisorError::Escalation(_) => ("Escalation", None),
                other => return Err(format!("supervisor error outside DSL v2: {other:?}")),
            };
            json!({"tag": "err", "error": kind, "child": child.map(|c| c.as_str())})
        }
        Outcome::Cancelled(reason) => {
            json!({"tag": "cancelled", "reason": project_reason(s, &reason)?})
        }
        Outcome::Panicked(payload) => json!({"tag": "panicked", "message": payload.message()}),
    };
    let mut children = Vec::with_capacity(report.children.len());
    for child in &report.children {
        let outcome = match &child.outcome {
            Outcome::Ok(()) => json!({"tag": "ok"}),
            Outcome::Err(status) => json!({"tag": "err", "status": status}),
            Outcome::Cancelled(reason) => {
                json!({"tag": "cancelled", "reason": project_reason(s, reason)?})
            }
            Outcome::Panicked(payload) => json!({"tag": "panicked", "message": payload.message()}),
        };
        children.push(json!({
            "child": child.name.as_str(),
            "generation": child.generation.number,
            "outcome": outcome,
        }));
    }
    let mut projected = json!({
        "outcome": outcome,
        "started": report.started,
        "joined": report.joined,
        "restart_batches": report.restart_batches,
        "escalations": report.escalations,
        "children": children,
    });
    // Only when set, so a supervisor without a storm threshold projects as it
    // always did (DSL §3.8).
    if report.restart_storm_detected {
        projected["restart_storm_detected"] = json!(true);
    }
    Ok(projected)
}

/// Name the canonical ids of children spawned through `cx.spawn`. The spawn
/// returns a handle carrying a provisional mailbox id; admission assigns the
/// canonical arena id every later trace event uses. The trace pairs the two:
/// `TaskSpawnEnqueued` (provisional) and `TaskAdmitted` (canonical) match in
/// per-region FIFO order (src/trace/event.rs, `task_admitted`). Idempotent:
/// it rescans the trace and moves each newly admitted child out of
/// `provisional`.
fn resolve_admissions(lab: &LabRuntime, shared: &SharedRef) -> RunResult<()> {
    use std::collections::VecDeque;
    let mut s = lock(shared);
    // A dropped supervisor's handle knows its controller's canonical id
    // once the controller was admitted, also when it never ran.
    let admitted: Vec<(String, TaskId)> = s
        .dropped_supervisors
        .iter()
        .filter_map(|(name, task)| {
            let id = task.task_id();
            s.supervisors
                .iter()
                .any(|i| i.name == *name && i.provisional != id)
                .then(|| (name.clone(), id))
        })
        .collect();
    for (name, id) in admitted {
        s.self_ids.entry(name).or_insert(id);
    }
    // A child that ran recorded its canonical id under its name
    // (run_program; a server from its hooks, `ServerName`): name it
    // directly. Enqueue order alone cannot pair a
    // region's spawns with its admissions once one was refused
    // (RegionAtCapacity under region_limits): the refused spawn has an
    // enqueue and no TaskAdmitted.
    let direct: Vec<(TaskId, String, TaskId)> = s
        .provisional
        .iter()
        .filter_map(|(p, n)| s.self_ids.get(n).map(|id| (*p, n.clone(), *id)))
        .collect();
    for (provisional, name, task) in direct {
        s.provisional.remove(&provisional);
        s.paired.insert(provisional);
        name_admitted(&mut s, name, task)?;
    }
    // Children that never ran: the enqueues and admissions not accounted
    // for, paired in per-region FIFO order (src/trace/event.rs,
    // `task_admitted`).
    let mut pending: HashMap<RegionId, VecDeque<TaskId>> = HashMap::new();
    for event in lab.trace().snapshot() {
        match (&event.kind, &event.data) {
            (TraceEventKind::TaskSpawnEnqueued, TraceData::Task { task, region }) => {
                if !s.paired.contains(task) {
                    pending.entry(*region).or_default().push_back(*task);
                }
            }
            (TraceEventKind::TaskAdmitted, TraceData::Task { task, region }) => {
                if s.task_names.contains_key(task) {
                    continue;
                }
                let provisional = pending
                    .get_mut(region)
                    .and_then(VecDeque::pop_front)
                    .ok_or_else(|| {
                        format!("TaskAdmitted {task:?} has no pending enqueue in {region:?}")
                    })?;
                if let Some(name) = s.provisional.remove(&provisional) {
                    s.paired.insert(provisional);
                    name_admitted(&mut s, name, *task)?;
                }
            }
            _ => {}
        }
    }
    Ok(())
}

fn name_admitted(s: &mut Shared, name: String, task: TaskId) -> RunResult<()> {
    if let Some(other) = s.task_names.get(&task)
        && *other != name
    {
        return Err(format!(
            "canonical {task:?} of {name:?} is already named {other:?}"
        ));
    }
    s.self_ids.entry(name.clone()).or_insert(task);
    s.task_ids.insert(name.clone(), task);
    s.task_names.insert(task, name);
    Ok(())
}

/// Spawns their region never admitted. Rust's `cx.spawn` accepts a child
/// into the spawn mailbox and admission later refuses it when the region is
/// closing or closed (`SpawnError::RegionClosed`, runtime/state.rs:1702) or
/// at its live-task limit (`SpawnError::RegionAtCapacity` under
/// `region_limits`), resolving the handle as cancelled
/// (spawn_mailbox.rs:1394-1417); the child never runs and has no trace
/// events. The refusal is projected onto the spawn step as
/// `ASX_E_REGION_CLOSED` or `ASX_E_ADMISSION_LIMIT`, and a join of the
/// child observes it too (DSL §3.4); C's interpreter projects its
/// admission refusals the same way (`asx_task_admission_status`). An
/// unadmitted spawn whose handle did not resolve as cancelled is a
/// scenario or harness defect.
fn project_denied_spawns(s: &mut Shared) -> RunResult<()> {
    let denied: Vec<String> = s.provisional.values().cloned().collect();
    for name in denied {
        if s.servers.contains(&name) || s.supervisors.iter().any(|i| i.name == name) {
            // A server or supervisor the lab never admitted: its spawn
            // returned a handle (the step observed ASX_OK), and a join
            // observed the refusal itself (Cancelled). Its task is not in
            // the snapshot.
            s.raw_outcomes.remove(&name);
            s.task_regions.remove(&name);
            continue;
        }
        if s.group_members.contains(&name) {
            // A combinator's member: the combinator joined it (Cancelled),
            // and its own observation reports that.
            s.task_regions.remove(&name);
            continue;
        }
        // A closed region resolves the handle Cancelled(ParentCancelled); any
        // other admission error resolves it Cancelled(User) with the
        // SpawnError's text as the message (lab/runtime.rs:3999-4007;
        // cx.rs:5528). RegionAtCapacity, under region_limits, is ASUP-E006.
        let status = match s.raw_outcomes.get(&name) {
            Some(Err(JoinError::Cancelled(reason))) => {
                if reason.kind == CancelKind::User
                    && reason
                        .message
                        .as_deref()
                        .is_some_and(|m| m.starts_with("[ASUP-E006]"))
                {
                    "ASX_E_ADMISSION_LIMIT"
                } else {
                    "ASX_E_REGION_CLOSED"
                }
            }
            _ => {
                return Err(format!(
                    "spawned task {name:?} was never admitted (scenario or harness defect)"
                ));
            }
        };
        s.raw_outcomes.remove(&name);
        let spawn_at = s.spawn_observations.get(&name).copied();
        let join_at = s
            .outcome_observations
            .iter()
            .filter(|(_, target)| *target == name)
            .map(|(i, _)| *i);
        for i in spawn_at.into_iter().chain(join_at).collect::<Vec<_>>() {
            s.observations[i]["status"] = json!(status);
            s.observations[i]["value"] = Value::Null;
        }
        s.outcome_observations.retain(|(_, target)| *target != name);
        s.task_regions.remove(&name);
    }
    s.provisional.clear();
    Ok(())
}

struct Projected {
    events: Vec<Value>,
    /// obligation name -> (kind, holder, region, last state, abort reason)
    obligations: Vec<(String, Value)>,
    timers_pending: Vec<Value>,
}

/// The pinned private ownership envelope (trace/event.rs:24-76). Decode
/// IDs through their maintained serde form, never debug strings.
#[derive(serde::Deserialize)]
#[serde(deny_unknown_fields)]
struct HandoffFields {
    id: asupersync::ObligationId,
    #[serde(rename = "source_holder")]
    source: TaskId,
    #[serde(rename = "destination_holder")]
    destination: TaskId,
    source_region: RegionId,
    destination_region: RegionId,
    source_ticket: u64,
    destination_ticket: u64,
}

fn handoff_fields(message: &str) -> RunResult<HandoffFields> {
    if message.len() > 2048 {
        return Err("handoff message exceeds 2048 bytes".to_string());
    }
    let payload = message
        .strip_prefix("obligation_handoff_v1 ")
        .ok_or("unsupported obligation handoff message version")?;
    let fields: HandoffFields =
        serde_json::from_str(payload).map_err(|e| format!("invalid handoff: {e}"))?;
    if fields.destination_ticket <= fields.source_ticket {
        return Err("handoff destination ticket must follow source".to_string());
    }
    if fields.source == fields.destination {
        return Err("handoff requires distinct holder generations".to_string());
    }
    Ok(fields)
}

fn project_trace(lab: &LabRuntime, shared: &SharedRef) -> RunResult<Projected> {
    project_events(&lab.trace().snapshot(), shared)
}

/// The projection proper, over any slice of trace events: the unit tests of
/// `projection_tests` feed it hand-built traces (bd-9kll.2.23).
fn project_events(trace: &[TraceEvent], shared: &SharedRef) -> RunResult<Projected> {
    use TraceEventKind as K;
    let s = lock(shared);
    let mut accepted_handoffs = s.handoff_tickets.clone();
    let mut events = Vec::new();
    let mut obligation_names: HashMap<asupersync::ObligationId, String> = HashMap::new();
    let mut per_holder: HashMap<String, u32> = HashMap::new();
    let mut obligations: Vec<(String, Value)> = Vec::new();
    let mut timers: HashMap<String, (u64, bool)> = HashMap::new();
    let mut timer_order: Vec<String> = Vec::new();
    // timer id -> the name of its current schedule (ids are reused).
    let mut timer_current: HashMap<u64, String> = HashMap::new();
    let ids_seen = IdsSeen::from_events(trace);
    for event in trace {
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
            // Every field is named, so a field added on a re-pin fails to
            // compile instead of being dropped; the ones ignored here are
            // listed in docs/VOCABULARY_EXCLUSIONS.md (bd-9kll.2.7).
            (K::RegionCloseBegin, TraceData::Region { region, parent: _ }) => {
                json!({"k": "region.close_begin", "region": region_name(&s, *region)?})
            }
            (K::RegionCloseComplete, TraceData::Region { region, parent: _ }) => {
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
                    state: _,
                    duration_ns,
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
                // The virtual time the obligation was held, on the events
                // that resolve it (`state.rs:5597`, bd-yqe4); a reserve
                // carries none.
                if k != "obligation.reserved" {
                    ev["duration_ns"] = json!(*duration_ns);
                }
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
                // Every event its sleep recorded carries the sleep's name
                // (NamedSleep); one recorded outside the sleep falls back to
                // the timer id's current schedule.
                let name = match s.timer_names.get(&event.seq) {
                    Some(name) => {
                        if event.kind == K::TimerScheduled {
                            timer_current.insert(*timer_id, name.clone());
                        }
                        name.clone()
                    }
                    None => timer_current
                        .get(timer_id)
                        .cloned()
                        .ok_or_else(|| format!("unnamed timer {timer_id}"))?,
                };
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
                    if event.version != asupersync::trace::TRACE_EVENT_SCHEMA_VERSION {
                        return Err("unsupported handoff trace envelope".to_string());
                    }
                    let h = handoff_fields(message)?;
                    if accepted_handoffs.remove(&(h.source_ticket, h.destination_ticket))
                        != Some((
                            h.source,
                            h.destination,
                            h.source_region,
                            h.destination_region,
                        ))
                    {
                        return Err(
                            "handoff trace lacks an accepted runtime ticket link".to_string()
                        );
                    }
                    let name = obligation_names
                        .get(&h.id)
                        .ok_or("handoff of an unnamed obligation")?
                        .clone();
                    let source = task_name(&s, h.source)?;
                    let destination = task_name(&s, h.destination)?;
                    let source_region = region_name(&s, h.source_region)?;
                    let destination_region = region_name(&s, h.destination_region)?;
                    let (_, record) = obligations
                        .iter_mut()
                        .find(|(n, _)| *n == name)
                        .ok_or("handoff lacks a reserved record")?;
                    if record["state"] != "Reserved"
                        || record["holder"] != source
                        || record["region"] != source_region
                    {
                        return Err("handoff disagrees with pending source ownership".to_string());
                    }
                    record["holder"] = json!(destination);
                    record["region"] = json!(destination_region);
                    json!({"k": "obligation.handoff", "obligation": name, "from_task": source, "to_task": destination, "from_region": source_region, "to_region": destination_region})
                } else {
                    match parse_supervisor_trace(message)? {
                        // Its raw ids replaced by their names (vocabulary §3).
                        Some(t) => {
                            let region = ids_seen
                                .regions
                                .get(t.region)
                                .ok_or_else(|| format!("supervisor trace names {}", t.region))?;
                            let task = ids_seen
                                .tasks
                                .get(t.task)
                                .ok_or_else(|| format!("supervisor trace names {}", t.task))?;
                            json!({"k": "user.trace", "message": format!(
                                "{SUPERVISOR_TRACE}action={} supervisor={:?} child={:?} generation={} region={} task={} outcome={}",
                                t.action, t.supervisor, t.child, t.generation,
                                region_name(&s, *region)?, task_name(&s, *task)?, t.outcome,
                            )})
                        }
                        None => json!({"k": "user.trace", "message": message}),
                    }
                }
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
            // A declared futurelock (DSL §1): the check runs at every step
            // with panic off, and each report is a vocabulary event
            // (docs/CANONICAL_VOCABULARY_V2.md §3). Its held list keeps
            // Rust's obligation order; the names are the reserves' own.
            (
                K::FuturelockDetected,
                TraceData::Futurelock {
                    task,
                    region,
                    idle_steps,
                    held,
                },
            ) if s.futurelock_declared => {
                let mut items = Vec::with_capacity(held.len());
                for (oid, kind) in held {
                    let name = obligation_names.get(oid).ok_or_else(|| {
                        format!("futurelock names an obligation never reserved: {oid:?}")
                    })?;
                    items.push(json!({"obligation": name, "kind": format!("{kind:?}")}));
                }
                json!({
                    "k": "futurelock.detected", "task": task_name(&s, *task)?,
                    "region": region_name(&s, *region)?, "idle_steps": idle_steps,
                    "held": items,
                })
            }
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
    if !accepted_handoffs.is_empty() {
        return Err("accepted handoff is missing its runtime ownership trace".to_string());
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
        if s.group_members.contains(name) || s.supervised.contains(name) {
            continue;
        }
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

fn check_expectation(scenario: &Value, observations: &[Value], events: &[Value]) -> RunResult<()> {
    let expect = scenario.get("expect").ok_or("missing expect")?;
    let declared = scenario
        .get("lab")
        .and_then(|l| l.get("futurelock_max_idle_steps"))
        .is_some();
    let reports = events
        .iter()
        .filter(|e| e["k"] == "futurelock.detected")
        .count();
    match str_field(expect, "kind")? {
        // DSL §5: the run reports at least one futurelock, and the scenario
        // declared the threshold that makes it a report.
        "futurelock" => {
            if !declared {
                return Err("expect futurelock needs lab.futurelock_max_idle_steps".to_string());
            }
            if reports == 0 {
                return Err("futurelock expected, none reported".to_string());
            }
            return Ok(());
        }
        "ok" if reports != 0 => {
            return Err("a futurelock was reported in a scenario that expects ok".to_string());
        }
        _ => {}
    }
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

/// The projection of hand-built Rust traces into the vocabulary
/// (bd-9kll.2.23). Each expected event is spelled from the vocabulary table
/// (docs/CANONICAL_VOCABULARY_V2.md section 3), not read back from what the
/// projection returns, so a field that changes in `project_events` fails here
/// before it is baked into every fixture. The last test runs the whole path
/// (lab, names, projection, canonical form) on two small committed scenarios.
#[cfg(test)]
mod projection_tests {
    use super::*;

    fn task(index: u32) -> TaskId {
        serde_json::from_value(json!({"kind": "TaskId", "index": index, "generation": 1}))
            .expect("a task id from its serde envelope")
    }

    fn region(index: u32) -> RegionId {
        serde_json::from_value(json!({"kind": "RegionId", "index": index, "generation": 1}))
            .expect("a region id from its serde envelope")
    }

    fn obligation(index: u32) -> asupersync::ObligationId {
        serde_json::from_value(json!({"kind": "ObligationId", "index": index, "generation": 1}))
            .expect("an obligation id from its serde envelope")
    }

    fn at(nanos: u64) -> Time {
        Time::from_nanos(nanos)
    }

    /// Tasks 1 and 2 are `t1` and `t2`; regions 1 and 2 are `root` and `child`.
    fn world() -> SharedRef {
        let mut s = Shared::default();
        s.task_names.insert(task(1), "t1".to_string());
        s.task_names.insert(task(2), "t2".to_string());
        s.region_names.insert(region(1), "root".to_string());
        s.region_names.insert(region(2), "child".to_string());
        Arc::new(Mutex::new(s))
    }

    fn project(trace: &[TraceEvent], shared: &SharedRef) -> Projected {
        project_events(trace, shared).expect("the trace projects")
    }

    fn reason(kind: CancelKind, nanos: u64) -> CancelReason {
        CancelReason::with_origin(kind, region(1), at(nanos))
    }

    #[test]
    fn a_task_spawn_and_completion_name_the_task_and_its_region() {
        let shared = world();
        let trace = [
            TraceEvent::spawn(1, at(0), task(1), region(2)),
            TraceEvent::complete(2, at(5), task(1), region(2)),
        ];
        assert_eq!(
            Value::Array(project(&trace, &shared).events),
            json!([
                {"k": "task.spawned", "task": "t1", "region": "child"},
                {"k": "task.completed", "task": "t1", "region": "child"},
            ])
        );
    }

    #[test]
    fn a_region_is_created_closed_begin_and_closed_without_its_parent_field() {
        let shared = world();
        let data = TraceData::Region {
            region: region(2),
            parent: Some(region(1)),
        };
        let trace = [
            TraceEvent::region_created(1, at(0), region(1), None),
            TraceEvent::region_created(2, at(0), region(2), Some(region(1))),
            TraceEvent::new(3, at(9), TraceEventKind::RegionCloseBegin, data.clone()),
            TraceEvent::new(4, at(9), TraceEventKind::RegionCloseComplete, data),
        ];
        assert_eq!(
            Value::Array(project(&trace, &shared).events),
            json!([
                {"k": "region.created", "region": "root", "parent": null},
                {"k": "region.created", "region": "child", "parent": "root"},
                {"k": "region.close_begin", "region": "child"},
                {"k": "region.closed", "region": "child"},
            ])
        );
    }

    /// `cancel.requested` carries the reason (docs/CANONICAL_VOCABULARY_V2.md);
    /// the acknowledgement is an excluded event (docs/VOCABULARY_EXCLUSIONS.md).
    #[test]
    fn a_cancel_request_projects_its_reason_and_the_ack_is_excluded() {
        let shared = world();
        let user = reason(CancelKind::User, 7)
            .with_task(task(2))
            .with_message("stop");
        let trace = [
            TraceEvent::cancel_request(1, at(7), task(1), region(2), user.clone()),
            TraceEvent::new(
                2,
                at(8),
                TraceEventKind::CancelAck,
                TraceData::Cancel {
                    task: task(1),
                    region: region(2),
                    reason: user,
                },
            ),
            TraceEvent::region_cancelled(
                3,
                at(9),
                region(2),
                reason(CancelKind::ParentCancelled, 9),
            ),
        ];
        assert_eq!(
            Value::Array(project(&trace, &shared).events),
            json!([
                {
                    "k": "cancel.requested", "task": "t1", "region": "child",
                    "reason": {
                        "kind": "User", "origin_region": "root", "origin_task": "t2",
                        "timestamp_ns": 7, "message": "stop", "cause_chain_len": 0,
                        "truncated": false,
                    },
                },
                {
                    "k": "region.cancelled", "region": "child",
                    "reason": {
                        "kind": "ParentCancelled", "origin_region": "root", "origin_task": null,
                        "timestamp_ns": 9, "message": null, "cause_chain_len": 0,
                        "truncated": false,
                    },
                },
            ])
        );
    }

    /// Names are `<holder>/o<n>` by the holder's order of first appearance;
    /// only the events that resolve an obligation carry `duration_ns`, and
    /// only an abort carries `abort_reason`.
    #[test]
    fn obligation_reserve_commit_abort_and_leak_project_with_stable_names() {
        let shared = world();
        let trace = [
            TraceEvent::obligation_reserve(
                1,
                at(0),
                obligation(1),
                task(1),
                region(1),
                ObligationKind::SendPermit,
            ),
            TraceEvent::obligation_reserve(
                2,
                at(0),
                obligation(2),
                task(2),
                region(1),
                ObligationKind::Ack,
            ),
            TraceEvent::obligation_commit(
                3,
                at(10),
                obligation(1),
                task(1),
                region(1),
                ObligationKind::SendPermit,
                10,
            ),
            TraceEvent::obligation_abort(
                4,
                at(3),
                obligation(2),
                task(2),
                region(1),
                ObligationKind::Ack,
                3,
                ObligationAbortReason::Cancel,
            ),
            TraceEvent::obligation_reserve(
                5,
                at(3),
                obligation(3),
                task(1),
                region(1),
                ObligationKind::Lease,
            ),
            TraceEvent::obligation_leak(
                6,
                at(12),
                obligation(3),
                task(1),
                region(1),
                ObligationKind::Lease,
                9,
            ),
        ];
        let projected = project(&trace, &shared);
        assert_eq!(
            Value::Array(projected.events),
            json!([
                {"k": "obligation.reserved", "obligation": "t1/o1", "task": "t1",
                 "region": "root", "kind": "SendPermit"},
                {"k": "obligation.reserved", "obligation": "t2/o1", "task": "t2",
                 "region": "root", "kind": "Ack"},
                {"k": "obligation.committed", "obligation": "t1/o1", "task": "t1",
                 "region": "root", "kind": "SendPermit", "duration_ns": 10},
                {"k": "obligation.aborted", "obligation": "t2/o1", "task": "t2",
                 "region": "root", "kind": "Ack", "duration_ns": 3, "abort_reason": "Cancel"},
                {"k": "obligation.reserved", "obligation": "t1/o2", "task": "t1",
                 "region": "root", "kind": "Lease"},
                {"k": "obligation.leaked", "obligation": "t1/o2", "task": "t1",
                 "region": "root", "kind": "Lease", "duration_ns": 9},
            ])
        );
        // The final state of each obligation, for the snapshot.
        let names: Vec<&str> = projected
            .obligations
            .iter()
            .map(|(name, _)| name.as_str())
            .collect();
        assert_eq!(names, ["t1/o1", "t2/o1", "t1/o2"]);
        assert_eq!(
            projected.obligations[0].1,
            json!({"state": "Committed", "kind": "SendPermit", "holder": "t1",
                   "region": "root", "abort_reason": null})
        );
        assert_eq!(
            projected.obligations[1].1,
            json!({"state": "Aborted", "kind": "Ack", "holder": "t2",
                   "region": "root", "abort_reason": "Cancel"})
        );
        assert_eq!(
            projected.obligations[2].1,
            json!({"state": "Leaked", "kind": "Lease", "holder": "t1",
                   "region": "root", "abort_reason": null})
        );
    }

    /// A timer takes its sleep's name from the event that scheduled it (seq
    /// in `timer_names`); a fire or cancel recorded under the same timer id
    /// carries that name, and only an unresolved timer is pending.
    #[test]
    fn timer_schedule_fire_and_cancel_project_with_the_sleeps_name() {
        let shared = world();
        {
            let mut s = lock(&shared);
            s.timer_names.insert(10, "sleep.a".to_string());
            s.timer_names.insert(12, "sleep.b".to_string());
            s.timer_names.insert(14, "sleep.c".to_string());
        }
        let trace = [
            TraceEvent::timer_scheduled(10, at(0), 7, at(100)),
            TraceEvent::timer_fired(11, at(100), 7),
            TraceEvent::timer_scheduled(12, at(100), 8, at(200)),
            TraceEvent::timer_cancelled(13, at(150), 8),
            TraceEvent::timer_scheduled(14, at(150), 9, at(300)),
        ];
        let projected = project(&trace, &shared);
        assert_eq!(
            Value::Array(projected.events),
            json!([
                {"k": "timer.scheduled", "timer": "sleep.a", "deadline_ns": 100},
                {"k": "timer.fired", "timer": "sleep.a"},
                {"k": "timer.scheduled", "timer": "sleep.b", "deadline_ns": 200},
                {"k": "timer.cancelled", "timer": "sleep.b"},
                {"k": "timer.scheduled", "timer": "sleep.c", "deadline_ns": 300},
            ])
        );
        assert_eq!(
            Value::Array(projected.timers_pending),
            json!([{"timer": "sleep.c", "deadline_ns": 300}])
        );
    }

    #[test]
    fn a_user_trace_message_passes_through() {
        let shared = world();
        let trace = [TraceEvent::user_trace(1, at(0), "hello")];
        assert_eq!(
            Value::Array(project(&trace, &shared).events),
            json!([{"k": "user.trace", "message": "hello"}])
        );
    }

    /// The scheduler's own bookkeeping is not in the vocabulary
    /// (docs/VOCABULARY_EXCLUSIONS.md), and anything the projection cannot
    /// name is an error, never a silent drop.
    #[test]
    fn excluded_events_vanish_and_unprojectable_ones_fail_the_run() {
        let shared = world();
        let excluded = [
            TraceEvent::schedule(1, at(0), task(1), region(1)),
            TraceEvent::poll(2, at(0), task(1), region(1)),
            TraceEvent::wake(3, at(0), task(1), region(1)),
            TraceEvent::yield_task(4, at(0), task(1), region(1)),
        ];
        assert!(project(&excluded, &shared).events.is_empty());

        let unnamed = [TraceEvent::spawn(1, at(0), task(9), region(1))];
        let error = project_events(&unnamed, &shared).err();
        assert!(error.is_some_and(|e| e.contains("unnamed task")));

        let mismatched = [TraceEvent::new(
            1,
            at(0),
            TraceEventKind::Spawn,
            TraceData::Time {
                old: at(0),
                new: at(1),
            },
        )];
        let error = project_events(&mismatched, &shared).err();
        assert!(error.is_some_and(|e| e.contains("unprojectable trace event")));
    }

    /// Two of the smallest committed scenarios, through the lab: the names,
    /// the order, the Foata form and the final obligation state.
    #[test]
    fn small_scenarios_run_end_to_end() {
        let task_err = json!({
            "channels": [],
            "description": "A task returns an error outcome.",
            "expect": {"kind": "ok"},
            "id": "task-lifecycle-err-outcome-001",
            "lab": {"max_steps": 100},
            "regions": [{"budget": null, "name": "r.main", "parent": "root"}],
            "rules": ["def.outcome.four_valued"],
            "schema": "asx.scenario.v2",
            "script": [],
            "seed": 42,
            "sync": [],
            "tasks": [{
                "budget": null,
                "name": "t.a",
                "program": [{
                    "op": "return",
                    "outcome": {"status": "ASX_E_INVALID_ARGUMENT", "tag": "err"},
                }],
                "region": "r.main",
            }],
            "unit": "task_lifecycle",
        });
        let fixture = run_scenario(&task_err).expect("the scenario runs");
        assert_eq!(
            fixture["trace"],
            json!([
                [{"k": "region.created", "parent": null, "region": "root"}],
                [{"k": "region.created", "parent": "root", "region": "r.main"}],
                [{"k": "task.spawned", "region": "r.main", "task": "t.a"}],
                [{"k": "task.completed", "region": "r.main", "task": "t.a"}],
            ])
        );
        assert_eq!(
            fixture["snapshot"]["tasks"]["t.a"]["outcome"],
            json!({"status": "ASX_E_INVALID_ARGUMENT", "tag": "err"})
        );

        let reserve_commit = json!({
            "channels": [],
            "description": "A task reserves a Lease, commits it and completes ok.",
            "expect": {"kind": "ok"},
            "id": "obligation-reserve-commit-001",
            "lab": {"max_steps": 100},
            "regions": [{"budget": null, "name": "r.main", "parent": "root"}],
            "rules": [
                "rule.obligation.reserve",
                "rule.obligation.commit",
                "inv.obligation.linear",
            ],
            "schema": "asx.scenario.v2",
            "script": [],
            "seed": 42,
            "sync": [],
            "tasks": [{
                "budget": null,
                "name": "t.a",
                "program": [
                    {"as": "o.lease", "kind": "Lease", "op": "reserve"},
                    {"op": "yield"},
                    {"obligation": "o.lease", "op": "commit"},
                ],
                "region": "r.main",
            }],
            "unit": "obligation",
        });
        let fixture = run_scenario(&reserve_commit).expect("the scenario runs");
        assert_eq!(
            fixture["trace"],
            json!([
                [{"k": "region.created", "parent": null, "region": "root"}],
                [{"k": "region.created", "parent": "root", "region": "r.main"}],
                [{"k": "task.spawned", "region": "r.main", "task": "t.a"}],
                [{"k": "obligation.reserved", "kind": "Lease", "obligation": "t.a/o1",
                  "region": "r.main", "task": "t.a"}],
                [{"k": "obligation.committed", "kind": "Lease", "obligation": "t.a/o1",
                  "region": "r.main", "task": "t.a", "duration_ns": 0}],
                [{"k": "task.completed", "region": "r.main", "task": "t.a"}],
            ])
        );
        assert_eq!(
            fixture["snapshot"]["obligations"],
            json!({"t.a/o1": {"abort_reason": null, "holder": "t.a", "kind": "Lease",
                              "region": "r.main", "state": "Committed"}})
        );
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn scenario(lab: Value, expect: Value) -> Value {
        json!({"lab": lab, "expect": expect})
    }

    /// DSL §5: `futurelock` needs the knob and a report, and `ok` with the
    /// knob needs none (bd-9kll.9.6).
    #[test]
    fn futurelock_expectation_is_checked_both_ways() {
        let report = json!({
            "k": "futurelock.detected", "task": "t.holder", "region": "r.main",
            "idle_steps": 4, "held": [{"obligation": "t.holder/o1", "kind": "Lease"}],
        });
        let knob = json!({"max_steps": 100, "futurelock_max_idle_steps": 3});
        let no_knob = json!({"max_steps": 100});

        let expects = scenario(knob.clone(), json!({"kind": "futurelock"}));
        assert!(check_expectation(&expects, &[], std::slice::from_ref(&report)).is_ok());
        assert!(check_expectation(&expects, &[], &[]).is_err());

        let undeclared = scenario(no_knob, json!({"kind": "futurelock"}));
        assert!(check_expectation(&undeclared, &[], std::slice::from_ref(&report)).is_err());

        let ok = scenario(knob, json!({"kind": "ok"}));
        assert!(check_expectation(&ok, &[], &[]).is_ok());
        assert!(check_expectation(&ok, &[], std::slice::from_ref(&report)).is_err());
    }
}
