//! twin_run: the Rust side of the Rust-vs-C oracle (bead W1.4, bd-9kll.2.4).
//!
//! `twin_run capture <scenario.json>... --out <dir>` runs each DSL v2 scenario
//! (docs/SCENARIO_DSL_V2.md) inside asupersync's LabRuntime at the pinned rev,
//! projects the run into vocabulary v2 (docs/CANONICAL_VOCABULARY_V2.md) and
//! writes one fixture per scenario. It fails closed: an unsupported op, an
//! unprojectable trace event or a run error fails that scenario and the exit
//! status, and no fixture is written for it.

mod canon;
mod constants;
mod minimize;
mod run;
// `gen` is a reserved keyword in edition 2024.
#[path = "gen.rs"]
mod scenario_gen;

use std::path::PathBuf;
use std::process::ExitCode;

use serde_json::{Value, json};
use sha2::{Digest, Sha256};

fn usage() -> ExitCode {
    eprintln!(
        "usage: twin_run capture <scenario.json>... --out <dir>\n       \
         twin_run generate --seed <u64> --count <n> --out <dir>\n       \
         twin_run canon-fuzz --seed <u64> --count <n> --out <file.jsonl>\n       \
         twin_run minimize <scenario.json> --runner <asx-conformance> --out <dir>\n       \
         twin_run trace <scenario.json>   (raw lab trace, for diagnosis)\n       \
         twin_run constants --source <asupersync crate dir> --out <file.json>"
    );
    ExitCode::from(2)
}

/// Write `count` generated scenarios (gen.rs) for batch `seed` to `out`.
fn generate(seed: u64, count: u64, out: &PathBuf) -> ExitCode {
    if let Err(err) = std::fs::create_dir_all(out) {
        eprintln!("twin_run: cannot create {}: {err}", out.display());
        return ExitCode::from(1);
    }
    for index in 0..count {
        let scenario = scenario_gen::scenario(seed, index);
        let file = out.join(format!("gen-{seed}-{index}.json"));
        let text = match canon::canonical_json(&scenario) {
            Ok(t) => t,
            Err(err) => {
                eprintln!("twin_run: scenario {index}: {err}");
                return ExitCode::from(1);
            }
        };
        if let Err(err) = std::fs::write(&file, text + "\n") {
            eprintln!("twin_run: cannot write {}: {err}", file.display());
            return ExitCode::from(1);
        }
    }
    println!("twin_run generate: {count} scenario(s) for seed {seed}");
    ExitCode::SUCCESS
}

/// One random vocabulary v2 event over small name pools, so that events
/// often share a task, region, obligation or timer (dependent) and often do
/// not (independent). Messages include characters canonical JSON must
/// escape, and non-ASCII text.
fn random_event(rng: &mut scenario_gen::Rng) -> Value {
    fn pick(rng: &mut scenario_gen::Rng, items: &[&str]) -> String {
        items[usize::try_from(rng.below(items.len() as u64)).unwrap_or(0)].to_string()
    }
    const TASKS: [&str; 4] = ["t.0", "t.1", "t.2", "t.0.c1"];
    const REGIONS: [&str; 3] = ["root", "r.main", "r.side"];
    const OBLIGATIONS: [&str; 4] = ["t.0/o1", "t.0/o2", "t.1/o1", "t.2/o1"];
    const TIMERS: [&str; 3] = ["t.0/tm1", "t.1/tm1", "t.2/tm1"];
    const MESSAGES: [&str; 6] = ["m", "a\"b", "x\\y", "line\nbreak", "\u{1}\u{1f}", "café ✓"];
    match rng.below(13) {
        0 => json!({"k": "task.spawned", "task": pick(rng, &TASKS), "region": pick(rng, &REGIONS)}),
        1 => {
            json!({"k": "task.completed", "task": pick(rng, &TASKS), "region": pick(rng, &REGIONS)})
        }
        2 => json!({"k": "cancel.requested", "task": pick(rng, &TASKS),
                    "region": pick(rng, &REGIONS), "reason": pick(rng, &["User", "Shutdown"])}),
        3 => {
            let parent = if rng.chance(30) {
                Value::Null
            } else {
                json!(pick(rng, &REGIONS))
            };
            json!({"k": "region.created", "region": pick(rng, &REGIONS), "parent": parent})
        }
        4 => json!({"k": pick(rng, &["region.close_begin", "region.closed", "region.cancelled"]),
                    "region": pick(rng, &REGIONS)}),
        5..=7 => json!({
            "k": pick(rng, &["obligation.reserved", "obligation.committed",
                             "obligation.aborted", "obligation.leaked"]),
            "obligation": pick(rng, &OBLIGATIONS), "task": pick(rng, &TASKS),
            "region": pick(rng, &REGIONS), "kind": pick(rng, &["Lease", "SendPermit"])}),
        8 => json!({"k": "obligation.handoff", "obligation": pick(rng, &OBLIGATIONS),
                    "from_task": pick(rng, &TASKS), "to_task": pick(rng, &TASKS),
                    "from_region": pick(rng, &REGIONS), "to_region": pick(rng, &REGIONS)}),
        9 => json!({"k": "timer.scheduled", "timer": pick(rng, &TIMERS),
                    "deadline_ns": 50 * rng.below(8)}),
        10 => json!({"k": pick(rng, &["timer.fired", "timer.cancelled"]),
                     "timer": pick(rng, &TIMERS)}),
        _ => json!({"k": "user.trace", "message": pick(rng, &MESSAGES)}),
    }
}

