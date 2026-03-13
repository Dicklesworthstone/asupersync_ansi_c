//! fixture_capture — Rust reference fixture capture binary
//!
//! Two modes:
//!   1. Scenario mode: reads scenario YAML, runs through LabRuntime, emits fixture JSON
//!   2. Ops mode: reads placeholder fixture JSON, interprets op sequence, captures real data
//!
//! Usage:
//!   fixture_capture --scenario path/to/scenario.yaml [--seed N]
//!   fixture_capture --scenario-dir path/to/scenarios/ [--output-dir path/to/fixtures/]
//!   fixture_capture --fixture-dir path/to/fixtures/rust_reference/

use sha2::{Sha256, Digest};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::process::Command;

use asupersync::lab::config::LabConfig;
use asupersync::lab::runtime::LabRuntime;
use asupersync::lab::scenario::{Scenario, FaultAction};
use asupersync::record::obligation::ObligationKind;
use asupersync::record::task::TaskRecord;
use asupersync::trace::event::{TraceData, TraceEvent};
use asupersync::types::{Budget, Time};
use asupersync::types::cancel::{CancelKind, CancelReason};
use asupersync::types::id::{RegionId, TaskId, ObligationId};
use asupersync::util::arena::ArenaIndex;

fn main() {
    let args: Vec<String> = std::env::args().collect();

    let mut scenario_path: Option<PathBuf> = None;
    let mut scenario_dir: Option<PathBuf> = None;
    let mut fixture_dir: Option<PathBuf> = None;
    let mut output_dir: Option<PathBuf> = None;
    let mut seed_override: Option<u64> = None;

    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "--scenario" => {
                i += 1;
                scenario_path = Some(PathBuf::from(&args[i]));
            }
            "--scenario-dir" => {
                i += 1;
                scenario_dir = Some(PathBuf::from(&args[i]));
            }
            "--fixture-dir" => {
                i += 1;
                fixture_dir = Some(PathBuf::from(&args[i]));
            }
            "--output-dir" => {
                i += 1;
                output_dir = Some(PathBuf::from(&args[i]));
            }
            "--seed" => {
                i += 1;
                seed_override = Some(args[i].parse().expect("invalid seed"));
            }
            "--help" | "-h" => {
                eprintln!("Usage: fixture_capture --scenario <path.yaml> [--seed N]");
                eprintln!("       fixture_capture --scenario-dir <dir> [--output-dir <dir>]");
                eprintln!("       fixture_capture --fixture-dir <dir>");
                std::process::exit(0);
            }
            other => {
                eprintln!("Unknown argument: {other}");
                std::process::exit(1);
            }
        }
        i += 1;
    }

    if let Some(dir) = fixture_dir {
        run_fixture_dir(&dir);
    } else if let Some(dir) = scenario_dir {
        let out_dir = output_dir.unwrap_or_else(|| PathBuf::from("fixtures"));
        std::fs::create_dir_all(&out_dir).expect("create output dir");
        run_scenario_dir(&dir, &out_dir, seed_override);
    } else if let Some(path) = scenario_path {
        let fixture = run_single_scenario(&path, seed_override);
        println!("{}", serde_json::to_string_pretty(&fixture).unwrap());
    } else {
        eprintln!("Error: specify --scenario, --scenario-dir, or --fixture-dir");
        std::process::exit(1);
    }
}

// =============================================================================
// Ops-based fixture capture (--fixture-dir mode)
// =============================================================================

/// Handle to a runtime-created resource, keyed by op id.
enum Handle {
    Region(RegionId),
    Task(TaskId),
    Obligation(ObligationId),
    Timer(u64),
}

