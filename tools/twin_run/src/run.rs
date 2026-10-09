//! Runs one asx.scenario.v2 scenario inside asupersync's LabRuntime and
//! projects it into asx.vocab.v2 (docs/SCENARIO_DSL_V2.md,
//! docs/CANONICAL_VOCABULARY_V2.md).
//!
//! Increment 1 implements the driver (setup, script, finish), the control,
//! masking, obligation, spawn/join and child-region steps, trace projection
//! and the snapshot; increment 2 adds the sync (mutex, semaphore, barrier,
//! notify) and channel (mpsc, oneshot, broadcast, watch) steps. Group,
//! combinator and actor steps are not yet interpreted: a scenario that uses
//! one fails as a harness error, never silently.

use std::collections::{BTreeMap, HashMap, HashSet};
use std::future::Future;
use std::pin::Pin;
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::Duration;

use asupersync::channel::{broadcast, mpsc, oneshot, watch};
use asupersync::cx::{ChildRegion, ChildRegionSpec};
use asupersync::lab::{LabConfig, LabRuntime};
use asupersync::record::task::TaskState;
use asupersync::record::{ObligationAbortReason, ObligationKind};
use asupersync::runtime::obligation_mailbox::ObligationToken;
use asupersync::runtime::{JoinError, TaskHandle};
use asupersync::sync as asx_sync;
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
    /// Child name -> index of the observation of the spawn step that
    /// created it (`project_denied_spawns`).
    spawn_observations: HashMap<String, usize>,
    /// Task name -> name of the region it runs in.
    task_regions: HashMap<String, String>,
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
    Ok(())
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
    tokens: HashMap<String, ObligationToken>,
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
    /// Held mpsc send permits by permit name, with their channel (DSL
    /// §3.6). Declared before `cx` and `mpsc_tx` so they drop first: each
    /// borrows its channel's sender and the Cx it was reserved with
    /// (`Reserve<'a>` ties both to the permit, `channel/mpsc.rs:951`).
    send_permits: BTreeMap<String, (String, mpsc::SendPermit<'static, u64>)>,
    /// A clone of the task's Cx for permits to borrow (`lend`).
    cx: Option<Box<Cx>>,
    /// The channel endpoints this task owns, by channel name (DSL §3.6).
    /// They drop in this order (each map by name) when the body ends; the C
    /// interpreter closes them in the same order.
    mpsc_tx: BTreeMap<String, Box<mpsc::Sender<u64>>>,
    mpsc_rx: BTreeMap<String, mpsc::Receiver<u64>>,
    oneshot_tx: BTreeMap<String, oneshot::Sender<u64>>,
    oneshot_rx: BTreeMap<String, oneshot::Receiver<u64>>,
    broadcast_tx: BTreeMap<String, broadcast::Sender<u64>>,
    broadcast_rx: BTreeMap<String, broadcast::Receiver<u64>>,
    watch_tx: BTreeMap<String, watch::Sender<u64>>,
    watch_rx: BTreeMap<String, watch::Receiver<u64>>,
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
        // `Local`. The sender box is freed only by `close_sender`, which
        // first requires that no permit on the channel is held; the Cx box
        // is never replaced; and when `Local` drops, `send_permits` goes
        // first (fields drop in declaration order). A borrow taken for a
        // `send` or `reserve` future lives within one step, and the steps
        // of a task never overlap.
        Ok(unsafe { (&*tx, &*cx) })
    }

    fn receiver(&mut self, channel: &str) -> RunResult<&mut mpsc::Receiver<u64>> {
        self.mpsc_rx
            .get_mut(channel)
            .ok_or_else(|| format!("this task does not own the receiver of {channel:?}"))
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
    Semaphore(&'static asx_sync::Semaphore),
    Barrier(&'static asx_sync::Barrier),
    Notify(&'static asx_sync::Notify),
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
}

fn run_program(
    cx: Cx,
    ctx: Arc<TaskCtx>,
    steps: Vec<Value>,
    endpoints: Vec<(String, Endpoint)>,
) -> Pin<Box<dyn Future<Output = Body> + Send>> {
    Box::pin(async move {
        let mut local = Local {
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

/// The observation of a step naming an obligation, guard, semaphore permit
/// or send permit the task does not hold, because the step that would have
/// acquired it failed or it was already released (DSL §3).
const NOT_HELD: &str = "ASX_E_NOT_FOUND";

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
            let Some(token) = local.tokens.remove(name) else {
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
            let factory = move |ccx: Cx| run_program(ccx, child_ctx, program, Vec::new());
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
            let Some((_, permit)) = local.send_permits.remove(name) else {
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
                return Err(format!(
                    "close_sender on {ch:?} while holding one of its permits (a Rust body cannot drop a borrowed sender)"
                ));
            }
            local.close(ch, true)?;
            observe(&ctx.shared, me, idx, label, "ASX_OK", Value::Null);
        }
        "close_receiver" => {
            local.close(str_field(step, "channel")?, false)?;
            observe(&ctx.shared, me, idx, label, "ASX_OK", Value::Null);
        }
        // Non-blocking oneshot, broadcast and watch sends (DSL §3.6).
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
            let status = match tx.send(cx, u64_field(step, "value")?) {
                Ok(_) => "ASX_OK",
                Err(e) => broadcast_send_status(&e),
            };
            observe(&ctx.shared, me, idx, label, status, Value::Null);
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
        // Blocking oneshot, broadcast and watch receives (DSL §3.6).
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
            let ch = str_field(step, "channel")?;
            let rx = local
                .watch_rx
                .get_mut(ch)
                .ok_or_else(|| format!("this task does not own a watch receiver of {ch:?}"))?;
            match rx.changed(cx).await {
                Ok(()) => {
                    let v = *rx.borrow_and_update();
                    observe(&ctx.shared, me, idx, op, "ASX_OK", json!(v));
                }
                Err(e) => observe(&ctx.shared, me, idx, op, watch_recv_status(e), Value::Null),
            }
        }
        // Blocking sync steps (DSL §3.7).
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
        // Task groups (DSL §3.5).
        "race" | "join_all" => {
            if step.get("deadline_ns").is_some() {
                return Err("race deadline_ns is not interpreted yet (increment 3b)".to_string());
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
                    });
                    let program = program.clone();
                    move |ccx: Cx| async move {
                        match run_program(ccx, child_ctx, program, Vec::new()).await {
                            Body::Ok => Ok(()),
                            Body::Err(status) => Err(status),
                        }
                    }
                })
                .collect();
            let result = cx.scope().quorum(cx, needed, branches).await;
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
            match result {
                Ok(successes) => {
                    observe(&ctx.shared, me, idx, op, "ASX_OK", json!(successes.len()))
                }
                Err(e) => {
                    return Err(format!(
                        "quorum error {e:?} has no vocabulary mapping yet (increment 3b)"
                    ));
                }
            }
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

/// A sleep that names the Timer events it records. Those events carry no
/// task, and their timer id is a wheel slab index that is reused once the
/// timer fires (time/wheel.rs:554): a sleep records its TimerFired when it
/// is next polled (time/sleep.rs:688), by which time another sleep may have
/// been scheduled under the same id. So every Timer event is named by the
/// sleep that records it, the one being polled or dropped at that moment
/// (worker_count 1): its first TimerScheduled takes the owner's next timer
/// number (a sleep that schedules none takes no number, as C counts them),
/// and a re-arm, the fire and a cancel keep that name.
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

impl<F: Future<Output = ()>> Future for NamedSleep<'_, F> {
    type Output = ();

    fn poll(self: Pin<&mut Self>, pcx: &mut std::task::Context<'_>) -> std::task::Poll<()> {
        let this = self.get_mut();
        let before = this.last_seq();
        let Some(sleep) = this.sleep.as_mut() else {
            return std::task::Poll::Ready(());
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

// ---------------------------------------------------------------------------
// The driver
// ---------------------------------------------------------------------------

pub fn run_scenario(scenario: &Value) -> RunResult<Value> {
    run_scenario_with(scenario, None)
}

/// As [`run_scenario`], also appending the raw lab trace (seq, kind, data)
/// to `raw` for diagnosing a divergence (`twin_run trace`).
pub fn run_scenario_with(scenario: &Value, raw: Option<&mut Vec<String>>) -> RunResult<Value> {
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
        match str_field(decl, "type")? {
            "mpsc" => {
                let (tx, rx) = mpsc::channel::<u64>(capacity()?);
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
        lab.scheduler.lock().schedule(task_id, 0);
        let mut s = lock(&shared);
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

    let schedule = lab
        .finish_forced_schedule_recording()
        .map_err(|e| format!("{e:?}"))?;
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
    project_denied_spawns(&mut lock(&shared))?;
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
            "forced_schedule": forced_schedule,
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

/// Spawns their region never admitted. Rust's `cx.spawn` accepts a child
/// into the spawn mailbox and admission later refuses it when the region is
/// closing or closed (`SpawnError::RegionClosed`, runtime/state.rs:1702),
/// resolving the handle as cancelled (spawn_mailbox.rs:1394-1417); the
/// child never runs and has no trace events. C refuses the same spawn
/// synchronously with `ASX_E_REGION_CLOSED`, so the refusal is projected
/// onto the spawn step, and a join of the child observes it too (DSL §3.4).
/// An unadmitted spawn whose handle did not resolve as cancelled is a
/// scenario or harness defect.
fn project_denied_spawns(s: &mut Shared) -> RunResult<()> {
    let denied: Vec<String> = s.provisional.values().cloned().collect();
    for name in denied {
        if !matches!(
            s.raw_outcomes.get(&name),
            Some(Err(JoinError::Cancelled(_)))
        ) {
            return Err(format!(
                "spawned task {name:?} was never admitted (scenario or harness defect)"
            ));
        }
        s.raw_outcomes.remove(&name);
        let spawn_at = s.spawn_observations.get(&name).copied();
        let join_at = s
            .outcome_observations
            .iter()
            .filter(|(_, target)| *target == name)
            .map(|(i, _)| *i);
        for i in spawn_at.into_iter().chain(join_at).collect::<Vec<_>>() {
            s.observations[i]["status"] = json!("ASX_E_REGION_CLOSED");
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

fn project_trace(lab: &LabRuntime, shared: &SharedRef) -> RunResult<Projected> {
    use TraceEventKind as K;
    let s = lock(shared);
    let mut events = Vec::new();
    let mut obligation_names: HashMap<asupersync::ObligationId, String> = HashMap::new();
    let mut per_holder: HashMap<String, u32> = HashMap::new();
    let mut obligations: Vec<(String, Value)> = Vec::new();
    let mut timers: HashMap<String, (u64, bool)> = HashMap::new();
    let mut timer_order: Vec<String> = Vec::new();
    // timer id -> the name of its current schedule (ids are reused).
    let mut timer_current: HashMap<u64, String> = HashMap::new();
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
        if s.group_members.contains(name) {
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