/// `count` random vocabulary traces for batch `seed`, each with its Foata
/// canonical form and digest (canon.rs), one canonical JSON object per line
/// of `out`: the Rust side of the canonicalizer differential that
/// `asx-conformance canon` checks against C's canon.c (bd-9kll.9.1).
fn canon_fuzz(seed: u64, count: u64, out: &PathBuf) -> ExitCode {
    let mut text = String::new();
    let mut rng = scenario_gen::Rng::new(seed);
    for _ in 0..count {
        let len = 1 + rng.below(40);
        let events: Vec<Value> = (0..len).map(|_| random_event(&mut rng)).collect();
        let line = canon::canonical_trace(&events).and_then(|layers| {
            let layers = Value::Array(layers.into_iter().map(Value::Array).collect());
            let digest = canon::digest(&layers)?;
            canon::canonical_json(&json!({"events": events, "canonical": layers, "digest": digest}))
        });
        match line {
            Ok(l) => {
                text.push_str(&l);
                text.push('\n');
            }
            Err(err) => {
                eprintln!("twin_run canon-fuzz: {err}");
                return ExitCode::from(1);
            }
        }
    }
    if let Some(dir) = out.parent()
        && let Err(err) = std::fs::create_dir_all(dir)
    {
        eprintln!("twin_run: cannot create {}: {err}", dir.display());
        return ExitCode::from(1);
    }
    if let Err(err) = std::fs::write(out, text) {
        eprintln!("twin_run: cannot write {}: {err}", out.display());
        return ExitCode::from(1);
    }
    println!("twin_run canon-fuzz: {count} trace(s) for seed {seed}");
    ExitCode::SUCCESS
}

/// The asupersync commit this tool links, read from its own Cargo.lock so it
/// cannot drift from the pin in Cargo.toml.
fn linked_asupersync_commit() -> Result<String, String> {
    include_str!("../Cargo.lock")
        .lines()
        .find(|l| l.starts_with("source = \"git+https://github.com/Dicklesworthstone/asupersync"))
        .and_then(|l| l.rsplit('#').next())
        .map(|sha| sha.trim_end_matches('"').to_string())
        .filter(|sha| sha.len() == 40 && sha.bytes().all(|b| b.is_ascii_hexdigit()))
        .ok_or_else(|| "Cargo.lock has no pinned asupersync git source".to_string())
}

fn provenance() -> Result<Value, String> {
    let lock = Sha256::digest(include_bytes!("../Cargo.lock"));
    Ok(json!({
        "rust_baseline_commit": linked_asupersync_commit()?,
        // The compiler that built this binary (build.rs), not the one on PATH.
        "rust_toolchain_commit_hash": env!("TWIN_RUN_RUSTC_COMMIT_HASH"),
        "rust_toolchain_release": env!("TWIN_RUN_RUSTC_RELEASE"),
        "rust_toolchain_host": env!("TWIN_RUN_RUSTC_HOST"),
        "cargo_lock_sha256": lock.iter().map(|b| format!("{b:02x}")).collect::<String>(),
        "capture_run_id": uuid::Uuid::new_v4().to_string(),
        "scenario_dsl": "asx.scenario.v2",
        "vocabulary": "asx.vocab.v2",
        "producer": "tools/twin_run",
    }))
}