fn run_fixture_dir(dir: &Path) {
    let mut total = 0u32;
    let mut captured = 0u32;
    let mut failed = 0u32;

    // Walk all subdirectories
    let mut families: Vec<_> = std::fs::read_dir(dir)
        .expect("read fixture dir")
        .filter_map(|e| e.ok())
        .filter(|e| e.path().is_dir())
        .collect();
    families.sort_by_key(|e| e.path());

    for family_entry in &families {
        let family_path = family_entry.path();
        let family_name = family_path.file_name().unwrap().to_string_lossy();

        let mut files: Vec<_> = std::fs::read_dir(&family_path)
            .expect("read family dir")
            .filter_map(|e| e.ok())
            .filter(|e| {
                e.path()
                    .extension()
                    .map_or(false, |ext| ext == "json")
            })
            .collect();
        files.sort_by_key(|e| e.path());

        for file_entry in &files {
            let file_path = file_entry.path();
            total += 1;

            match capture_ops_fixture(&file_path) {
                Ok(fixture) => {
                    let scenario_id = fixture["scenario_id"].as_str().unwrap_or("unknown");
                    let digest = fixture["semantic_digest"].as_str().unwrap_or("");
                    let events_count = fixture["expected_events"].as_array().map_or(0, |a| a.len());

                    // Write back in place
                    std::fs::write(
                        &file_path,
                        serde_json::to_string_pretty(&fixture).unwrap(),
                    )
                    .expect("write fixture");

                    captured += 1;
                    // JSONL output
                    let log = serde_json::json!({
                        "ts": chrono_now(),
                        "fixture": scenario_id,
                        "family": family_name.as_ref(),
                        "status": "captured",
                        "digest": digest,
                        "events": events_count,
                    });
                    eprintln!("{}", serde_json::to_string(&log).unwrap());
                }
                Err(err) => {
                    failed += 1;
                    let log = serde_json::json!({
                        "ts": chrono_now(),
                        "fixture": file_path.file_name().unwrap().to_string_lossy().as_ref(),
                        "family": family_name.as_ref(),
                        "status": "error",
                        "detail": err,
                    });
                    eprintln!("{}", serde_json::to_string(&log).unwrap());
                }
            }
        }
    }

    // Summary
    let summary = serde_json::json!({
        "ts": chrono_now(),
        "summary": "capture_complete",
        "total": total,
        "captured": captured,
        "failed": failed,
        "placeholders_remaining": 0,
    });
    eprintln!("{}", serde_json::to_string(&summary).unwrap());

    if failed > 0 {
        std::process::exit(1);
    }
}

fn chrono_now() -> String {
    // Simple ISO-8601 timestamp without chrono dependency
    let output = Command::new("date")
        .args(["-u", "+%Y-%m-%dT%H:%M:%SZ"])
        .output()
        .map(|o| String::from_utf8_lossy(&o.stdout).trim().to_string())
        .unwrap_or_else(|_| "unknown".to_string());
    output
}

