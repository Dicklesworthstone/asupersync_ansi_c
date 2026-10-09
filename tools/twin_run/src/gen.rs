//! Seeded generator of asx.scenario.v2 scenarios for Rust-vs-C differential
//! fuzzing (bd-ij9w).
//!
//! Every generated scenario uses only steps both oracles interpret
//! (docs/SCENARIO_DSL_V2.md §3, §7), keeps names unique, puts only
//! non-blocking steps inside `masked`, and is bounded by `lab.max_steps`, so
//! a deadlock ends deterministically and both engines must agree on the
//! non-quiescent end state. A scenario Rust cannot run (twin_run fails it)
//! is simply not captured; one Rust runs and C does not is a finding.
//!
//! Child regions: a task opens at most one at a time (two in all), may
//! spawn into it, and closes it or leaves it to the drop backstop.
//!
//! Left out on purpose: multi-permit acquire, race
//! (same-round ties are an RNG pick in Rust and the lowest index in C, a
//! known divergence, bd-g652), region_limits, actors and supervision.
//! join_all, first_ok and quorum groups are generated.

use serde_json::{Value, json};

/// splitmix64: tiny, seedable, no dependency.
pub struct Rng(u64);

impl Rng {
    pub fn new(seed: u64) -> Self {
        Rng(seed)
    }

    pub fn next_u64(&mut self) -> u64 {
        self.0 = self.0.wrapping_add(0x9E37_79B9_7F4A_7C15);
        let mut z = self.0;
        z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
        z ^ (z >> 31)
    }

    /// Uniform in 0..n (n > 0).
    pub fn below(&mut self, n: u64) -> u64 {
        self.next_u64() % n
    }

    /// True with probability pct/100.
    pub fn chance(&mut self, pct: u64) -> bool {
        self.below(100) < pct
    }

    fn pick<'a, T>(&mut self, items: &'a [T]) -> &'a T {
        &items[usize::try_from(self.below(items.len() as u64)).unwrap_or(0)]
    }
}

const CANCEL_KINDS: [&str; 4] = ["User", "Shutdown", "Timeout", "Deadline"];
const OBLIGATION_KINDS: [&str; 3] = ["SendPermit", "Ack", "Lease"];
const ABORT_REASONS: [&str; 3] = ["Cancel", "Error", "Explicit"];
const ERR_STATUSES: [&str; 2] = ["ASX_E_TIMED_OUT", "ASX_E_INVALID_STATE"];

/// What the scenario declares that programs may use.
struct World {
    mutex: bool,
    semaphore: bool,
    notify: bool,
    /// (channel, producer task, consumer task)
    mpsc: Option<(String, String, String)>,
}

/// Per-task generation state: what the program holds so far.
#[derive(Default)]
struct Held {
    obligations: Vec<String>,
    children: Vec<String>,
    /// Child regions opened and not closed yet (at most one at a time).
    regions: Vec<String>,
    opened: u32,
    groups: u32,
    mutex: bool,
    permit: bool,
    reserves: u32,
    spawns: u32,
    messages: u32,
    sends: u32,
}

fn sleep_step(rng: &mut Rng) -> Value {
    json!({"op": "sleep", "ns": 50 * (1 + rng.below(6))})
}

fn checkpoint_step(rng: &mut Rng) -> Value {
    json!({"op": "checkpoint", "on_cancel": if rng.chance(50) { "return" } else { "continue" }})
}

/// A small child program: no names of its own, nothing held.
fn child_program(rng: &mut Rng) -> Vec<Value> {
    let mut steps = Vec::new();
    for _ in 0..1 + rng.below(3) {
        steps.push(match rng.below(3) {
            0 => json!({"op": "yield"}),
            1 => sleep_step(rng),
            _ => checkpoint_step(rng),
        });
    }
    if rng.chance(30) {
        steps.push(
            json!({"op": "return", "outcome": {"tag": "err", "status": *rng.pick(&ERR_STATUSES)}}),
        );
    }
    steps
}

