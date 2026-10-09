//! Canonical form of vocabulary v2 runs (docs/CANONICAL_VOCABULARY_V2.md
//! sections 8-9): RFC 8785 canonical JSON, event footprints, the
//! independence relation, Foata layering and digests.
//!
//! This module is engine-neutral: it sees only vocabulary events (JSON
//! objects with a `"k"` kind), never asupersync types. Malformed input is an
//! error, never a panic: the harness fails closed and reports it.

use serde_json::Value;
use sha2::{Digest, Sha256};

pub type CanonResult<T> = Result<T, String>;

/// Canonical JSON (RFC 8785) for the subset the vocabulary uses: null,
/// booleans, integers, strings, arrays, objects.
pub fn canonical_json(value: &Value) -> CanonResult<String> {
    let mut out = String::new();
    write_canonical(value, &mut out)?;
    Ok(out)
}

fn write_canonical(value: &Value, out: &mut String) -> CanonResult<()> {
    match value {
        Value::Null => out.push_str("null"),
        Value::Bool(b) => out.push_str(if *b { "true" } else { "false" }),
        Value::Number(n) => {
            if !(n.is_u64() || n.is_i64()) {
                return Err(format!("canonical JSON admits integers only, got {n}"));
            }
            out.push_str(&n.to_string());
        }
        Value::String(s) => write_string(s, out),
        Value::Array(items) => {
            out.push('[');
            for (i, item) in items.iter().enumerate() {
                if i > 0 {
                    out.push(',');
                }
                write_canonical(item, out)?;
            }
            out.push(']');
        }
        Value::Object(map) => {
            // RFC 8785 orders keys by their UTF-16 code units.
            let mut keys: Vec<&String> = map.keys().collect();
            keys.sort_by(|a, b| a.encode_utf16().cmp(b.encode_utf16()));
            out.push('{');
            for (i, key) in keys.iter().enumerate() {
                if i > 0 {
                    out.push(',');
                }
                write_string(key, out);
                out.push(':');
                write_canonical(&map[key.as_str()], out)?;
            }
            out.push('}');
        }
    }
    Ok(())
}

fn write_string(s: &str, out: &mut String) {
    out.push('"');
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\u{08}' => out.push_str("\\b"),
            '\u{0c}' => out.push_str("\\f"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
}

/// `"sha256:" + lowercase hex` of the canonical bytes.
pub fn digest(value: &Value) -> CanonResult<String> {
    let bytes = canonical_json(value)?;
    let hash = Sha256::digest(bytes.as_bytes());
    let mut hex = String::with_capacity(71);
    hex.push_str("sha256:");
    for byte in hash {
        hex.push_str(&format!("{byte:02x}"));
    }
    Ok(hex)
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Mode {
    Read,
    Write,
}

/// One resource access: a typed resource name (`task:<n>`, `region:<n>`,
/// `obligation:<n>`, `timer:<n>`, `clock`) and a mode.
type Access = (String, Mode);

fn field<'a>(event: &'a Value, key: &str) -> CanonResult<&'a str> {
    event
        .get(key)
        .and_then(Value::as_str)
        .ok_or_else(|| format!("vocabulary event lacks string field {key:?}: {event}"))
}

/// Resource footprint of a vocabulary event (section 8).
fn footprint(event: &Value) -> CanonResult<Vec<Access>> {
    let w = |r: String| (r, Mode::Write);
    let r = |r: String| (r, Mode::Read);
    let kind = field(event, "k")?;
    Ok(match kind {
        "task.spawned" | "task.completed" => vec![
            w(format!("task:{}", field(event, "task")?)),
            r(format!("region:{}", field(event, "region")?)),
        ],
        "cancel.requested" => vec![
            w(format!("task:{}", field(event, "task")?)),
            w(format!("region:{}", field(event, "region")?)),
        ],
        "region.created" => {
            let mut fp = vec![w(format!("region:{}", field(event, "region")?))];
            if let Some(parent) = event.get("parent").and_then(Value::as_str) {
                fp.push(r(format!("region:{parent}")));
            }
            fp
        }
        "region.close_begin" | "region.closed" | "region.cancelled" => {
            vec![w(format!("region:{}", field(event, "region")?))]
        }
        "obligation.reserved"
        | "obligation.committed"
        | "obligation.aborted"
        | "obligation.leaked" => vec![
            w(format!("obligation:{}", field(event, "obligation")?)),
            r(format!("task:{}", field(event, "task")?)),
            r(format!("region:{}", field(event, "region")?)),
        ],
        "obligation.handoff" => vec![
            w(format!("obligation:{}", field(event, "obligation")?)),
            w(format!("task:{}", field(event, "from_task")?)),
            w(format!("task:{}", field(event, "to_task")?)),
            w(format!("region:{}", field(event, "from_region")?)),
            w(format!("region:{}", field(event, "to_region")?)),
        ],
        "timer.scheduled" | "timer.fired" | "timer.cancelled" => {
            vec![
                w(format!("timer:{}", field(event, "timer")?)),
                r("clock".to_string()),
            ]
        }
        "user.trace" => vec![],
        other => return Err(format!("not a vocabulary v2 event kind: {other:?}")),
    })
}

/// The independence relation for two events at distinct stream positions.
/// `canonical_trace` applies it through footprints directly; this form is
/// what the normative table vectors are checked against.
#[cfg(test)]
pub fn independent(a: &Value, b: &Value) -> CanonResult<bool> {
    let fa = footprint(a)?;
    let fb = footprint(b)?;
    if fa.is_empty() || fb.is_empty() {
        return Ok(true);
    }
    Ok(!fa.iter().any(|(ra, ma)| {
        fb.iter()
            .any(|(rb, mb)| ra == rb && (*ma == Mode::Write || *mb == Mode::Write))
    }))
}

