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

use std::io::Write;
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

/// Rust's fixture for `scenario`, or None when Rust cannot run it.
fn capture(scenario: &Value, provenance: &Value) -> Option<Value> {
    let mut fixture = run::run_scenario(scenario).ok()?;
    fixture["provenance"] = provenance.clone();
    Some(fixture)
}

/// The four leading counts of the runner's comparison summary
/// (`asx-conformance: compared=N pass=N fail=N error=N`) in that order,
/// whatever `key=N` fields follow them: the runner appends
/// `forced_replayed`, `forced_unavailable` and `skipped_forbidden`. None for
/// any other line, including one with anything but `key=N` after the counts.
fn summary_counts(line: &str) -> Option<[u64; 4]> {
    let mut fields = line.strip_prefix("asx-conformance: ")?.split(' ');
    let mut counts = [0u64; 4];
    for (slot, name) in counts.iter_mut().zip(["compared", "pass", "fail", "error"]) {
        let value = fields.next()?.strip_prefix(name)?.strip_prefix('=')?;
        *slot = value.parse().ok()?;
    }
    for field in fields {
        let (key, value) = field.split_once('=')?;
        let key_ok = !key.is_empty() && key.bytes().all(|b| b.is_ascii_lowercase() || b == b'_');
        if !key_ok || value.parse::<u64>().is_err() {
            return None;
        }
    }
    Some(counts)
}

