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
//! Child regions: a task holds at most two at a time (two in all), may
//! spawn into either, and closes or cancels the older first or leaves them
//! to the drop backstop. The declared side regions are sometimes children
//! of `r.main`, so a driver cancel of `r.main` reaches sibling child
//! regions holding tasks, in an order both runtimes must agree on
//! (bd-e038).
//!
//! Contention: when the scenario has a mutex, rwlock (reads and writes) or
//! semaphore, most tasks open with a critical section (acquire, a few
//! steps that let others run,
//! release), so several tasks queue on one lock and every release must
//! hand it on in arrival order. Random lock toggles alone almost never
//! park two waiters at once: a lock granting newest-first passed 200
//! scenarios without them.
//!
//! Crowds: about one scenario in twelve has 17 to 32 tasks queueing on one
//! lock, more than the C runtime once let wait on it (bd-9kll.5.1); an
//! rwlock crowd can run 16 writer hand-offs in a row and reach the forced
//! reader turn (bd-rm2d).
//!
//! Channels and sync: besides mpsc (send / try_send / reserve with
//! permit_send or permit_abort, recv / try_recv, close of either end),
//! a scenario may connect its tasks with a oneshot, a broadcast or a watch
//! channel and declare a barrier, whose waits may fall short of its
//! parties.
//!
//! Semaphore acquires sometimes take 2 permits at once (all or nothing),
//! a held permit is sometimes forgotten instead of released, and the pool
//! sometimes grows (add_permits); about one scenario in ten sets admission
//! limits on a region.
//!
//! Task groups: join_all, first_ok, quorum and race (sometimes with a
//! deadline); a race's same-round tie is drawn from the owner's entropy in
//! both runtimes (bd-g652).
//!
//! Servers (DSL §3.8, bd-86np): a task may spawn up to two counter or echo
//! servers with a mailbox of 1 to 4, cast to and call them and stop them;
//! a stopped server is never named again. Tasks never run in `root`, so a
//! call is never refused as a scenario error. Left out on purpose:
//! supervision, which neither oracle interprets.

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
    rwlock: bool,
    semaphore: bool,
    notify: bool,
    /// Parties of the barrier `b`.
    barrier: Option<u64>,
    /// (channel, producer task, consumer task)
    mpsc: Option<(String, String, String)>,
    /// (channel, sender task, receiver task)
    oneshot: Option<(String, String, String)>,
    /// (channel, sender task, subscriber tasks)
    broadcast: Option<(String, String, Vec<String>)>,
    /// (channel, sender task, subscriber tasks)
    watch: Option<(String, String, Vec<String>)>,
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
    /// This task holds a guard on the rwlock (one at a time: a second
    /// acquire by the holder could wait on itself).
    rw: bool,
    permit: bool,
    reserves: u32,
    spawns: u32,
    messages: u32,
    sends: u32,
    /// This task closed its end of the mpsc channel.
    closed: bool,
    /// This task used its end of the oneshot (it sends or receives once).
    oneshot_used: bool,
    /// This task waited at the barrier (each party arrives once).
    barrier_waited: bool,
    /// Servers this task spawned and has not stopped.
    servers: Vec<String>,
    server_spawns: u32,
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

/// An rwlock acquire: a write 40% of the time, else a read.
fn rw_acquire(rng: &mut Rng) -> Value {
    let op = if rng.chance(40) {
        "rwlock_write"
    } else {
        "rwlock_read"
    };
    json!({"op": op, "rwlock": "rw"})
}

