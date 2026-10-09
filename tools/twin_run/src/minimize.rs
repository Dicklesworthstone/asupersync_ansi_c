//! Delta-debugging minimizer for a Rust-vs-C divergence (bd-ij9w).
//!
//! `twin_run minimize <scenario.json> --runner <asx-conformance> --out <dir>`
//! starts from a scenario whose Rust capture the C runtime fails to match
//! (`asx-conformance compare` reports FAIL) and deletes structure one element
//! at a time: tasks, script entries, program steps (also inside spawned
//! programs, `masked` blocks and group members), and region, channel and
//! sync declarations. A deletion is kept only when Rust still runs the
//! smaller scenario and C still FAILs it in the same part of the comparison
//! (trace, snapshot, observations, ...); a scenario Rust rejects or C cannot
//! interpret (ERROR) is not a reproduction. Passes repeat until none removes
//! anything, so the result is 1-minimal for single deletions.
//!
//! The output is the reduced scenario, renamed `<id>-min`, and its Rust
//! fixture: unmodified capture output, ready to become a curated fixture
//! once the divergence is understood. `candidate.json` in the output
//! directory is the last fixture compared.

use std::path::Path;
use std::process::Command;

use serde_json::Value;

use crate::{canon, run};

/// Arrays whose elements a reduction may delete.
const REDUCIBLE: [&str; 8] = [
    "tasks", "script", "regions", "channels", "sync", "program", "steps", "members",
];

#[derive(Clone)]
enum Seg {
    Key(String),
    Idx(usize),
}

/// Every deletable element, as a path from the document root. An array is
/// reducible under a key in REDUCIBLE, or as one member program of a group
/// (`members` holds arrays of steps).
fn deletable(v: &Value, path: &mut Vec<Seg>, reducible: bool, out: &mut Vec<Vec<Seg>>) {
    match v {
        Value::Object(map) => {
            for (k, child) in map {
                path.push(Seg::Key(k.clone()));
                deletable(child, path, REDUCIBLE.contains(&k.as_str()), out);
                path.pop();
            }
        }
        Value::Array(items) => {
            let members = matches!(path.last(), Some(Seg::Key(k)) if k == "members");
            for (i, item) in items.iter().enumerate() {
                path.push(Seg::Idx(i));
                if reducible {
                    out.push(path.clone());
                }
                deletable(item, path, members, out);
                path.pop();
            }
        }
        _ => {}
    }
}

/// The document without the element at `path` (which ends in an index).
fn without(doc: &Value, path: &[Seg]) -> Option<Value> {
    let mut copy = doc.clone();
    let (last, parent) = path.split_last()?;
    let Seg::Idx(i) = last else { return None };
    let mut node = &mut copy;
    for seg in parent {
        node = match seg {
            Seg::Key(k) => node.get_mut(k.as_str())?,
            Seg::Idx(j) => node.get_mut(*j)?,
        };
    }
    let items = node.as_array_mut()?;
    if *i >= items.len() {
        return None;
    }
    items.remove(*i);
    Some(copy)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn paths(doc: &Value) -> Vec<String> {
        let mut out = Vec::new();
        deletable(doc, &mut Vec::new(), false, &mut out);
        out.iter()
            .map(|p| {
                p.iter()
                    .map(|s| match s {
                        Seg::Key(k) => k.clone(),
                        Seg::Idx(i) => i.to_string(),
                    })
                    .collect::<Vec<_>>()
                    .join("/")
            })
            .collect()
    }

    #[test]
    fn enumerates_nested_programs_members_and_declarations() {
        let doc = json!({
            "id": "x", "seed": 1, "lab": {"max_steps": 10},
            "regions": [{"name": "r.main"}],
            "tasks": [{"name": "t", "program": [
                {"op": "spawn", "as": "c", "program": [{"op": "yield"}]},
                {"op": "masked", "steps": [{"op": "checkpoint"}]},
                {"op": "join_all", "members": [[{"op": "yield"}, {"op": "yield"}]]},
            ]}],
            "script": [{"at_ns": 0, "op": {"op": "advance", "ns": 5}}],
        });
        let mut got = paths(&doc);
        got.sort();
        let mut want = vec![
            "regions/0",
            "script/0",
            "tasks/0",
            "tasks/0/program/0",
            "tasks/0/program/0/program/0",
            "tasks/0/program/1",
            "tasks/0/program/1/steps/0",
            "tasks/0/program/2",
            "tasks/0/program/2/members/0",
            "tasks/0/program/2/members/0/0",
            "tasks/0/program/2/members/0/1",
        ];
        want.sort_unstable();
        assert_eq!(got, want);
    }

    #[test]
    fn deletes_exactly_one_element() {
        let doc = json!({"tasks": [{"program": [{"op": "a"}, {"op": "b"}, {"op": "c"}]}]});
        let path = vec![
            Seg::Key("tasks".into()),
            Seg::Idx(0),
            Seg::Key("program".into()),
            Seg::Idx(1),
        ];
        let smaller = without(&doc, &path).expect("deletable");
        assert_eq!(
            smaller,
            json!({"tasks": [{"program": [{"op": "a"}, {"op": "c"}]}]})
        );
        // The source is untouched; a path past the end deletes nothing.
        assert_eq!(doc["tasks"][0]["program"].as_array().map(Vec::len), Some(3));
        let past = vec![Seg::Key("tasks".into()), Seg::Idx(4)];
        assert!(without(&doc, &past).is_none());
    }
}