/// A completed single-fixture comparison. A PASS or an interpreter ERROR is
/// not a semantic reproduction. Inconsistent status/output is a broken
/// runner, not a candidate the minimizer may accept or silently discard.
fn comparison_kind(code: Option<i32>, stdout: &str, id: &str) -> Result<Option<String>, String> {
    let results: Vec<&str> = stdout
        .lines()
        .filter(|line| {
            line.starts_with("PASS ") || line.starts_with("FAIL ") || line.starts_with("ERROR ")
        })
        .collect();
    let summaries: Vec<&str> = stdout
        .lines()
        .filter(|line| line.starts_with("asx-conformance: "))
        .collect();
    if results.is_empty() || summaries.len() != 1 {
        return Err("runner must report a result and exactly one comparison summary".into());
    }
    // A scenario that fails reports one FAIL line per check that failed
    // (trace and snapshot, then the schedule certificate and the replay of
    // Rust's schedule): the first is the divergence the minimizer keeps. A
    // PASS or an ERROR is the only line.
    if results.len() > 1 && !results.iter().all(|line| line.starts_with("FAIL ")) {
        return Err("runner must report exactly one result unless every one is a FAIL".into());
    }
    let result = results[0];
    let counts = summary_counts(summaries[0]);
    if results.len() > 1 {
        let prefix = format!("FAIL {id}: ");
        if results.iter().any(|line| !line.starts_with(&prefix)) {
            return Err("runner result and scenario id disagree".into());
        }
    }
    if code == Some(0)
        && result.starts_with(&format!("PASS {id} sha256:"))
        && counts == Some([1, 1, 0, 0])
    {
        return Ok(None);
    }
    if code == Some(1)
        && result.starts_with(&format!("ERROR {id}: "))
        && counts == Some([1, 0, 0, 1])
    {
        return Ok(None);
    }
    if code == Some(1)
        && counts == Some([1, 0, 1, 0])
        && let Some(what) = result.strip_prefix(&format!("FAIL {id}: "))
        && let Some(kind) = what.split_whitespace().next()
    {
        return Ok(Some(kind.to_string()));
    }
    Err("runner exit status, scenario id, result and comparison summary disagree".into())
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
    let id = fixture["scenario_id"]
        .as_str()
        .ok_or("captured fixture has no scenario_id")?;
    let log_path = work.join("runner.log");
    let mut log = std::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(&log_path)
        .map_err(|e| format!("cannot open {}: {e}", log_path.display()))?;
    writeln!(log, "scenario={id} status={}\nstdout:", output.status)
        .and_then(|()| log.write_all(&output.stdout))
        .and_then(|()| writeln!(log, "\nstderr:"))
        .and_then(|()| log.write_all(&output.stderr))
        .and_then(|()| writeln!(log))
        .map_err(|e| format!("cannot retain {}: {e}", log_path.display()))?;
    let stdout = std::str::from_utf8(&output.stdout).map_err(|e| {
        format!(
            "runner stdout is not UTF-8: {e}; see {}",
            log_path.display()
        )
    })?;
    comparison_kind(output.status.code(), stdout, id).map_err(|e| {
        format!(
            "{e}; runner={} status={}; see {}\nstderr: {}",
            runner.display(),
            output.status,
            log_path.display(),
            String::from_utf8_lossy(&output.stderr)
        )
    })
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

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn comparator_accepts_completed_pass_fail_and_interpreter_error() {
        let pass = "PASS x sha256:abc\nasx-conformance: compared=1 pass=1 fail=0 error=0\n";
        let fail = "FAIL x: trace\n  first_diff: event\nasx-conformance: compared=1 pass=0 fail=1 error=0\n";
        let error = "ERROR x: C run failed: unsupported op\nasx-conformance: compared=1 pass=0 fail=0 error=1\n";
        assert_eq!(comparison_kind(Some(0), pass, "x"), Ok(None));
        assert_eq!(
            comparison_kind(Some(1), fail, "x"),
            Ok(Some("trace".into()))
        );
        assert_eq!(comparison_kind(Some(1), error, "x"), Ok(None));
    }

    /// The runner appends fields to the four counts (`forced_replayed`,
    /// `forced_unavailable`, `skipped_forbidden`); they must not matter.
    #[test]
    fn comparator_accepts_the_fields_the_runner_appends_to_its_summary() {
        let extra = " forced_replayed=1 forced_unavailable=0 skipped_forbidden=0";
        let pass = format!(
            "PASS x sha256:abc\nasx-conformance: compared=1 pass=1 fail=0 error=0{extra}\n"
        );
        let fail = format!(
            "FAIL x: snapshot\n  first_diff: tasks\nasx-conformance: compared=1 pass=0 fail=1 error=0{extra}\n"
        );
        let error = format!(
            "ERROR x: C run failed: unsupported op\nasx-conformance: compared=1 pass=0 fail=0 error=1{extra}\n"
        );
        assert_eq!(comparison_kind(Some(0), &pass, "x"), Ok(None));
        assert_eq!(
            comparison_kind(Some(1), &fail, "x"),
            Ok(Some("snapshot".into()))
        );
        assert_eq!(comparison_kind(Some(1), &error, "x"), Ok(None));
        // The counts still decide: appended fields do not make a pass a fail.
        assert!(comparison_kind(Some(1), &pass, "x").is_err());
        assert!(comparison_kind(Some(0), &fail, "x").is_err());
    }

    #[test]
    fn summary_counts_ignore_appended_key_value_fields_and_nothing_else() {
        let base = "asx-conformance: compared=1 pass=0 fail=1 error=0";
        assert_eq!(summary_counts(base), Some([1, 0, 1, 0]));
        assert_eq!(
            summary_counts(&format!("{base} forced_replayed=3 skipped_forbidden=0")),
            Some([1, 0, 1, 0])
        );
        for line in [
            "",
            "asx-conformance: compared=1 pass=0 fail=1",
            "asx-conformance: pass=0 compared=1 fail=1 error=0",
            "asx-conformance: compared=1 pass=0 fail=1 error=x",
            "asx-conformance: compared=1 pass=0 fail=1 error=0 junk",
            "asx-conformance: compared=1 pass=0 fail=1 error=0 forced_replayed=",
            "asx-conformance: compared=1 pass=0 fail=1 error=0 =3",
            "asx-conformance: compared=1 pass=0 fail=1 error=0 Forced=3",
            "asx-conformance sweep: compared=1 pass=0 fail=1 error=0",
        ] {
            assert_eq!(summary_counts(line), None, "{line:?}");
        }
    }

    #[test]
    fn comparator_rejects_incomplete_inconsistent_and_crashed_runs() {
        let fail = "FAIL x: snapshot\nasx-conformance: compared=1 pass=0 fail=1 error=0\n";
        for code in [Some(0), Some(2), Some(127), None] {
            assert!(comparison_kind(code, fail, "x").is_err());
        }
        for output in [
            "",
            "FAIL x: snapshot\n",
            "asx-conformance: compared=0 pass=0 fail=0 error=0\n",
            "FAIL x: \nasx-conformance: compared=1 pass=0 fail=1 error=0\n",
            "FAIL x: snapshot\nasx-conformance: compared=2 pass=0 fail=2 error=0\n",
            "FAIL x: snapshot\nasx-conformance: compared=1 pass=0 fail=0 error=1\n",
            "FAIL x: trace\nFAIL y: snapshot\nasx-conformance: compared=1 pass=0 fail=1 error=0\n",
            "FAIL x: trace\nPASS x sha256:abc\nasx-conformance: compared=1 pass=0 fail=1 error=0\n",
            "FAIL x: trace\nERROR x: C run failed\nasx-conformance: compared=1 pass=0 fail=1 error=0\n",
            "FAIL x: trace\nasx-conformance: compared=1 pass=0 fail=1 error=0\nasx-conformance: compared=1 pass=0 fail=1 error=0\n",
        ] {
            assert!(comparison_kind(Some(1), output, "x").is_err());
        }
        assert!(comparison_kind(Some(1), fail, "other-scenario").is_err());
    }

    /// The runner reports one FAIL line per failed check of a scenario: the
    /// trace, then the schedule certificate and the replay of Rust's
    /// schedule in C. The minimizer keeps the first.
    #[test]
    fn comparator_takes_the_first_of_several_fail_lines_of_one_scenario() {
        let fails = "FAIL x: trace\n  trace[1] differs\nFAIL x: schedule certificate\n    rust: a\n    c: b\nFAIL x: replaying Rust's schedule in C: forced schedule diverged\nasx-conformance: compared=1 pass=0 fail=1 error=0 forced_replayed=0 forced_unavailable=1 skipped_forbidden=0\n";
        assert_eq!(
            comparison_kind(Some(1), fails, "x"),
            Ok(Some("trace".into()))
        );
        assert!(comparison_kind(Some(0), fails, "x").is_err());
        assert!(comparison_kind(Some(1), fails, "y").is_err());
    }

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