/// One step of task `me`'s program, given what it holds.
fn step(rng: &mut Rng, world: &World, me: &str, held: &mut Held) -> Value {
    loop {
        match rng.below(27) {
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
                    s["region"] = json!(rng.pick(&held.regions));
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
                // try_join consumes the handle whether or not the child is
                // done (a running child gives TASK_NOT_COMPLETED).
                if rng.chance(25) {
                    return json!({"op": "try_join", "task": name});
                }
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
                // Sometimes the pool grows; sometimes a held permit is
                // forgotten instead of released, so the pool shrinks.
                if rng.chance(10) {
                    return json!({"op": "sem_add_permits", "semaphore": "s",
                                  "count": 1 + rng.below(2)});
                }
                held.permit = !held.permit;
                return if held.permit {
                    json!({"op": "sem_acquire", "semaphore": "s", "count": permit_count(rng)})
                } else if rng.chance(15) {
                    json!({"op": "sem_forget", "semaphore": "s"})
                } else {
                    json!({"op": "sem_release", "semaphore": "s"})
                };
            }
            13 if world.notify => {
                return match rng.below(10) {
                    0..=5 => json!({"op": "notify_one", "notify": "n"}),
                    6 => json!({"op": "notify_all", "notify": "n"}),
                    _ => json!({"op": "notify_wait", "notify": "n"}),
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
                    if held.closed {
                        continue;
                    }
                    // Dropping an end wakes and fails the other side.
                    if (me == producer || me == consumer) && rng.chance(8) {
                        held.closed = true;
                        let op = if me == producer {
                            "close_sender"
                        } else {
                            "close_receiver"
                        };
                        return json!({"op": op, "channel": ch});
                    }
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
            16 if held.regions.len() < 2 && held.opened < 2 => {
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
            // One task group per task: join_all, first_ok, race or quorum
            // over small member programs.
            18 if held.groups == 0 => {
                held.groups += 1;
                let members: Vec<Value> = (0..1 + rng.below(3))
                    .map(|_| Value::Array(child_program(rng)))
                    .collect();
                return match rng.below(4) {
                    0 => json!({"op": "join_all", "members": members}),
                    1 => json!({"op": "first_ok", "members": members}),
                    2 => {
                        // Same-round ties are drawn from the owner's (or the
                        // deadline wrapper's) entropy in both runtimes.
                        let mut s = json!({"op": "race", "members": members});
                        if rng.chance(25) {
                            s["deadline_ns"] = json!(50 * (1 + rng.below(6)));
                        }
                        s
                    }
                    _ => {
                        // Sometimes one more than the members: InvalidQuorum.
                        let needed = 1 + rng.below(members.len() as u64 + 1);
                        json!({"op": "quorum", "needed": needed, "members": members})
                    }
                };
            }
            19 if !held.oneshot_used => {
                if let Some((ch, sender, receiver)) = &world.oneshot {
                    if me == sender {
                        held.oneshot_used = true;
                        held.sends += 1;
                        return json!({"op": "oneshot_send", "channel": ch, "value": held.sends});
                    }
                    if me == receiver {
                        held.oneshot_used = true;
                        return json!({"op": "oneshot_recv", "channel": ch});
                    }
                }
            }
            20 => {
                if let Some((ch, sender, subscribers)) = &world.broadcast {
                    if me == sender {
                        held.sends += 1;
                        return json!({"op": "broadcast_send", "channel": ch, "value": held.sends});
                    }
                    if subscribers.iter().any(|s| s == me) {
                        return json!({"op": "broadcast_recv", "channel": ch});
                    }
                }
            }
            21 => {
                if let Some((ch, sender, subscribers)) = &world.watch {
                    if me == sender {
                        held.sends += 1;
                        return json!({"op": "watch_send", "channel": ch, "value": held.sends});
                    }
                    if subscribers.iter().any(|s| s == me) {
                        return json!({"op": "watch_changed", "channel": ch});
                    }
                }
            }
            22 if world.barrier.is_some() && !held.barrier_waited => {
                held.barrier_waited = true;
                return json!({"op": "barrier_wait", "barrier": "b"});
            }
            23 => return json!({"op": "sleep_until", "at_ns": 50 * (1 + rng.below(12))}),
            24 if world.rwlock => {
                held.rw = !held.rw;
                return if held.rw {
                    rw_acquire(rng)
                } else {
                    json!({"op": "rwlock_unlock", "rwlock": "rw"})
                };
            }
            25 if held.servers.len() < 2 && (held.servers.is_empty() || rng.chance(25)) => {
                held.server_spawns += 1;
                let name = format!("{me}.srv{}", held.server_spawns);
                held.servers.push(name.clone());
                let behavior = if rng.chance(50) { "counter" } else { "echo" };
                return json!({"op": "server_spawn", "as": name, "behavior": behavior,
                              "mailbox": 1 + rng.below(4)});
            }
            25 | 26 if !held.servers.is_empty() => return server_step(rng, held),
            _ => {}
        }
    }
}

/// A cast, call or stop on one of the task's live servers.
fn server_step(rng: &mut Rng, held: &mut Held) -> Value {
    let i = usize::try_from(rng.below(held.servers.len() as u64)).unwrap_or(0);
    let server = held.servers[i].clone();
    match rng.below(10) {
        0..=3 => {
            held.sends += 1;
            json!({"op": "cast", "server": server, "value": held.sends})
        }
        4..=7 => json!({"op": "call", "server": server, "request": rng.below(100)}),
        _ => {
            held.servers.remove(i);
            json!({"op": "server_stop", "server": server})
        }
    }
}

/// Permits one semaphore acquire takes, all or nothing: usually 1,
/// sometimes 2 (the semaphore holds 1 or 2; asking for more than it holds
/// waits until lab.max_steps, and both engines must agree on that too).
fn permit_count(rng: &mut Rng) -> u64 {
    if rng.chance(20) { 2 } else { 1 }
}

/// A critical section on one of the scenario's locks: acquire, one to
/// three steps that let other tasks run (they queue behind it), release.
/// It opens the program, so nothing is held yet and the random lock
/// toggles of `step` that follow start from released.
fn critical_section(rng: &mut Rng, world: &World, me: &str, held: &mut Held) -> Vec<Value> {
    let mut locks = Vec::new();
    if world.semaphore {
        locks.push("s");
    }
    if world.mutex {
        locks.push("m");
    }
    if world.rwlock {
        locks.push("rw");
    }
    let (acquire, release) = match *rng.pick(&locks) {
        "s" => (
            json!({"op": "sem_acquire", "semaphore": "s", "count": permit_count(rng)}),
            json!({"op": "sem_release", "semaphore": "s"}),
        ),
        "m" => (
            json!({"op": "mutex_lock", "mutex": "m"}),
            json!({"op": "mutex_unlock", "mutex": "m"}),
        ),
        _ => (
            rw_acquire(rng),
            json!({"op": "rwlock_unlock", "rwlock": "rw"}),
        ),
    };
    let mut steps = vec![acquire];
    for _ in 0..1 + rng.below(3) {
        steps.push(match rng.below(3) {
            0 => json!({"op": "yield"}),
            1 => sleep_step(rng),
            _ => {
                held.messages += 1;
                json!({"op": "trace", "message": format!("{me}:m{}", held.messages)})
            }
        });
    }
    steps.push(release);
    steps
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
    // A crowd: more tasks than the C runtime once let wait on one mutex or
    // semaphore (16), all queueing on that lock (bd-9kll.5.1). Rust's
    // waiter queues are unbounded; C's must serve them the same way.
    let crowd = rng.chance(8);
    let task_count = if crowd {
        17 + rng.below(16)
    } else {
        1 + rng.below(4)
    };
    let tasks: Vec<String> = (0..task_count).map(|i| format!("t.{i}")).collect();
    let mut regions = vec![json!({"name": "r.main", "parent": "root", "budget": null})];
    let mut region_names = vec!["r.main"];
    if rng.chance(40) {
        let parent = if rng.chance(50) { "r.main" } else { "root" };
        regions.push(json!({"name": "r.side", "parent": parent, "budget": null}));
        region_names.push("r.side");
        if parent == "r.main" && rng.chance(50) {
            regions.push(json!({"name": "r.side2", "parent": "r.main", "budget": null}));
            region_names.push("r.side2");
        }
    }
    let world = if crowd {
        // One lock or two; an rwlock crowd mixes readers and writers, long
        // enough to reach the forced reader turn after 16 writers.
        let lock = rng.below(3);
        World {
            mutex: lock == 0,
            rwlock: lock == 1,
            semaphore: lock == 2 || rng.chance(30),
            notify: false,
            barrier: None,
            mpsc: None,
            oneshot: None,
            broadcast: None,
            watch: None,
        }
    } else {
        // Channels other than mpsc connect the last task to the others.
        let last = tasks[tasks.len() - 1].clone();
        let others: Vec<String> = tasks[..tasks.len() - 1].to_vec();
        let several = task_count >= 2;
        World {
            mutex: rng.chance(35),
            rwlock: rng.chance(25),
            semaphore: rng.chance(25),
            notify: rng.chance(20),
            // Fewer arrivals than parties leave the waiters parked until
            // lab.max_steps: both engines must agree on that end state too.
            barrier: (several && rng.chance(15)).then(|| 2 + rng.below(task_count - 1)),
            mpsc: (several && rng.chance(35))
                .then(|| ("ch".to_string(), tasks[0].clone(), tasks[1].clone())),
            oneshot: (several && rng.chance(15))
                .then(|| ("os".to_string(), last.clone(), tasks[0].clone())),
            broadcast: (several && rng.chance(15))
                .then(|| ("bc".to_string(), tasks[0].clone(), tasks[1..].to_vec())),
            watch: (several && rng.chance(15)).then(|| ("wt".to_string(), last, others)),
        }
    };
    let mut sync = Vec::new();
    if world.mutex {
        sync.push(json!({"name": "m", "type": "mutex"}));
    }
    if world.rwlock {
        sync.push(json!({"name": "rw", "type": "rwlock"}));
    }
    if world.semaphore {
        sync.push(json!({"name": "s", "type": "semaphore", "permits": 1 + rng.below(2)}));
    }
    if world.notify {
        sync.push(json!({"name": "n", "type": "notify"}));
    }
    if let Some(parties) = world.barrier {
        sync.push(json!({"name": "b", "type": "barrier", "parties": parties}));
    }
    let mut channels: Vec<Value> = world
        .mpsc
        .iter()
        .map(|(ch, tx, rx)| {
            json!({"name": ch, "type": "mpsc", "capacity": 1 + rng.below(2), "sender": tx, "receiver": rx})
        })
        .collect();
    if let Some((ch, tx, rx)) = &world.oneshot {
        channels.push(json!({"name": ch, "type": "oneshot", "sender": tx, "receiver": rx}));
    }
    if let Some((ch, tx, subs)) = &world.broadcast {
        channels.push(
            json!({"name": ch, "type": "broadcast", "capacity": 1 + rng.below(3),
                             "sender": tx, "subscribers": subs}),
        );
    }
    if let Some((ch, tx, subs)) = &world.watch {
        channels.push(
            json!({"name": ch, "type": "watch", "initial": 0, "sender": tx,
                             "subscribers": subs}),
        );
    }

    let mut task_values = Vec::new();
    for me in &tasks {
        let mut held = Held::default();
        let mut program = Vec::new();
        if crowd {
            // Queue on the lock, then a short tail that holds nothing (no
            // spawns: the crowd alone fills most of C's task arena).
            program.extend(critical_section(&mut rng, &world, me, &mut held));
            program.extend(child_program(&mut rng));
            task_values.push(json!({
                "name": me,
                "region": *rng.pick(&region_names),
                "budget": budget(&mut rng),
                "program": program,
            }));
            continue;
        }
        if (world.mutex || world.semaphore || world.rwlock) && rng.chance(75) {
            program.extend(critical_section(&mut rng, &world, me, &mut held));
        }
        for _ in 0..1 + rng.below(7) {
            let s = step(&mut rng, &world, me, &mut held);
            // A reserved send permit is sent (or aborted) at once (see
            // step 15).
            if s["op"] == "reserve_send" {
                let permit = s["as"].clone();
                program.push(s);
                if rng.chance(20) {
                    program.push(json!({"op": "permit_abort", "permit": permit}));
                } else {
                    program
                        .push(json!({"op": "permit_send", "permit": permit, "value": held.sends}));
                }
            } else if s["op"] == "server_spawn" {
                // A burst of casts and calls right after the spawn, before
                // the server first runs, fills small mailboxes (casts wait)
                // and leaves messages queued for a cancel to drain.
                program.push(s);
                for _ in 0..rng.below(6) {
                    if held.servers.is_empty() {
                        break;
                    }
                    program.push(server_step(&mut rng, &mut held));
                }
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
    // Admission limits on a region once the first burst has run: later
    // spawns, child regions and reservations past them are refused
    // (ASX_E_ADMISSION_LIMIT). A field left out is unlimited. max_tasks is
    // checked when the next lab step admits a spawn (bd-orxy), and a
    // permit's obligation when the poll that registered it returns
    // (bd-2fga), in both runtimes.
    if rng.chance(10) {
        let mut op = json!({"op": "region_limits", "region": *rng.pick(&region_names)});
        if rng.chance(50) {
            op["max_tasks"] = json!(rng.below(4));
        }
        if rng.chance(70) {
            op["max_children"] = json!(rng.below(3));
        }
        if rng.chance(70) {
            op["max_obligations"] = json!(rng.below(3));
        }
        script.push(json!({"at_ns": 0, "op": op}));
    }
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