/// Rust's fixture for `scenario`, or None when Rust cannot run it.
fn capture(scenario: &Value, provenance: &Value) -> Option<Value> {
    let mut fixture = run::run_scenario(scenario).ok()?;
    fixture["provenance"] = provenance.clone();
    Some(fixture)
}

/// The part of the comparison that differs ("trace", "snapshot...", ...) when
/// the C runtime FAILs `fixture`; None when it passes or reports ERROR.
fn c_divergence(fixture: &Value, runner: &Path, work: &Path) -> Result<Option<String>, String> {
    let file = work.join("candidate.json");
    let text = canon::canonical_json(fixture)?;
    std::fs::write(&file, text + "\n")
        .map_err(|e| format!("cannot write {}: {e}", file.display()))?;
    let output = Command::new(runner)
        .arg("compare")
        .arg(&file)
        .output()
        .map_err(|e| format!("cannot run {}: {e}", runner.display()))?;
    let stdout = String::from_utf8_lossy(&output.stdout);
    Ok(stdout.lines().find_map(|line| {
        let rest = line.strip_prefix("FAIL ")?;
        let (_, what) = rest.split_once(": ")?;
        Some(what.split_whitespace().next().unwrap_or("").to_string())
    }))
}

fn count_deletable(doc: &Value) -> usize {
    let mut out = Vec::new();
    deletable(doc, &mut Vec::new(), false, &mut out);
    out.len()
}

/// Minimize `scenario`; writes `<id>-min.scenario.json` and
/// `<id>-min.fixture.json` to `out`.
pub fn minimize(
    scenario: &Value,
    runner: &Path,
    out: &Path,
    provenance: &Value,
) -> Result<(), String> {
    std::fs::create_dir_all(out).map_err(|e| format!("cannot create {}: {e}", out.display()))?;
    let fixture = capture(scenario, provenance).ok_or("Rust cannot run the scenario")?;
    let kind = c_divergence(&fixture, runner, out)?
        .ok_or("the C runtime does not FAIL this scenario; nothing to minimize")?;
    let before = count_deletable(scenario);
    let mut current = scenario.clone();
    let mut tried = 0usize;
    loop {
        let mut candidates = Vec::new();
        deletable(&current, &mut Vec::new(), false, &mut candidates);
        // Larger structures first: shorter paths name tasks and script
        // entries before the steps inside them.
        candidates.sort_by_key(Vec::len);
        let mut reduced = None;
        for path in &candidates {
            let Some(smaller) = without(&current, path) else {
                continue;
            };
            tried += 1;
            let Some(fixture) = capture(&smaller, provenance) else {
                continue;
            };
            if c_divergence(&fixture, runner, out)?.as_deref() == Some(kind.as_str()) {
                reduced = Some(smaller);
                break;
            }
        }
        match reduced {
            Some(smaller) => current = smaller,
            None => break,
        }
    }

    let id = current["id"].as_str().unwrap_or("scenario").to_string();
    let min_id = format!("{id}-min");
    current["id"] = Value::String(min_id.clone());
    let fixture = capture(&current, provenance).ok_or("Rust cannot run the renamed scenario")?;
    if c_divergence(&fixture, runner, out)?.as_deref() != Some(kind.as_str()) {
        return Err("the renamed minimal scenario no longer diverges".to_string());
    }
    for (name, doc) in [("scenario", &current), ("fixture", &fixture)] {
        let file = out.join(format!("{min_id}.{name}.json"));
        std::fs::write(&file, canon::canonical_json(doc)? + "\n")
            .map_err(|e| format!("cannot write {}: {e}", file.display()))?;
    }
    println!(
        "twin_run minimize: {id}: FAIL {kind}; {before} -> {} deletable elements ({tried} candidates tried); wrote {min_id}.scenario.json and {min_id}.fixture.json",
        count_deletable(&current)
    );
    Ok(())
}
