//! twin_run: the Rust side of the Rust-vs-C oracle (bead W1.4, bd-9kll.2.4).
//!
//! `twin_run capture <scenario.json>... --out <dir>` runs each DSL v2 scenario
//! (docs/SCENARIO_DSL_V2.md) inside asupersync's LabRuntime at the pinned rev,
//! projects the run into vocabulary v2 (docs/CANONICAL_VOCABULARY_V2.md) and
//! writes one fixture per scenario. It fails closed: an unsupported op, an
//! unprojectable trace event or a run error fails that scenario and the exit
//! status, and no fixture is written for it.

mod canon;
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
         twin_run trace <scenario.json>   (raw lab trace, for diagnosis)"
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
        Some("generate") => {
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
                (Some(seed), Some(count), Some(out)) => generate(seed, count, &out),
                _ => usage(),
            }
        }
        _ => usage(),
    }
}
