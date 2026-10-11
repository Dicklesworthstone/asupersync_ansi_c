//! `twin_run constants --source <asupersync crate dir>` (bd-9kll.2.13): the
//! kernel constants and defaults of asupersync at the pinned rev, as the JSON
//! document `schemas/rust_kernel_constants.json`.
//!
//! Values reachable through the crate's public API are read from the linked
//! crate itself, so they are what the Rust runtime uses, computed values
//! included (cleanup budgets, defaults). Private constants are parsed from the
//! crate's source with syn and carry their span; so is the CancelKind
//! declaration order, which makes a new upstream variant an error here rather
//! than a silent omission. `asx-conformance constants` compares the C
//! runtime's values with the document.

use std::path::Path;

use asupersync::lab::{LabConfig, LabRuntime};
use asupersync::record::FinalizerEscalation;
use asupersync::record::finalizer::{FINALIZER_POLL_BUDGET, FINALIZER_TIME_BUDGET_NANOS};
use asupersync::runtime::RuntimeConfig;
use asupersync::supervision::{BackoffStrategy, SupervisionConfig};
use asupersync::time::TimerWheelConfig;
use asupersync::types::{
    Budget, CancelAttributionConfig, CancelKind, CancelReason, MAX_MASK_DEPTH,
};
use serde_json::{Map, Value, json};
use syn::visit::Visit;

/// Enums whose variants C mirrors: (key, file under src/, enum name). The
/// value is the variant names in declaration order.
const ENUMS: &[(&str, &str, &str)] = &[
    ("task_state.variants", "record/task.rs", "TaskState"),
    ("region_state.variants", "record/region.rs", "RegionState"),
    (
        "obligation_state.variants",
        "record/obligation.rs",
        "ObligationState",
    ),
    (
        "obligation_kind.variants",
        "record/obligation.rs",
        "ObligationKind",
    ),
    (
        "obligation_abort_reason.variants",
        "record/obligation.rs",
        "ObligationAbortReason",
    ),
    ("cancel_phase.variants", "types/cancel.rs", "CancelPhase"),
    (
        "leak_response.variants",
        "runtime/config.rs",
        "ObligationLeakResponse",
    ),
    (
        "finalizer_escalation.variants",
        "record/finalizer.rs",
        "FinalizerEscalation",
    ),
    (
        "supervision.restart_policy.variants",
        "supervision.rs",
        "RestartPolicy",
    ),
    (
        "supervision.escalation_policy.variants",
        "supervision.rs",
        "EscalationPolicy",
    ),
    (
        "supervision.backoff.variants",
        "supervision.rs",
        "BackoffStrategy",
    ),
    (
        "supervision.restart_mode.variants",
        "supervision.rs",
        "ManagedRestartMode",
    ),
];

/// Private constants: (key, file under src/, const name).
const PRIVATE: &[(&str, &str, &str)] = &[
    (
        "lab.handle_cancel_batch",
        "lab/runtime.rs",
        "HANDLE_CANCEL_BATCH",
    ),
    (
        "lab.region_command_batch",
        "lab/runtime.rs",
        "REGION_COMMAND_BATCH",
    ),
    (
        "lab.cancel_streak_limit",
        "lab/runtime.rs",
        "DEFAULT_LAB_CANCEL_STREAK_LIMIT",
    ),
    (
        "timer.level0_resolution_ns",
        "time/wheel.rs",
        "LEVEL0_RESOLUTION_NS",
    ),
    (
        "gen_server.yield_interval",
        "gen_server.rs",
        "YIELD_INTERVAL",
    ),
    ("actor.yield_interval", "actor.rs", "YIELD_INTERVAL"),
    (
        "rwlock.max_consecutive_writers",
        "sync/rwlock.rs",
        "MAX_CONSECUTIVE_WRITERS_BEFORE_READER_BATCH",
    ),
];

struct Doc(Map<String, Value>);

impl Doc {
    fn put(&mut self, key: &str, value: Value, source: &str) {
        self.0
            .insert(key.to_string(), json!({"value": value, "source": source}));
    }
}

fn nanos(d: std::time::Duration) -> Result<u64, String> {
    u64::try_from(d.as_nanos()).map_err(|_| format!("duration {d:?} exceeds u64 ns"))
}

fn budget(doc: &mut Doc, key: &str, b: Budget, source: &str) {
    doc.put(
        &format!("{key}.deadline_ns"),
        b.deadline.map_or(Value::Null, |t| json!(t.as_nanos())),
        source,
    );
    doc.put(&format!("{key}.poll_quota"), json!(b.poll_quota), source);
    doc.put(
        &format!("{key}.cost_quota"),
        b.cost_quota.map_or(Value::Null, |c| json!(c)),
        source,
    );
    doc.put(&format!("{key}.priority"), json!(b.priority), source);
}

/// The first `const NAME: T = <integer literal>;` in a file, with its line.
struct ConstFinder<'a> {
    name: &'a str,
    found: Option<(u64, usize)>,
}