/// One step of task `me`'s program, given what it holds.
fn step(rng: &mut Rng, world: &World, me: &str, held: &mut Held) -> Value {
    loop {
        match rng.below(19) {
            0 | 1 => return json!({"op": "yield"}),
            2 | 3 => return sleep_step(rng),
            4 | 5 => return checkpoint_step(rng),
            6 => {
                held.messages += 1;
                return json!({"op": "trace", "message": format!("{me}:m{}", held.messages)});
            }
            7 => {
                held.reserves += 1;
                let name = format!("{me}.o{}", held.reserves);
                held.obligations.push(name.clone());
                return json!({"op": "reserve", "kind": *rng.pick(&OBLIGATION_KINDS), "as": name});
            }
            8 if !held.obligations.is_empty() => {
                let i = usize::try_from(rng.below(held.obligations.len() as u64)).unwrap_or(0);
                let name = held.obligations.remove(i);
                return match rng.below(3) {
                    0 => json!({"op": "commit", "obligation": name}),
                    1 => {
                        json!({"op": "abort", "obligation": name, "reason": *rng.pick(&ABORT_REASONS)})
                    }
                    _ => json!({"op": "leak", "obligation": name}),
                };
            }
            9 if held.spawns < 2 => {
                held.spawns += 1;
                let name = format!("{me}.c{}", held.spawns);
                held.children.push(name.clone());
                let mut s = json!({"op": "spawn", "as": name, "program": child_program(rng)});
                if !held.regions.is_empty() && rng.chance(60) {
                    s["region"] = json!(held.regions[0]);
                }
                return s;
            }
            10 if !held.children.is_empty() => {
                // An aborted child stays joinable.
                if rng.chance(30) {
                    let i = usize::try_from(rng.below(held.children.len() as u64)).unwrap_or(0);
                    return json!({"op": "abort_task", "task": held.children[i],
                                  "kind": *rng.pick(&CANCEL_KINDS)});
                }
                let name = held.children.remove(0);
                return json!({"op": "join", "task": name});
            }
            11 if world.mutex => {
                held.mutex = !held.mutex;
                return if held.mutex {
                    json!({"op": "mutex_lock", "mutex": "m"})
                } else {
                    json!({"op": "mutex_unlock", "mutex": "m"})
                };
            }
            12 if world.semaphore => {
                held.permit = !held.permit;
                return if held.permit {
                    json!({"op": "sem_acquire", "semaphore": "s", "count": 1})
                } else {
                    json!({"op": "sem_release", "semaphore": "s"})
                };
            }
            13 if world.notify => {
                return if rng.chance(70) {
                    json!({"op": "notify_one", "notify": "n"})
                } else {
                    json!({"op": "notify_wait", "notify": "n"})
                };
            }
            14 => {
                held.messages += 1;
                return json!({"op": "masked", "steps": [
                    {"op": "checkpoint", "on_cancel": "continue"},
                    {"op": "trace", "message": format!("{me}:m{}", held.messages)},
                ]});
            }
            15 => {
                if let Some((ch, producer, consumer)) = &world.mpsc {
                    if me == producer {
                        held.sends += 1;
                        if rng.chance(25) {
                            return json!({"op": "try_send", "channel": ch, "value": held.sends});
                        }
                        if rng.chance(50) {
                            return json!({"op": "send", "channel": ch, "value": held.sends});
                        }
                        held.reserves += 1;
                        // The caller sends the permit at once: a held permit
                        // would keep the sender borrowed (DSL §3.6).
                        return json!({"op": "reserve_send", "channel": ch, "as": format!("{me}.p{}", held.reserves)});
                    }
                    if me == consumer {
                        let op = if rng.chance(30) { "try_recv" } else { "recv" };
                        return json!({"op": op, "channel": ch});
                    }
                }
            }
            // A child region opened here is closed by a later step, or by
            // the drop backstop when the program ends with it open.
            16 if held.regions.is_empty() && held.opened < 2 => {
                held.opened += 1;
                let name = format!("{me}.r{}", held.opened);
                held.regions.push(name.clone());
                let mut s = json!({"op": "open_region", "as": name});
                if rng.chance(20) {
                    s["budget"] = json!({"poll_quota": 2 + rng.below(6)});
                }
                return s;
            }
            17 if !held.regions.is_empty() => {
                if rng.chance(25) {
                    return json!({"op": "cancel_region", "region": held.regions[0],
                                  "kind": *rng.pick(&CANCEL_KINDS)});
                }
                let name = held.regions.remove(0);
                return json!({"op": "close_region", "region": name});
            }
            // One task group per task: join_all or first_ok over small
            // member programs (race is left out, see above).
            18 if held.groups == 0 => {
                held.groups += 1;
                let members: Vec<Value> = (0..1 + rng.below(3))
                    .map(|_| Value::Array(child_program(rng)))
                    .collect();
                return match rng.below(3) {
                    0 => json!({"op": "join_all", "members": members}),
                    1 => json!({"op": "first_ok", "members": members}),
                    _ => {
                        // Sometimes one more than the members: InvalidQuorum.
                        let needed = 1 + rng.below(members.len() as u64 + 1);
                        json!({"op": "quorum", "needed": needed, "members": members})
                    }
                };
            }
            _ => {}
        }
    }
}