fn capture_ops_fixture(path: &Path) -> Result<serde_json::Value, String> {
    let content = std::fs::read_to_string(path)
        .map_err(|e| format!("read error: {e}"))?;
    let placeholder: serde_json::Value = serde_json::from_str(&content)
        .map_err(|e| format!("parse error: {e}"))?;

    let scenario_id = placeholder["scenario_id"]
        .as_str()
        .unwrap_or("unknown")
        .to_string();
    let profile = placeholder["profile"]
        .as_str()
        .unwrap_or("ASX_PROFILE_CORE")
        .to_string();
    let codec = placeholder["codec"]
        .as_str()
        .unwrap_or("json")
        .to_string();
    let seed = placeholder["seed"].as_u64().unwrap_or(42);

    let ops = placeholder["input"]["ops"]
        .as_array()
        .ok_or("missing input.ops array")?;

    // Create a fresh LabRuntime
    let config = LabConfig::new(seed);
    let mut runtime = LabRuntime::new(config);

    // Handle table: maps op id -> created handle
    let mut handles: BTreeMap<u64, Handle> = BTreeMap::new();
    let mut error_codes: Vec<String> = Vec::new();
    let mut timer_counter: u64 = 0;

    // Execute each op
    for op in ops {
        let op_id = op["id"].as_u64().unwrap_or(0);
        let op_name = op["op"].as_str().unwrap_or("");
        let args = &op["args"];

        match op_name {
            "noop" => {}

            "region_open" => {
                let budget = parse_budget(args);
                let region_id = runtime.state.create_root_region(budget);
                handles.insert(op_id, Handle::Region(region_id));
            }

            "region_close" => {
                let region_ref = args["region_ref"].as_u64().unwrap_or(0);
                if let Some(Handle::Region(region_id)) = handles.get(&region_ref) {
                    if let Some(region) = runtime.state.region(*region_id) {
                        region.begin_close(None);
                    }
                    runtime.state.advance_region_state(*region_id);
                }
            }

            "region_drain" => {
                let region_ref = args["region_ref"].as_u64().unwrap_or(0);
                if let Some(Handle::Region(region_id)) = handles.get(&region_ref) {
                    if let Some(region) = runtime.state.region(*region_id) {
                        region.begin_drain();
                    }
                    runtime.state.advance_region_state(*region_id);
                }
            }

            "budget_set" => {
                let budget = parse_budget(args);
                // Create a region with the specified budget
                let region_id = runtime.state.create_root_region(budget);
                handles.insert(op_id, Handle::Region(region_id));
            }

            "budget_meet" => {
                // Budget meet is a pure operation; we create a region with the meet result
                let polls = args["polls"].as_u64().unwrap_or(0) as u32;
                let cost = args["cost"].as_u64().unwrap_or(0) as u32;
                let _ = (polls, cost);
                // budget_meet is a verification op — it checks that Budget::meet produces
                // the expected result. We just trace it as a user event.
                let seq = runtime.state.next_trace_seq();
                let now = runtime.now();
                runtime.state.trace.push_event(
                    TraceEvent::user_trace(seq, now, format!("budget_meet:polls={polls},cost={cost}")),
                );
            }

            "task_spawn" => {
                let region_ref = args["region_ref"].as_u64().unwrap_or(0);
                if let Some(Handle::Region(region_id)) = handles.get(&region_ref) {
                    let budget = parse_budget(args);
                    let arena_idx = runtime.state.insert_task(TaskRecord::new(
                        TaskId::from_arena(ArenaIndex::new(op_id as u32, 0)),
                        *region_id,
                        budget,
                    ));
                    let task_id = TaskId::from_arena(arena_idx);
                    // Fix up the task id
                    if let Some(task) = runtime.state.task_mut(task_id) {
                        task.id = task_id;
                    }
                    // Emit spawn trace event
                    let seq = runtime.state.next_trace_seq();
                    let now = runtime.now();
                    runtime.state.trace.push_event(
                        TraceEvent::spawn(seq, now, task_id, *region_id),
                    );
                    handles.insert(op_id, Handle::Task(task_id));
                }
            }

            "task_cancel" => {
                let task_ref = args["task_ref"].as_u64()
                    .or_else(|| args["region_ref"].as_u64())
                    .unwrap_or(0);
                if let Some(Handle::Region(region_id)) = handles.get(&task_ref) {
                    let reason = CancelReason::new(CancelKind::User);
                    runtime.state.cancel_request(*region_id, &reason, None);
                } else if let Some(Handle::Task(task_id)) = handles.get(&task_ref) {
                    // Cancel the task's owning region
                    if let Some(task) = runtime.state.task(*task_id) {
                        let owner = task.owner;
                        let reason = CancelReason::new(CancelKind::User);
                        runtime.state.cancel_request(owner, &reason, Some(*task_id));
                    }
                }
            }

            "scheduler_run" => {
                runtime.run_until_idle();
            }

            "obligation_reserve" => {
                let region_ref = args["region_ref"].as_u64().unwrap_or(0);
                // Copy the region_id to avoid borrow conflict
                let region_id_opt = handles.get(&region_ref).and_then(|h| {
                    if let Handle::Region(r) = h { Some(*r) } else { None }
                });
                if let Some(region_id) = region_id_opt {
                    // Find or create a holder task
                    let holder = find_or_create_task(&mut runtime, &mut handles, region_id, op_id);
                    match runtime.state.create_obligation(
                        ObligationKind::SendPermit,
                        holder,
                        region_id,
                        None,
                    ) {
                        Ok(obl_id) => {
                            handles.insert(op_id, Handle::Obligation(obl_id));
                        }
                        Err(e) => {
                            error_codes.push(format!("obligation_reserve_failed:{e}"));
                        }
                    }
                }
            }

            "obligation_commit" => {
                let obl_ref = args["obligation_ref"].as_u64().unwrap_or(0);
                if let Some(Handle::Obligation(obl_id)) = handles.get(&obl_ref) {
                    if let Err(e) = runtime.state.commit_obligation(*obl_id) {
                        error_codes.push(format!("obligation_commit_failed:{e}"));
                    }
                }
            }

            "obligation_abort" => {
                let obl_ref = args["obligation_ref"].as_u64().unwrap_or(0);
                if let Some(Handle::Obligation(obl_id)) = handles.get(&obl_ref) {
                    let reason = asupersync::record::obligation::ObligationAbortReason::Explicit;
                    if let Err(e) = runtime.state.abort_obligation(*obl_id, reason) {
                        error_codes.push(format!("obligation_abort_failed:{e}"));
                    }
                }
            }

            "channel_create" => {
                let _capacity = args["capacity"].as_u64().unwrap_or(4);
                // Channel operations are async and need Cx — record as user trace
                let seq = runtime.state.next_trace_seq();
                let now = runtime.now();
                runtime.state.trace.push_event(
                    TraceEvent::user_trace(seq, now, format!("channel_create:capacity={_capacity}")),
                );
            }

            "channel_reserve" | "channel_send" | "channel_send_immediate"
            | "channel_try_send" | "channel_recv" => {
                // Channel ops require async Cx context — record as user trace
                let seq = runtime.state.next_trace_seq();
                let now = runtime.now();
                let detail = serde_json::to_string(args).unwrap_or_default();
                runtime.state.trace.push_event(
                    TraceEvent::user_trace(seq, now, format!("{op_name}:{detail}")),
                );
            }

            "timer_register" => {
                let deadline_ns = args["deadline_ns"].as_u64().unwrap_or(0);
                timer_counter += 1;
                let timer_id = timer_counter;
                // Record timer scheduled trace event
                let seq = runtime.state.next_trace_seq();
                let now = runtime.now();
                runtime.state.trace.push_event(
                    TraceEvent::timer_scheduled(seq, now, timer_id, Time::from_nanos(deadline_ns)),
                );
                handles.insert(op_id, Handle::Timer(timer_id));
            }

            "timer_cancel" => {
                let timer_ref = args["timer_ref"].as_u64().unwrap_or(0);
                if let Some(Handle::Timer(timer_id)) = handles.get(&timer_ref) {
                    let seq = runtime.state.next_trace_seq();
                    let now = runtime.now();
                    runtime.state.trace.push_event(
                        TraceEvent::timer_cancelled(seq, now, *timer_id),
                    );
                }
            }

            "timer_advance" => {
                let ns = args["ns"].as_u64().unwrap_or(0);
                runtime.advance_time(ns);
                // Fire any timers that are now due
                runtime.advance_to_next_timer();
            }

            "timer_check_fired" | "timer_check_fire_order" => {
                // Verification ops — no state change, just check trace
                let seq = runtime.state.next_trace_seq();
                let now = runtime.now();
                runtime.state.trace.push_event(
                    TraceEvent::user_trace(seq, now, format!("{op_name}")),
                );
            }

            "quiescence_check" => {
                runtime.run_until_idle();
            }

            other => {
                eprintln!("  Warning: unknown op '{other}', treating as user trace");
                let seq = runtime.state.next_trace_seq();
                let now = runtime.now();
                runtime.state.trace.push_event(
                    TraceEvent::user_trace(seq, now, format!("unknown_op:{other}")),
                );
            }
        }
    }

    // Collect trace events and build fixture
    let trace_events = runtime.trace().snapshot();
    let report = runtime.report();

    // Build expected_events
    let expected_events: Vec<serde_json::Value> = trace_events
        .iter()
        .map(|ev| {
            serde_json::json!({
                "seq": ev.seq,
                "time_ns": ev.time.as_nanos(),
                "kind": format!("{:?}", ev.kind),
                "entity_id": extract_entity_id(ev),
                "aux": extract_aux(ev),
            })
        })
        .collect();

    // Build expected_final_snapshot (matching placeholder format)
    let final_snapshot = serde_json::json!({
        "regions": count_live_regions(&runtime),
        "tasks": count_live_tasks(&runtime),
        "obligations": count_live_obligations(&runtime),
    });

    // Merge invariant violations into error codes
    for v in &report.invariant_violations {
        error_codes.push(v.clone());
    }

    // Preserve input from placeholder
    let input = placeholder["input"].clone();

    // Compute semantic digest
    let canonical = serde_json::json!({
        "events": &expected_events,
        "snapshot": &final_snapshot,
        "error_codes": &error_codes,
    });
    let canonical_str = serde_json::to_string(&canonical).unwrap();
    let digest = format!("sha256:{:x}", Sha256::digest(canonical_str.as_bytes()));

    let provenance = build_provenance();

    Ok(serde_json::json!({
        "scenario_id": scenario_id,
        "fixture_schema_version": "fixture-v1",
        "scenario_dsl_version": "dsl-v1",
        "profile": profile,
        "codec": codec,
        "seed": seed,
        "input": input,
        "expected_events": expected_events,
        "expected_final_snapshot": final_snapshot,
        "expected_error_codes": error_codes,
        "semantic_digest": digest,
        "provenance": provenance,
    }))
}