impl<'ast> Visit<'ast> for ConstFinder<'_> {
    fn visit_item_const(&mut self, item: &'ast syn::ItemConst) {
        if self.found.is_none()
            && item.ident == self.name
            && let syn::Expr::Lit(syn::ExprLit {
                lit: syn::Lit::Int(int),
                ..
            }) = &*item.expr
            && let Ok(value) = int.base10_parse::<u64>()
        {
            self.found = Some((value, item.ident.span().start().line));
        }
        syn::visit::visit_item_const(self, item);
    }
}

/// The variants of `enum NAME`, in declaration order, with its line.
struct EnumFinder<'a> {
    name: &'a str,
    found: Option<(Vec<String>, usize)>,
}

impl<'ast> Visit<'ast> for EnumFinder<'_> {
    fn visit_item_enum(&mut self, item: &'ast syn::ItemEnum) {
        if self.found.is_none() && item.ident == self.name {
            let variants = item.variants.iter().map(|v| v.ident.to_string()).collect();
            self.found = Some((variants, item.ident.span().start().line));
        }
        syn::visit::visit_item_enum(self, item);
    }
}

fn parse(source: &Path, file: &str) -> Result<syn::File, String> {
    let path = source.join("src").join(file);
    let text = std::fs::read_to_string(&path)
        .map_err(|e| format!("cannot read {}: {e}", path.display()))?;
    syn::parse_file(&text).map_err(|e| format!("cannot parse {}: {e}", path.display()))
}