fn budget(rng: &mut Rng) -> Value {
    if rng.chance(15) {
        json!({"poll_quota": 2 + rng.below(6)})
    } else {
        Value::Null
    }
}

/// Scenario `index` of the batch seeded `seed`.
pub fn scenario(seed: u64, index: u64) -> Value {
    let mut rng = Rng::new(seed ^ index.wrapping_mul(0xA24B_AED4_963E_E407));
    let task_count = 1 + rng.below(4);
    let tasks: Vec<String> = (0..task_count).map(|i| format!("t.{i}")).collect();
    let mut regions = vec![json!({"name": "r.main", "parent": "root", "budget": null})];
    let mut region_names = vec!["r.main"];
    if rng.chance(40) {
        regions.push(json!({"name": "r.side", "parent": "root", "budget": null}));
        region_names.push("r.side");
    }
    let world = World {
        mutex: rng.chance(35),
        semaphore: rng.chance(25),
        notify: rng.chance(20),
        mpsc: (task_count >= 2 && rng.chance(35))
            .then(|| ("ch".to_string(), tasks[0].clone(), tasks[1].clone())),
    };
    let mut sync = Vec::new();
    if world.mutex {
        sync.push(json!({"name": "m", "type": "mutex"}));
    }
    if world.semaphore {
        sync.push(json!({"name": "s", "type": "semaphore", "permits": 1 + rng.below(2)}));
    }
    if world.notify {
        sync.push(json!({"name": "n", "type": "notify"}));
    }
    let channels: Vec<Value> = world
        .mpsc
        .iter()
        .map(|(ch, tx, rx)| {
            json!({"name": ch, "type": "mpsc", "capacity": 1 + rng.below(2), "sender": tx, "receiver": rx})
        })
        .collect();

    let mut task_values = Vec::new();
    for me in &tasks {
        let mut held = Held::default();
        let mut program = Vec::new();
        for _ in 0..1 + rng.below(7) {
            let s = step(&mut rng, &world, me, &mut held);
            // A reserved send permit is sent at once (see step 15).
            if s["op"] == "reserve_send" {
                let permit = s["as"].clone();
                program.push(s);
                program.push(json!({"op": "permit_send", "permit": permit, "value": held.sends}));
            } else {
                program.push(s);
            }
        }
        if rng.chance(15) {
            program.push(json!({"op": "return", "outcome": {"tag": "err", "status": *rng.pick(&ERR_STATUSES)}}));
        }
        task_values.push(json!({
            "name": me,
            "region": *rng.pick(&region_names),
            "budget": budget(&mut rng),
            "program": program,
        }));
    }

    let mut script = Vec::new();
    let mut at = 0u64;
    for _ in 0..rng.below(3) {
        at += 50 * rng.below(5);
        let op = match rng.below(3) {
            0 => {
                json!({"op": "cancel_task", "task": *rng.pick(&tasks), "kind": *rng.pick(&CANCEL_KINDS)})
            }
            1 => {
                json!({"op": "cancel_region", "region": *rng.pick(&region_names), "kind": *rng.pick(&CANCEL_KINDS)})
            }
            _ => json!({"op": "advance", "ns": 50 * (1 + rng.below(4))}),
        };
        script.push(json!({"at_ns": at, "op": op}));
    }

    json!({
        "schema": "asx.scenario.v2",
        "id": format!("gen-{seed}-{index}"),
        "description": "generated (twin_run generate, bd-ij9w)",
        "unit": "scheduler",
        "rules": [],
        "seed": rng.next_u64() % 1_000_000,
        "lab": {"max_steps": 2000},
        "regions": regions,
        "channels": channels,
        "sync": sync,
        "tasks": task_values,
        "script": script,
        "expect": {"kind": "ok"},
    })
}