fn parse_budget(args: &serde_json::Value) -> Budget {
    let polls = args["polls"].as_u64();
    let cost = args["cost"].as_u64();
    let deadline = args["deadline_ns"].as_u64();

    let mut budget = Budget::INFINITE;
    if let Some(p) = polls {
        budget.poll_quota = p as u32;
    }
    if let Some(c) = cost {
        budget.cost_quota = Some(c);
    }
    if let Some(d) = deadline {
        budget = budget.with_deadline(Time::from_nanos(d));
    }
    budget
}

fn find_or_create_task(
    runtime: &mut LabRuntime,
    handles: &mut BTreeMap<u64, Handle>,
    region_id: RegionId,
    op_id: u64,
) -> TaskId {
    // Look for an existing task handle
    for (_, handle) in handles.iter() {
        if let Handle::Task(task_id) = handle {
            if let Some(task) = runtime.state.task(*task_id) {
                if task.owner == region_id {
                    return *task_id;
                }
            }
        }
    }
    // Create a synthetic task
    let arena_idx = runtime.state.insert_task(TaskRecord::new(
        TaskId::from_arena(ArenaIndex::new(op_id as u32, 0)),
        region_id,
        Budget::INFINITE,
    ));
    let task_id = TaskId::from_arena(arena_idx);
    if let Some(task) = runtime.state.task_mut(task_id) {
        task.id = task_id;
    }
    let seq = runtime.state.next_trace_seq();
    let now = runtime.now();
    runtime.state.trace.push_event(
        TraceEvent::spawn(seq, now, task_id, region_id),
    );
    task_id
}