fn capture(paths: &[PathBuf], out: &PathBuf) -> ExitCode {
    if let Err(err) = std::fs::create_dir_all(out) {
        eprintln!("twin_run: cannot create {}: {err}", out.display());
        return ExitCode::from(1);
    }
    let provenance = match provenance() {
        Ok(p) => p,
        Err(err) => {
            eprintln!("twin_run: {err}");
            return ExitCode::from(1);
        }
    };
    let mut failed = 0usize;
    for path in paths {
        let scenario: Value = match std::fs::read_to_string(path)
            .map_err(|e| e.to_string())
            .and_then(|s| serde_json::from_str(&s).map_err(|e| e.to_string()))
        {
            Ok(v) => v,
            Err(err) => {
                eprintln!("FAIL {}: unreadable scenario: {err}", path.display());
                failed += 1;
                continue;
            }
        };
        let Some(id) = scenario["id"].as_str().map(str::to_string) else {
            eprintln!("FAIL {}: scenario has no string id", path.display());
            failed += 1;
            continue;
        };
        match run::run_scenario(&scenario) {
            Ok(mut fixture) => {
                fixture["provenance"] = provenance.clone();
                let file = out.join(format!("{id}.json"));
                let text = match canon::canonical_json(&fixture) {
                    Ok(t) => t,
                    Err(err) => {
                        eprintln!("FAIL {id}: {err}");
                        failed += 1;
                        continue;
                    }
                };
                if let Err(err) = std::fs::write(&file, text + "\n") {
                    eprintln!("FAIL {id}: cannot write {}: {err}", file.display());
                    failed += 1;
                    continue;
                }
                println!(
                    "PASS {id} {}",
                    fixture["semantic_digest"].as_str().unwrap_or("")
                );
            }
            Err(err) => {
                eprintln!("FAIL {id}: {err}");
                failed += 1;
            }
        }
    }
    println!(
        "twin_run capture: {} scenario(s), {failed} failed",
        paths.len()
    );
    if failed > 0 {
        ExitCode::from(1)
    } else {
        ExitCode::SUCCESS
    }
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    match args.first().map(String::as_str) {
        Some("capture") => {
            let mut paths = Vec::new();
            let mut out = None;
            let mut rest = args[1..].iter();
            while let Some(arg) = rest.next() {
                if arg == "--out" {
                    out = rest.next().map(PathBuf::from);
                } else {
                    paths.push(PathBuf::from(arg));
                }
            }
            match out {
                Some(out) if !paths.is_empty() => capture(&paths, &out),
                _ => usage(),
            }
        }
        Some("trace") => {
            // Raw lab trace of one scenario, for diagnosing a divergence.
            let Some(path) = args.get(1) else {
                return usage();
            };
            let scenario: Value = match std::fs::read_to_string(path)
                .map_err(|e| e.to_string())
                .and_then(|s| serde_json::from_str(&s).map_err(|e| e.to_string()))
            {
                Ok(v) => v,
                Err(err) => {
                    eprintln!("twin_run: {path}: {err}");
                    return ExitCode::from(1);
                }
            };
            let mut raw = Vec::new();
            let result = run::run_scenario_with(&scenario, Some(&mut raw));
            for line in &raw {
                println!("{line}");
            }
            match result {
                Ok(_) => ExitCode::SUCCESS,
                Err(err) => {
                    eprintln!("twin_run: {err}");
                    ExitCode::from(1)
                }
            }
        }
        Some(cmd @ ("generate" | "canon-fuzz")) => {
            let mut seed = None;
            let mut count = None;
            let mut out = None;
            let mut rest = args[1..].iter();
            while let Some(arg) = rest.next() {
                let value = rest.next();
                match arg.as_str() {
                    "--seed" => seed = value.and_then(|v| v.parse::<u64>().ok()),
                    "--count" => count = value.and_then(|v| v.parse::<u64>().ok()),
                    "--out" => out = value.map(PathBuf::from),
                    _ => return usage(),
                }
            }
            match (seed, count, out) {
                (Some(seed), Some(count), Some(out)) if cmd == "generate" => {
                    generate(seed, count, &out)
                }
                (Some(seed), Some(count), Some(out)) => canon_fuzz(seed, count, &out),
                _ => usage(),
            }
        }
        Some("constants") => {
            // The kernel constants of the linked asupersync (constants.rs).
            let mut source = None;
            let mut out = None;
            let mut rest = args[1..].iter();
            while let Some(arg) = rest.next() {
                match arg.as_str() {
                    "--source" => source = rest.next().map(PathBuf::from),
                    "--out" => out = rest.next().map(PathBuf::from),
                    _ => return usage(),
                }
            }
            let (Some(source), Some(out)) = (source, out) else {
                return usage();
            };
            let result = linked_asupersync_commit()
                .and_then(|rev| constants::constants(&source, &rev))
                .and_then(|doc| canon::canonical_json(&doc))
                .and_then(|text| {
                    std::fs::write(&out, text + "\n").map_err(|e| format!("{}: {e}", out.display()))
                });
            match result {
                Ok(()) => {
                    println!("twin_run constants: wrote {}", out.display());
                    ExitCode::SUCCESS
                }
                Err(err) => {
                    eprintln!("twin_run constants: {err}");
                    ExitCode::from(1)
                }
            }
        }
        Some("minimize") => {
            let mut path = None;
            let mut runner = None;
            let mut out = None;
            let mut rest = args[1..].iter();
            while let Some(arg) = rest.next() {
                match arg.as_str() {
                    "--runner" => runner = rest.next().map(PathBuf::from),
                    "--out" => out = rest.next().map(PathBuf::from),
                    _ => path = Some(PathBuf::from(arg)),
                }
            }
            let (Some(path), Some(runner), Some(out)) = (path, runner, out) else {
                return usage();
            };
            let result = std::fs::read_to_string(&path)
                .map_err(|e| format!("{}: {e}", path.display()))
                .and_then(|s| serde_json::from_str::<Value>(&s).map_err(|e| e.to_string()))
                .and_then(|scenario| minimize::minimize(&scenario, &runner, &out, &provenance()?));
            match result {
                Ok(()) => ExitCode::SUCCESS,
                Err(err) => {
                    eprintln!("twin_run minimize: {err}");
                    ExitCode::from(1)
                }
            }
        }
        _ => usage(),
    }
}