/// The constants document for the linked asupersync, revision `rev`, whose
/// source is `crate_dir` (the directory holding its Cargo.toml).
pub fn constants(crate_dir: &Path, rev: &str) -> Result<Value, String> {
    let manifest = std::fs::read_to_string(crate_dir.join("Cargo.toml"))
        .map_err(|e| format!("{}: {e}", crate_dir.join("Cargo.toml").display()))?;
    if !manifest
        .lines()
        .any(|l| l.trim() == "name = \"asupersync\"")
    {
        return Err(format!(
            "{} is not the asupersync crate",
            crate_dir.display()
        ));
    }
    let mut doc = Doc(Map::new());

    // CancelKind: declaration order from the source; each variant's values
    // from the linked crate (its serde name, discriminant, severity and the
    // cleanup budget a CancelReason of that kind carries).
    let cancel = parse(crate_dir, "types/cancel.rs")?;
    let mut finder = EnumFinder {
        name: "CancelKind",
        found: None,
    };
    finder.visit_file(&cancel);
    let (variants, line) = finder
        .found
        .ok_or("src/types/cancel.rs declares no enum CancelKind")?;
    let span = format!("src/types/cancel.rs:{line} enum CancelKind");
    doc.put("cancel_kind.variants", json!(variants), &span);
    for name in &variants {
        let kind: CancelKind = serde_json::from_value(json!(name))
            .map_err(|e| format!("CancelKind::{name} does not deserialize from its name: {e}"))?;
        let cleanup = CancelReason::new(kind).cleanup_budget();
        let budget_source = format!("CancelReason::new(CancelKind::{name}).cleanup_budget()");
        doc.put(
            &format!("cancel_kind.{name}.ordinal"),
            json!(kind as u8),
            &format!("CancelKind::{name} as u8"),
        );
        doc.put(
            &format!("cancel_kind.{name}.severity"),
            json!(kind.severity()),
            &format!("CancelKind::{name}.severity()"),
        );
        doc.put(
            &format!("cancel_kind.{name}.cleanup_poll_quota"),
            json!(cleanup.poll_quota),
            &budget_source,
        );
        doc.put(
            &format!("cancel_kind.{name}.cleanup_priority"),
            json!(cleanup.priority),
            &budget_source,
        );
    }
    doc.put(
        "cancel_reason.default_timestamp_ns",
        json!(CancelReason::new(CancelKind::User).timestamp().as_nanos()),
        "CancelReason::new(..).timestamp()",
    );
    doc.put(
        "cancel_attribution.max_depth",
        json!(CancelAttributionConfig::DEFAULT_MAX_DEPTH),
        "CancelAttributionConfig::DEFAULT_MAX_DEPTH",
    );
    doc.put(
        "cancel_attribution.max_memory",
        json!(CancelAttributionConfig::DEFAULT_MAX_MEMORY),
        "CancelAttributionConfig::DEFAULT_MAX_MEMORY",
    );
    doc.put(
        "task.max_mask_depth",
        json!(MAX_MASK_DEPTH),
        "types::MAX_MASK_DEPTH",
    );

    budget(
        &mut doc,
        "budget.infinite",
        Budget::INFINITE,
        "Budget::INFINITE",
    );
    budget(&mut doc, "budget.zero", Budget::ZERO, "Budget::ZERO");
    budget(
        &mut doc,
        "budget.default",
        Budget::default(),
        "Budget::default()",
    );

    doc.put(
        "timer.max_duration_ns",
        json!(nanos(TimerWheelConfig::default().max_timer_duration)?),
        "TimerWheelConfig::default().max_timer_duration",
    );

    let sup = SupervisionConfig::default();
    let source = "SupervisionConfig::default()";
    doc.put(
        "supervision.default.restart_policy",
        json!(format!("{:?}", sup.restart_policy)),
        source,
    );
    doc.put(
        "supervision.default.escalation",
        json!(format!("{:?}", sup.escalation)),
        source,
    );
    doc.put(
        "supervision.default.max_restarts",
        json!(sup.max_restarts),
        source,
    );
    doc.put(
        "supervision.default.restart_window_ns",
        json!(nanos(sup.restart_window)?),
        source,
    );
    let source = "SupervisionConfig::default().backoff";
    match sup.backoff {
        BackoffStrategy::Exponential {
            initial,
            max,
            multiplier,
        } => {
            doc.put(
                "supervision.default.backoff.kind",
                json!("Exponential"),
                source,
            );
            doc.put(
                "supervision.default.backoff.initial_ns",
                json!(nanos(initial)?),
                source,
            );
            doc.put(
                "supervision.default.backoff.max_ns",
                json!(nanos(max)?),
                source,
            );
            // Canonical JSON holds integers only. C's multiplier is an
            // integer, so an integral one is recorded as such and any other
            // as its decimal text, which C then reports as a difference.
            let integral =
                multiplier.fract() == 0.0 && (0.0..=f64::from(u32::MAX)).contains(&multiplier);
            doc.put(
                "supervision.default.backoff.multiplier",
                if integral {
                    json!(multiplier as u64)
                } else {
                    json!(multiplier.to_string())
                },
                source,
            );
        }
        other => return Err(format!("default backoff is not exponential: {other:?}")),
    }

    doc.put(
        "gen_server.default_mailbox_capacity",
        json!(asupersync::gen_server::DEFAULT_GENSERVER_MAILBOX_CAPACITY),
        "gen_server::DEFAULT_GENSERVER_MAILBOX_CAPACITY",
    );
    doc.put(
        "actor.default_mailbox_capacity",
        json!(asupersync::actor::DEFAULT_MAILBOX_CAPACITY),
        "actor::DEFAULT_MAILBOX_CAPACITY",
    );

    let lab = LabConfig::default();
    let source = "LabConfig::default()";
    doc.put("lab.default.seed", json!(lab.seed), source);
    doc.put(
        "lab.default.max_steps",
        lab.max_steps.map_or(Value::Null, |n| json!(n)),
        source,
    );
    doc.put(
        "lab.default.panic_on_obligation_leak",
        json!(lab.panic_on_obligation_leak),
        source,
    );
    let streak = LabRuntime::new(LabConfig::default())
        .scheduler
        .lock()
        .cancel_streak_limit();
    doc.put(
        "lab.runtime_cancel_streak_limit",
        json!(streak),
        "LabRuntime::new(LabConfig::default()).scheduler.lock().cancel_streak_limit()",
    );

    let rt = RuntimeConfig::default();
    doc.put(
        "runtime.default.obligation_leak_response",
        json!(format!("{:?}", rt.obligation_leak_response)),
        "RuntimeConfig::default().obligation_leak_response",
    );
    doc.put(
        "finalizer.poll_budget",
        json!(FINALIZER_POLL_BUDGET),
        "record::finalizer::FINALIZER_POLL_BUDGET",
    );
    doc.put(
        "finalizer.time_budget_ns",
        json!(FINALIZER_TIME_BUDGET_NANOS),
        "record::finalizer::FINALIZER_TIME_BUDGET_NANOS",
    );
    doc.put(
        "finalizer.default_escalation",
        json!(format!("{:?}", FinalizerEscalation::default())),
        "FinalizerEscalation::default()",
    );

    for (key, file, name) in ENUMS {
        let ast = parse(crate_dir, file)?;
        let mut finder = EnumFinder { name, found: None };
        finder.visit_file(&ast);
        let (variants, line) = finder
            .found
            .ok_or_else(|| format!("src/{file} declares no enum {name}"))?;
        doc.put(
            key,
            json!(variants),
            &format!("src/{file}:{line} enum {name}"),
        );
    }

    for (key, file, name) in PRIVATE {
        let ast = parse(crate_dir, file)?;
        let mut finder = ConstFinder { name, found: None };
        finder.visit_file(&ast);
        let (value, line) = finder
            .found
            .ok_or_else(|| format!("src/{file} has no integer const {name}"))?;
        doc.put(
            key,
            json!(value),
            &format!("src/{file}:{line} const {name}"),
        );
    }

    Ok(json!({
        "schema": "asx.rust_kernel_constants.v1",
        "asupersync_rev": rev,
        "constants": Value::Object(doc.0),
    }))
}