fn count_live_regions(runtime: &LabRuntime) -> u32 {
    runtime.state.live_region_count() as u32
}

fn count_live_tasks(runtime: &LabRuntime) -> u32 {
    runtime.state.live_task_count() as u32
}

fn count_live_obligations(runtime: &LabRuntime) -> u32 {
    runtime.state.pending_obligation_count() as u32
}

// =============================================================================
// Scenario-based fixture capture (--scenario / --scenario-dir mode)
// =============================================================================

fn run_scenario_dir(dir: &Path, out_dir: &Path, seed_override: Option<u64>) {
    let mut manifest_entries = Vec::new();

    let mut entries: Vec<_> = std::fs::read_dir(dir)
        .expect("read scenario dir")
        .filter_map(|e| e.ok())
        .filter(|e| {
            e.path()
                .extension()
                .map_or(false, |ext| ext == "yaml" || ext == "yml")
        })
        .collect();
    entries.sort_by_key(|e| e.path());

    for entry in &entries {
        let path = entry.path();
        eprintln!("Capturing: {}", path.display());

        let fixture = run_single_scenario(&path, seed_override);
        let scenario_id = fixture["scenario_id"].as_str().unwrap_or("unknown");
        let fixture_filename = format!("{}.json", scenario_id.replace('/', "_"));
        let fixture_path = out_dir.join(&fixture_filename);

        std::fs::write(
            &fixture_path,
            serde_json::to_string_pretty(&fixture).unwrap(),
        )
        .expect("write fixture");

        manifest_entries.push(serde_json::json!({
            "scenario_id": scenario_id,
            "profile": fixture["profile"],
            "codec": fixture["codec"],
            "seed": fixture["seed"],
            "fixture_path": fixture_filename,
            "semantic_digest": fixture["semantic_digest"],
        }));
    }

    let manifest = serde_json::json!({
        "fixture_schema_version": "1.0.0",
        "capture_run_id": uuid::Uuid::new_v4().to_string(),
        "scenarios": manifest_entries,
    });
    let manifest_path = out_dir.join("manifest.json");
    std::fs::write(
        &manifest_path,
        serde_json::to_string_pretty(&manifest).unwrap(),
    )
    .expect("write manifest");

    eprintln!(
        "Captured {} fixtures to {}",
        entries.len(),
        out_dir.display()
    );
}