/// Foata canonical form (section 8): layer each event by the single-pass
/// rule over the emission-ordered stream, then sort each layer by the
/// canonical bytes of its events.
pub fn canonical_trace(events: &[Value]) -> CanonResult<Vec<Vec<Value>>> {
    use std::collections::HashMap;
    // Per resource: (highest layer of any access, highest layer of a write).
    let mut highest: HashMap<String, (Option<usize>, Option<usize>)> = HashMap::new();
    let mut layers: Vec<Vec<(String, Value)>> = Vec::new();
    for event in events {
        let fp = footprint(event)?;
        let mut layer = 0usize;
        for (resource, mode) in &fp {
            if let Some((any, write)) = highest.get(resource) {
                let dep = match mode {
                    Mode::Write => *any,
                    Mode::Read => *write,
                };
                if let Some(d) = dep {
                    layer = layer.max(d + 1);
                }
            }
        }
        for (resource, mode) in &fp {
            let entry = highest.entry(resource.clone()).or_insert((None, None));
            entry.0 = Some(entry.0.map_or(layer, |v| v.max(layer)));
            if *mode == Mode::Write {
                entry.1 = Some(entry.1.map_or(layer, |v| v.max(layer)));
            }
        }
        if layers.len() <= layer {
            layers.resize_with(layer + 1, Vec::new);
        }
        layers[layer].push((canonical_json(event)?, event.clone()));
    }
    Ok(layers
        .into_iter()
        .map(|mut layer| {
            layer.sort_by(|a, b| a.0.cmp(&b.0));
            layer.into_iter().map(|(_, event)| event).collect()
        })
        .collect())
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn canonical_json_sorts_keys_and_escapes() {
        let v = json!({"b": 1, "a": [true, null, "x\"y\n\u{1}"], "aa": {"z": 0, "y": -2}});
        assert_eq!(
            canonical_json(&v).unwrap(),
            r#"{"a":[true,null,"x\"y\n\u0001"],"aa":{"y":-2,"z":0},"b":1}"#
        );
        assert!(canonical_json(&json!(1.5)).is_err(), "floats are rejected");
    }

    #[test]
    fn digest_is_sha256_of_canonical_bytes() {
        // sha256("{}")
        assert_eq!(
            digest(&json!({})).unwrap(),
            "sha256:44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a"
        );
    }

    #[test]
    fn unknown_kind_is_an_error() {
        assert!(
            independent(
                &json!({"k": "task.polled", "task": "t"}),
                &json!({"k": "user.trace", "message": ""})
            )
            .is_err()
        );
    }

    /// The normative vectors shared with the C table test (W1.2).
    #[test]
    fn independence_table_vectors() {
        let path = concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../../tests/conformance/vocab_v2_independence_table.json"
        );
        let table: Value = serde_json::from_str(&std::fs::read_to_string(path).unwrap()).unwrap();
        let reason = table["reason_fixture"].clone();
        let resolve = |mut ev: Value| {
            if ev.get("reason") == Some(&json!("$reason")) {
                ev["reason"] = reason.clone();
            }
            ev
        };
        let cases = table["cases"].as_array().unwrap();
        assert_eq!(cases.len(), 22);
        for case in cases {
            let a = resolve(case["a"].clone());
            let b = resolve(case["b"].clone());
            let expected = case["independent"].as_bool().unwrap();
            assert_eq!(
                independent(&a, &b).unwrap(),
                expected,
                "case {}",
                case["id"]
            );
            assert_eq!(
                independent(&b, &a).unwrap(),
                expected,
                "case {} (symmetry)",
                case["id"]
            );
        }
    }

    #[test]
    fn foata_layers_match_the_spec_example() {
        // The worked example in schemas/canonical_vocabulary_v2.json.
        let schema: Value = serde_json::from_str(
            &std::fs::read_to_string(concat!(
                env!("CARGO_MANIFEST_DIR"),
                "/../../schemas/canonical_vocabulary_v2.json"
            ))
            .unwrap(),
        )
        .unwrap();
        let expected = schema["examples"][0]["trace"].clone();
        let ev = |k: &str, rest: Value| {
            let mut e = rest;
            e["k"] = json!(k);
            e
        };
        // One emission order of the spike scenario.
        let stream = vec![
            ev("region.created", json!({"region": "root", "parent": null})),
            ev(
                "task.spawned",
                json!({"task": "t.producer", "region": "root"}),
            ),
            ev(
                "task.spawned",
                json!({"task": "t.consumer", "region": "root"}),
            ),
            ev(
                "task.spawned",
                json!({"task": "t.waiter", "region": "root"}),
            ),
            ev(
                "user.trace",
                json!({"message": "oneshot::reserve creating permit"}),
            ),
            ev(
                "obligation.reserved",
                json!({"obligation": "t.consumer/o1", "task": "t.consumer", "region": "root", "kind": "SendPermit"}),
            ),
            ev(
                "obligation.committed",
                json!({"obligation": "t.consumer/o1", "task": "t.consumer", "region": "root", "kind": "SendPermit"}),
            ),
            ev(
                "task.completed",
                json!({"task": "t.producer", "region": "root"}),
            ),
            ev(
                "task.completed",
                json!({"task": "t.consumer", "region": "root"}),
            ),
            ev(
                "user.trace",
                json!({"message": "oneshot::recv received value"}),
            ),
            ev(
                "task.completed",
                json!({"task": "t.waiter", "region": "root"}),
            ),
        ];
        assert_eq!(
            serde_json::to_value(canonical_trace(&stream).unwrap()).unwrap(),
            expected
        );
    }
}