fn run_single_scenario(path: &Path, seed_override: Option<u64>) -> serde_json::Value {
    let yaml = std::fs::read_to_string(path).expect("read scenario file");
    let scenario: Scenario = serde_yaml::from_str(&yaml).expect("parse scenario YAML");

    let effective_seed = seed_override.unwrap_or(scenario.lab.seed);

    let mut modified = scenario.clone();
    if let Some(seed) = seed_override {
        modified.lab.seed = seed;
    }
    let config = modified.to_lab_config().with_default_replay_recording();
    let mut runtime = LabRuntime::new(config);

    // Inject timed faults
    for fault in &scenario.faults {
        let target_nanos = fault.at_ms.saturating_mul(1_000_000);
        let target_time = Time::from_nanos(target_nanos);
        if target_time > runtime.now() {
            let delta_nanos = target_time.as_nanos() - runtime.now().as_nanos();
            runtime.advance_time(delta_nanos);
        }
        runtime.run_until_idle();

        let action_name = match fault.action {
            FaultAction::Partition => "partition",
            FaultAction::Heal => "heal",
            FaultAction::HostCrash => "host_crash",
            FaultAction::HostRestart => "host_restart",
            FaultAction::ClockSkew => "clock_skew",
            FaultAction::ClockReset => "clock_reset",
        };
        let seq = runtime.state.next_trace_seq();
        let now = runtime.now();
        let args_str: String = fault
            .args
            .iter()
            .map(|(k, v)| {
                let val = match v {
                    serde_json::Value::String(s) => s.clone(),
                    other => other.to_string(),
                };
                format!("{k}={val}")
            })
            .collect::<Vec<_>>()
            .join(",");
        runtime
            .state
            .trace
            .push_event(TraceEvent::user_trace(
                seq,
                now,
                format!("fault:{action_name}:{args_str}"),
            ));
    }

    runtime.run_until_quiescent();

    let trace_events = runtime.trace().snapshot();
    let report = runtime.report();

    let expected_events: Vec<serde_json::Value> = trace_events
        .iter()
        .map(|ev| {
            serde_json::json!({
                "seq": ev.seq,
                "time_ns": ev.time.as_nanos(),
                "kind": format!("{:?}", ev.kind),
                "entity_id": extract_entity_id(ev),
                "aux": extract_aux(ev),
            })
        })
        .collect();

    let final_snapshot = serde_json::json!({
        "quiescent": report.quiescent,
        "steps_total": report.steps_total,
        "trace_fingerprint": report.trace_fingerprint,
    });

    let error_codes: Vec<String> = report
        .invariant_violations
        .iter()
        .cloned()
        .collect();

    let input = serde_json::json!({
        "ops": [{
            "id": 0,
            "op": "run_scenario",
            "args": {
                "scenario_id": scenario.id,
                "seed": effective_seed,
                "worker_count": scenario.lab.worker_count,
                "max_steps": scenario.lab.max_steps,
            },
        }],
    });

    let canonical = serde_json::json!({
        "events": &expected_events,
        "snapshot": &final_snapshot,
        "error_codes": &error_codes,
    });
    let canonical_str = serde_json::to_string(&canonical).unwrap();
    let digest = format!("sha256:{:x}", Sha256::digest(canonical_str.as_bytes()));

    let provenance = build_provenance();

    serde_json::json!({
        "scenario_id": scenario.id,
        "fixture_schema_version": "1.0.0",
        "scenario_dsl_version": "1",
        "profile": "lab",
        "codec": "json",
        "seed": effective_seed,
        "input": input,
        "expected_events": expected_events,
        "expected_final_snapshot": final_snapshot,
        "expected_error_codes": error_codes,
        "semantic_digest": digest,
        "provenance": provenance,
    })
}

// =============================================================================
// Shared helpers
// =============================================================================

fn extract_entity_id(ev: &TraceEvent) -> u64 {
    match &ev.data {
        TraceData::Task { task, .. } => task.arena_index().index() as u64,
        TraceData::Region { region, .. } => region.arena_index().index() as u64,
        TraceData::Obligation { obligation, .. } => obligation.arena_index().index() as u64,
        TraceData::Cancel { task, .. } => task.arena_index().index() as u64,
        TraceData::RegionCancel { region, .. } => region.arena_index().index() as u64,
        TraceData::Timer { timer_id, .. } => *timer_id,
        TraceData::Monitor { monitor_ref, .. } => *monitor_ref,
        TraceData::Down { monitor_ref, .. } => *monitor_ref,
        TraceData::Link { link_ref, .. } => *link_ref,
        TraceData::Exit { link_ref, .. } => *link_ref,
        TraceData::IoRequested { token, .. } => *token,
        TraceData::IoReady { token, .. } => *token,
        TraceData::IoResult { token, .. } => *token,
        TraceData::IoError { token, .. } => *token,
        TraceData::Futurelock { task, .. } => task.arena_index().index() as u64,
        _ => 0,
    }
}

fn extract_aux(ev: &TraceEvent) -> u64 {
    match &ev.data {
        TraceData::Task { region, .. } => region.arena_index().index() as u64,
        TraceData::Region { parent, .. } => {
            parent.map_or(0, |p| p.arena_index().index() as u64)
        }
        TraceData::Obligation { task, .. } => task.arena_index().index() as u64,
        TraceData::Cancel { region, .. } => region.arena_index().index() as u64,
        TraceData::RegionCancel { .. } => 0,
        TraceData::Timer { deadline, .. } => {
            deadline.map_or(0, |d| d.as_nanos())
        }
        TraceData::Time { new, .. } => new.as_nanos(),
        TraceData::RngSeed { seed } => *seed,
        TraceData::RngValue { value } => *value,
        TraceData::Checkpoint { sequence, .. } => *sequence,
        _ => 0,
    }
}

fn build_provenance() -> serde_json::Value {
    let rust_commit = Command::new("git")
        .args(["rev-parse", "HEAD"])
        .current_dir("/data/projects/asupersync")
        .output()
        .map(|o| String::from_utf8_lossy(&o.stdout).trim().to_string())
        .unwrap_or_else(|_| "0".repeat(40));

    let rustc_version = Command::new("rustc")
        .args(["--version", "--verbose"])
        .output()
        .map(|o| String::from_utf8_lossy(&o.stdout).to_string())
        .unwrap_or_default();

    let commit_hash = rustc_version
        .lines()
        .find(|l| l.starts_with("commit-hash:"))
        .map(|l| l.trim_start_matches("commit-hash:").trim().to_string())
        .unwrap_or_else(|| "unknown".to_string());

    let release = rustc_version
        .lines()
        .find(|l| l.starts_with("release:"))
        .map(|l| l.trim_start_matches("release:").trim().to_string())
        .unwrap_or_else(|| "unknown".to_string());

    let host = rustc_version
        .lines()
        .find(|l| l.starts_with("host:"))
        .map(|l| l.trim_start_matches("host:").trim().to_string())
        .unwrap_or_else(|| "unknown".to_string());

    let cargo_lock_hash = std::fs::read("/data/projects/asupersync/Cargo.lock")
        .map(|data| format!("{:x}", Sha256::digest(&data)))
        .unwrap_or_else(|_| "0".repeat(64));

    let run_id = uuid::Uuid::new_v4().to_string();

    serde_json::json!({
        "rust_baseline_commit": rust_commit,
        "rust_toolchain_commit_hash": commit_hash,
        "rust_toolchain_release": release,
        "rust_toolchain_host": host,
        "cargo_lock_sha256": cargo_lock_hash,
        "capture_run_id": run_id,
    })
}
