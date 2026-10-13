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
//! Channels and sync: besides mpsc (send / try_send / send_evict_oldest /
//! reserve with permit_send or permit_abort, the sender's closed wait,
//! recv / try_recv / recv_many, close of either end),
//! a scenario may connect its tasks with a oneshot, a broadcast (sometimes
//! with a second sender, a subscriber holding a clone) or a watch
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
//! call is never refused as a scenario error.
//!
//! Supervisors (DSL §3.8): a task may spawn one supervisor of one to three
//! children (any policy and restart modes, up to three restarts in a 1 s
//! or 60 s window, sometimes escalating) and later join it, or leave it to
//! the drop at its body's end, which aborts it. A child sometimes ends in an
//! err, a panic or (bd-9kll.8.7) `return cancelled`, and one in four creates
//! names of its own (a spawned child, a server, a nested supervisor).
//!
//! Cancel storms (bd-9kll.4.10): one scenario in sixteen is not drawn from
//! the stream above but is a storm, from a stream of its own (see
//! [`storm_scenario`]): 20 to 40 tasks asleep in a region (some in a child
//! of it, sometimes at three priorities) that the driver cancels at the
//! instant 1 to 3 spinners wake. The cancel lane then holds more entries
//! than the 16 dispatches the lab serves in a row, with the ready lane
//! never empty, so the streak limit yields to it (LabRuntime's
//! cancel_streak_limit) and the priority groups of the lanes are exercised.
//! No scenario above reached more than 10 cancel dispatches in a row.
//!
//! Timers (bd-9kll.7.1): `twin_run generate --shape timers` draws every
//! scenario of a batch from a stream of its own, [`timer_scenario`]: groups
//! of timers whose deadlines share one 1 ms wheel tick at a drawn level,
//! registered at different times, cancelled, moved to a new waker and
//! advanced past in one collect. The default shape never draws from it.

use serde_json::{Value, json};

/// splitmix64: tiny, seedable, no dependency. It also carries the time
/// quantum of the scenario it generates (see [`time_quantum`]): every
/// generated time is a multiple of it, which does not draw from the stream.
pub struct Rng {
    state: u64,
    /// The unit, in ns, that generated sleeps, deadlines and clock jumps
    /// are multiples of. 50 unless the scenario draws another.
    quantum: u64,
}

/// The base quantum: 50 ns, every generated time before the quantum existed.
const BASE_QUANTUM: u64 = 50;

impl Rng {
    pub fn new(seed: u64) -> Self {
        Rng {
            state: seed,
            quantum: BASE_QUANTUM,
        }
    }

    pub fn next_u64(&mut self) -> u64 {
        self.state = self.state.wrapping_add(0x9E37_79B9_7F4A_7C15);
        let mut z = self.state;
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

/// The time quantum of scenario `index` of batch `seed`, from a stream of
/// its own so the main one (and so every scenario whose quantum is 50)
/// stays byte-identical. The timer wheel is 4 levels of 256 slots of 1 ms
/// ticks (a 256 ms first level), so the quanta are chosen to land timers in
/// the same tick, in the next, across the 256 ms level boundary (a cascade),
/// and in the upper levels and the overflow list:
///
/// | quantum | share | what a few sleeps reach |
/// |---|---|---|
/// | 50 ns | 60% | one tick: ordering within a bucket |
/// | 1 ms | 15% | same-tick and next-tick timers |
/// | 50 ms | 15% | the level 0/1 boundary and cascades |
/// | 1 s | 7% | level 1 |
/// | 100 s | 3% | levels 2 and 3, and past the wheel's horizon |
fn time_quantum(seed: u64, index: u64) -> u64 {
    let mut r = Rng::new(seed.wrapping_mul(0x9E37_79B9_7F4A_7C15) ^ index ^ 0x7131_0001);
    BASE_QUANTUM
        * match r.below(100) {
            0..=59 => 1,
            60..=74 => 20_000,
            75..=89 => 1_000_000,
            90..=96 => 20_000_000,
            _ => 2_000_000_000,
        }
}

/// Salt of the storm stream. Whether scenario `index` is a storm, and the
/// storm itself, come from streams that nothing else draws from, so every
/// scenario that is not a storm stays byte-identical.
const STORM_SALT: u64 = 0x5702_4D00_0000_0010;

/// One scenario in this many is a cancel storm.
const STORM_ONE_IN: u64 = 16;

/// The task priorities a storm's sleepers draw from when it has priorities.
const STORM_PRIORITIES: [u64; 3] = [0, 50, 200];

/// Whether scenario `index` of batch `seed` is a cancel storm.
fn is_storm(seed: u64, index: u64) -> bool {
    Rng::new(seed.wrapping_mul(0x9E37_79B9_7F4A_7C15) ^ index ^ STORM_SALT).below(STORM_ONE_IN) == 0
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
    /// (channel, sender task, subscriber tasks, a subscriber that also
    /// holds a sender clone)
    broadcast: Option<(String, String, Vec<String>, Option<String>)>,
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
    /// This consumer sealed its mpsc receiver (rx.close()) and still holds it.
    sealed: bool,
    /// This task used its end of the oneshot (it sends or receives once).
    oneshot_used: bool,
    /// This task waited at the barrier (each party arrives once).
    barrier_waited: bool,
    /// Servers this task spawned and has not stopped.
    servers: Vec<String>,
    server_spawns: u32,
    /// The supervisor this task spawned (at most one) and whether it was
    /// joined.
    supervisor: Option<String>,
    supervisor_joined: bool,
}

fn sleep_step(rng: &mut Rng) -> Value {
    json!({"op": "sleep", "ns": rng.quantum * (1 + rng.below(6))})
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
        match rng.below(29) {
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
                } else if rng.chance(15) {
                    // cx.spawn_in(scope_with_budget): met with the spawner's.
                    s["budget"] = some_budget(rng);
                }
                return s;
            }
            10 if !held.children.is_empty() => {
                // An aborted child stays joinable. A child spawned in this
                // poll may be aborted too, whatever its region does first:
                // its refusal is cached by the handle (bd-c129).
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
                // A lock_until releases at once, so it holds nothing; its
                // deadline lands before or after the scenario's waits.
                if !held.mutex && rng.chance(25) {
                    return json!({"op": "mutex_lock_until", "mutex": "m",
                                  "deadline_ns": rng.quantum * (1 + rng.below(20))});
                }
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
                // A mask hides a cancel from the checkpoint, not from
                // is_cancel_requested; the checkpoint may raise a budget
                // cancel the second look sees.
                let mut steps = vec![
                    json!({"op": "is_cancel_requested"}),
                    json!({"op": "checkpoint", "on_cancel": "continue"}),
                    json!({"op": "is_cancel_requested"}),
                ];
                // Sometimes masks nest: releasing an inner one leaves the
                // outer in force, so the checkpoint after it still passes.
                if rng.chance(25) {
                    let mut inner = json!({"op": "masked", "steps": [{"op": "checkpoint", "on_cancel": "continue"}]});
                    for _ in 0..rng.below(2) {
                        inner = json!({"op": "masked", "steps": [
                            inner, {"op": "checkpoint", "on_cancel": "continue"},
                        ]});
                    }
                    steps.push(inner);
                    steps.push(json!({"op": "checkpoint", "on_cancel": "continue"}));
                }
                steps.push(json!({"op": "trace", "message": format!("{me}:m{}", held.messages)}));
                return json!({"op": "masked", "steps": steps});
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
                        // Drop-oldest: never parks, evicts when full.
                        if rng.chance(15) {
                            return json!({"op": "send_evict_oldest", "channel": ch, "value": held.sends});
                        }
                        // Waits for the receiver's drop, at the latest the
                        // consumer's body end (a distinct task).
                        if rng.chance(5) {
                            return json!({"op": "closed", "channel": ch});
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
                        // rx.close(): sends fail from now on, the queue stays
                        // receivable (a later close_receiver discards it).
                        if !held.sealed && rng.chance(8) {
                            held.sealed = true;
                            return json!({"op": "seal", "channel": ch});
                        }
                        // A batch of up to 3 (0 returns at once).
                        if rng.chance(20) {
                            return json!({"op": "recv_many", "channel": ch, "limit": rng.below(4)});
                        }
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
                    s["budget"] = some_budget(rng);
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
                            s["deadline_ns"] = json!(rng.quantum * (1 + rng.below(6)));
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
                    // Before using its end, at times a query or a closed
                    // wait on it (a receiver's wait never ends once a value
                    // is sent: a value is no close).
                    let end = if me == sender {
                        "sender"
                    } else if me == receiver {
                        "receiver"
                    } else {
                        ""
                    };
                    if !end.is_empty() {
                        match rng.below(10) {
                            0 => {
                                return json!({"op": "oneshot_is_closed", "channel": ch, "end": end});
                            }
                            1 if end == "receiver" => {
                                return json!({"op": "oneshot_is_ready", "channel": ch});
                            }
                            2 => {
                                return json!({"op": "oneshot_poll_closed", "channel": ch, "end": end});
                            }
                            _ => {}
                        }
                    }
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
                if let Some((ch, sender, subscribers, extra)) = &world.broadcast {
                    // The extra sender is also a subscriber: it sends half the time.
                    if me == sender || (extra.as_deref() == Some(me) && rng.chance(50)) {
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
            23 => {
                return json!({"op": "sleep_until", "at_ns": rng.quantum * (1 + rng.below(12))});
            }
            24 if world.rwlock => {
                // A held guard is sometimes downgraded first (a read guard
                // has nothing to downgrade: ASX_E_NOT_FOUND in both).
                if held.rw && rng.chance(25) {
                    return json!({"op": "rwlock_downgrade", "rwlock": "rw"});
                }
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
            27 if held.supervisor.is_none() => {
                let name = format!("{me}.sup");
                held.supervisor = Some(name.clone());
                return supervise_step(rng, &name);
            }
            28 if held.supervisor.is_some() && !held.supervisor_joined => {
                held.supervisor_joined = true;
                return json!({"op": "supervisor_join", "supervisor": held.supervisor});
            }
            _ => {}
        }
    }
}

/// A supervised child's program that creates names: a spawned child it
/// joins, a server it uses and stops, or (only where the child runs once, so
/// the number of supervisors stays within the arena) a nested supervisor it
/// joins. The names (`w`, `s`, `n`, `k`) are each generation's own: the
/// interpreters scope them under the generation's name (DSL §3.8), so a
/// restart does not repeat them.
fn rich_child_program(rng: &mut Rng, runs_once: bool) -> Vec<Value> {
    match rng.below(if runs_once { 3 } else { 2 }) {
        0 => vec![
            json!({"op": "spawn", "as": "w", "program": child_program(rng)}),
            json!({"op": "yield"}),
            json!({"op": "join", "task": "w"}),
        ],
        1 => vec![
            json!({"op": "server_spawn", "as": "s", "behavior": "counter", "mailbox": 2}),
            json!({"op": "cast", "server": "s", "value": 1}),
            json!({"op": "call", "server": "s", "request": 0}),
            json!({"op": "server_stop", "server": "s"}),
        ],
        _ => vec![
            json!({"op": "supervise", "as": "n", "policy": "one_for_one", "max_restarts": 1,
                   "window_ns": 60_000_000_000u64,
                   "children": [{"name": "k", "mode": "transient", "program": child_program(rng)}]}),
            json!({"op": "supervisor_join", "supervisor": "n"}),
        ],
    }
}

/// A supervisor of one to three children running small programs, one in ten
/// ending in a panic, and one in four creating names of its own (see
/// [`rich_child_program`]). Its restarts are bounded: every refused restart
/// stops its child (or escalates), so it ends once its children do.
///
/// Which children are rich is drawn from a stream derived from the main
/// one's position without consuming it, so the main stream, and every
/// scenario without a rich child, is what it was. So is which children end by
/// returning `cancelled` (bd-9kll.8.7: the factory future's
/// `Outcome::Cancelled`, which a transient child is not replaced for, a
/// permanent one is): one in seven, from a stream of its own, as the last
/// step of the child's own program, the only place the DSL defines it.
fn supervise_step(rng: &mut Rng, name: &str) -> Value {
    let policy = *rng.pick(&["one_for_one", "one_for_all", "rest_for_one"]);
    let mut rich = Rng::new(rng.state ^ 0x5C09_ED07_A11E_0001);
    rich.quantum = rng.quantum;
    let mut cancels = Rng::new(rng.state ^ 0xCA9C_E11E_D0C0_0001);
    let mut children = Vec::new();
    for i in 0..1 + rng.below(3) {
        let mut program = child_program(rng);
        if rng.chance(10) {
            program.push(
                json!({"op": "return", "outcome": {"tag": "panicked", "message": "child panic"}}),
            );
        }
        let mode = *rng.pick(&["permanent", "transient", "temporary"]);
        if rich.chance(25) {
            program = rich_child_program(&mut rich, mode == "temporary");
        }
        if cancels.chance(14) {
            // Replace the program's own ending, if it has one: a step after
            // a return never runs, so the returns it already ends with go.
            while program
                .last()
                .is_some_and(|s| s.get("op") == Some(&json!("return")))
            {
                program.pop();
            }
            program.push(json!({"op": "return", "outcome": {"tag": "cancelled"}}));
        }
        children.push(json!({
            "name": format!("c{}", i + 1),
            "mode": mode,
            "program": program,
        }));
    }
    let mut step = json!({
        "op": "supervise", "as": name, "policy": policy, "max_restarts": rng.below(4),
        "window_ns": *rng.pick(&[1_000_000_000u64, 60_000_000_000]), "children": children,
    });
    if rng.chance(30) {
        step["escalation"] = json!("escalate");
    }
    step
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
    let is_write = acquire["op"] == "rwlock_write";
    let mut steps = vec![acquire];
    // A writer sometimes downgrades at once: the readers queued ahead of
    // the first queued writer then run beside it.
    if is_write && rng.chance(30) {
        steps.push(json!({"op": "rwlock_downgrade", "rwlock": "rw"}));
    }
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
    if rng.chance(20) {
        some_budget(rng)
    } else {
        Value::Null
    }
}

/// A poll quota, a deadline, or both, sometimes with a priority (which the
/// lab schedules an admitted spawn at) or a cost quota. A deadline of 0 is
/// already due (Rust's `Some(Time::ZERO)`); the others land among the
/// scenario's waits.
fn some_budget(rng: &mut Rng) -> Value {
    let kind = rng.below(3);
    let mut b = json!({});
    if kind != 1 {
        b["poll_quota"] = json!(2 + rng.below(6));
    }
    if kind != 0 {
        let deadline = if rng.chance(20) {
            0
        } else {
            rng.quantum * (1 + rng.below(20))
        };
        b["deadline_ns"] = json!(deadline);
    }
    if rng.chance(30) {
        b["priority"] = json!(rng.below(256));
    }
    // Nothing charges cost in Rust: a quota of 0 cancels at the first
    // checkpoint (CostBudget), any other only meets into children.
    if rng.chance(12) {
        b["cost_quota"] = json!(if rng.chance(60) { 0 } else { 5 });
    }
    b
}

/// A cancel storm: 20 to 40 sleepers in `r.storm` (a third of them in its
/// child `r.kid` half the time), 1 to 3 spinners in `r.main` that sleep
/// until the instant the driver cancels `r.storm`, then yield 20 to 60
/// times. A sleep ends at the cancel and the sleeper completes, so the
/// cancel lane holds the whole crowd at once while the spinners keep the
/// ready lane non-empty: after 16 consecutive cancel dispatches the lab
/// serves a spinner, then the cancel lane again. With priorities (two in
/// five storms) the lane's highest-priority group goes first. The cancel
/// kind is User or Shutdown, which a sleep acknowledges (Timeout and
/// Deadline leave it sleeping), so every sleeper is dispatched on the cancel
/// lane and completes there. A storm uses at most 43 tasks.
fn storm_scenario(seed: u64, index: u64) -> Value {
    let mut rng = Rng::new(seed ^ index.wrapping_mul(0xA24B_AED4_963E_E407) ^ STORM_SALT);
    rng.quantum = time_quantum(seed, index);
    let q = rng.quantum;
    let sleepers = 20 + rng.below(21);
    let spinners = 1 + rng.below(3);
    let at = q * (1 + rng.below(4));
    let kid = rng.chance(50);
    let prioritised = rng.chance(40);
    let mut regions = vec![
        json!({"name": "r.main", "parent": "root", "budget": null}),
        json!({"name": "r.storm", "parent": "r.main", "budget": null}),
    ];
    if kid {
        regions.push(json!({"name": "r.kid", "parent": "r.storm", "budget": null}));
    }
    let mut tasks = Vec::new();
    for i in 0..sleepers {
        let region = if kid && i % 3 == 0 {
            "r.kid"
        } else {
            "r.storm"
        };
        let mut program = vec![json!({"op": "sleep", "ns": q * 1000})];
        if rng.chance(40) {
            program.push(json!({"op": "checkpoint"}));
        }
        let budget = if prioritised {
            json!({"priority": *rng.pick(&STORM_PRIORITIES)})
        } else {
            Value::Null
        };
        tasks.push(json!({
            "name": format!("t.s{i}"),
            "region": region,
            "budget": budget,
            "program": program,
        }));
    }
    for i in 0..spinners {
        let mut program = vec![json!({"op": "sleep_until", "at_ns": at})];
        for _ in 0..20 + rng.below(41) {
            program.push(json!({"op": "yield"}));
        }
        tasks.push(json!({
            "name": format!("t.r{i}"),
            "region": "r.main",
            "budget": null,
            "program": program,
        }));
    }
    let kind = if rng.chance(50) { "User" } else { "Shutdown" };
    json!({
        "schema": "asx.scenario.v2",
        "id": format!("gen-{seed}-{index}-001"),
        "description": "generated cancel storm (twin_run generate, bd-9kll.4.10)",
        "unit": "scheduler",
        "rules": ["inv.determinism.replayable"],
        "seed": rng.next_u64() % 1_000_000,
        "lab": {"max_steps": 2000},
        "regions": regions,
        "channels": [],
        "sync": [],
        "tasks": tasks,
        "script": [{"at_ns": at, "op": {"op": "cancel_region", "region": "r.storm", "kind": kind}}],
        "expect": {"kind": "ok"},
    })
}

/// Salt of the timer-heavy shape's stream. Only a batch generated with
/// `--shape timers` draws from it, so the default stream is untouched.
const TIMERS_SALT: u64 = 0x7131_7100_0000_0007;

const MS: u64 = 1_000_000;

/// The millisecond tick of a timer group, by wheel level: 45% under the
/// first level's 256 ms, 20% in level 1 (under 65.5 s), 15% in level 2
/// (under 4.66 h), 20% beyond, where level 3 ends and the overflow heap
/// holds what is more than a day ahead (the wheel's configured maximum).
fn timers_tick_ms(rng: &mut Rng) -> u64 {
    match rng.below(100) {
        0..=44 => 1 + rng.below(255),
        45..=64 => 256 + rng.below(65_536 - 256),
        65..=79 => 65_536 + rng.below(16_777_216 - 65_536),
        _ => 16_777_216 + rng.below(1_000_000_000 - 16_777_216),
    }
}

/// One task of a timer-heavy scenario, as drawn.
struct TimerTask {
    deadline: u64,
    /// 0 sleep_until, 1 prelude sleep then sleep_until, 2 relative sleep,
    /// 3 timeout around a semaphore acquire, 4 mutex_lock_until.
    kind: u64,
    prelude: u64,
    /// A short sleep after the wake, 20% of the tasks.
    second: Option<u64>,
    /// Index in the task's region list (r.main, r.a, r.b).
    region: u64,
    priority: Option<u64>,
}

/// Draw `members` timers whose deadlines fall in one tick of a drawn level.
fn timers_group(rng: &mut Rng, members: u64, out: &mut Vec<TimerTask>) {
    let tick = timers_tick_ms(rng);
    for _ in 0..members {
        let deadline = tick * MS + rng.below(MS);
        let kind = match rng.below(100) {
            0..=39 => 0,
            40..=54 => 1,
            55..=69 => 2,
            70..=84 => 3,
            _ => 4,
        };
        let prelude = if kind == 1 {
            deadline / 4 * (1 + rng.below(3))
        } else {
            0
        };
        let second = rng.chance(20).then(|| 1 + rng.below(2 * MS));
        let region = match rng.below(100) {
            0..=59 => 0,
            60..=79 => 1,
            _ => 2,
        };
        let priority = rng.chance(15).then(|| *rng.pick(&STORM_PRIORITIES));
        out.push(TimerTask {
            deadline,
            kind,
            prelude,
            second,
            region,
            priority,
        });
    }
}

/// A timer-heavy scenario (bd-9kll.7.1, the 1000-random-timers differential
/// of the same-tick ordering port): 1 to 4 groups of 2 to 8 timers whose
/// deadlines fall in one 1 ms tick of the wheel at a drawn level, in an
/// order that is not their deadline order, plus 0 to 6 timers of their own.
/// A timer is a sleep, a sleep registered late (a prelude sleep first, so
/// the same deadline is filed at another level and reached by a cascade), a
/// `timeout` around a semaphore nobody releases before the deadline, or a
/// `mutex_lock_until` on a mutex a holder keeps. Tasks sit in `r.main`,
/// `r.a` or `r.b`; the script cancels a region or a task (User and Shutdown
/// end the sleeps, Timeout and Deadline leave them asleep and move their
/// timers to the new waker) and advances the clock past several deadlines at
/// once, so they come due in one collect and the wheel's order, not the
/// deadlines', is what both runtimes must agree on. Cancels and advances
/// land on a tick, sometimes on a member's exact deadline.
pub fn timer_scenario(seed: u64, index: u64) -> Value {
    let mut rng = Rng::new(seed ^ index.wrapping_mul(0xA24B_AED4_963E_E407) ^ TIMERS_SALT);
    let mut timers: Vec<TimerTask> = Vec::new();
    for _ in 0..1 + rng.below(4) {
        let members = 2 + rng.below(7);
        timers_group(&mut rng, members, &mut timers);
    }
    for _ in 0..rng.below(7) {
        timers_group(&mut rng, 1, &mut timers);
    }
    let deadlines: Vec<u64> = timers.iter().map(|t| t.deadline).collect();
    // Fisher-Yates: registration order is not group order.
    for i in (1..timers.len()).rev() {
        let j = usize::try_from(rng.below(i as u64 + 1)).unwrap_or(0);
        timers.swap(i, j);
    }
    let regions = ["r.main", "r.a", "r.b"];
    let mut tasks = Vec::new();
    let (mut uses_sem, mut uses_mutex) = (false, false);
    for (i, timer) in timers.iter().enumerate() {
        let TimerTask {
            deadline,
            kind,
            prelude,
            second,
            region,
            priority,
        } = timer;
        let name = format!("t.{i}");
        let mut program = Vec::new();
        match *kind {
            0 => program.push(json!({"op": "sleep_until", "at_ns": deadline})),
            1 => {
                program.push(json!({"op": "sleep", "ns": prelude}));
                program.push(json!({"op": "sleep_until", "at_ns": deadline}));
            }
            2 => program.push(json!({"op": "sleep", "ns": deadline})),
            3 => {
                uses_sem = true;
                program.push(json!({"op": "timeout", "ns": deadline,
                    "inner": {"op": "sem_acquire", "semaphore": "s", "count": 1}}));
            }
            _ => {
                uses_mutex = true;
                program
                    .push(json!({"op": "mutex_lock_until", "mutex": "m", "deadline_ns": deadline}));
            }
        }
        program.push(json!({"op": "trace", "message": name}));
        if let Some(ns) = second {
            program.push(json!({"op": "sleep", "ns": ns}));
            program.push(json!({"op": "trace", "message": format!("{name}/again")}));
        }
        let budget = match priority {
            Some(p) => json!({"priority": p}),
            None => Value::Null,
        };
        tasks.push(json!({
            "name": name,
            "region": regions[usize::try_from(*region).unwrap_or(0)],
            "budget": budget,
            "program": program,
        }));
    }
    // What the waits wait for: a semaphore with no permits that a releaser
    // tops up at an instant of the drawn deadlines (waiters whose deadline
    // is later get it, the others have timed out), and a mutex a holder
    // keeps until then.
    let mut sync = Vec::new();
    if uses_sem {
        sync.push(json!({"name": "s", "type": "semaphore", "permits": 0}));
        let at = *rng.pick(&deadlines) + rng.below(3) * (MS / 3);
        tasks.push(json!({
            "name": "t.rel", "region": "r.main", "budget": null,
            "program": [
                {"op": "sleep_until", "at_ns": at},
                {"op": "sem_add_permits", "semaphore": "s", "count": 1 + rng.below(2)},
            ],
        }));
    }
    if uses_mutex {
        sync.push(json!({"name": "m", "type": "mutex"}));
        let at = *rng.pick(&deadlines) + rng.below(3) * (MS / 3);
        tasks.push(json!({
            "name": "t.hold", "region": "r.main", "budget": null,
            "program": [
                {"op": "mutex_lock", "mutex": "m"},
                {"op": "sleep_until", "at_ns": at},
                {"op": "mutex_unlock", "mutex": "m"},
            ],
        }));
    }
    let mut script = Vec::new();
    for _ in 0..rng.below(4) {
        let base = *rng.pick(&deadlines);
        let at = if rng.chance(40) {
            base
        } else {
            base - base % MS + rng.below(MS)
        };
        let op = match rng.below(4) {
            0 => {
                json!({"op": "cancel_region", "region": "r.a", "kind": *rng.pick(&CANCEL_KINDS)})
            }
            1 => {
                json!({"op": "cancel_region", "region": "r.b", "kind": *rng.pick(&CANCEL_KINDS)})
            }
            2 => {
                let victim = format!("t.{}", rng.below(timers.len() as u64));
                json!({"op": "cancel_task", "task": victim, "kind": *rng.pick(&CANCEL_KINDS)})
            }
            _ => {
                let ns = match rng.below(3) {
                    0 => 1,
                    1 => 1 + rng.below(MS),
                    _ => MS * (1 + rng.below(300)),
                };
                json!({"op": "advance", "ns": ns})
            }
        };
        script.push(json!({"at_ns": at, "op": op}));
    }
    script.sort_by_key(|entry| entry["at_ns"].as_u64());
    json!({
        "schema": "asx.scenario.v2",
        "id": format!("gen-tm{seed}-{index}-001"),
        "description": "generated timer-heavy scenario (twin_run generate --shape timers, bd-9kll.7.1)",
        "unit": "timers",
        "rules": ["inv.determinism.replayable"],
        "seed": rng.next_u64() % 1_000_000,
        "lab": {"max_steps": 4000},
        "regions": [
            {"name": "r.main", "parent": "root", "budget": null},
            {"name": "r.a", "parent": "r.main", "budget": null},
            {"name": "r.b", "parent": "r.main", "budget": null},
        ],
        "channels": [],
        "sync": sync,
        "tasks": tasks,
        "script": script,
        "expect": {"kind": "ok"},
    })
}

/// Salt of the metamorphic shape's stream (`--shape meta`, bd-9kll.12.1).
const META_SALT: u64 = 0x4D45_5441_0000_0012;

/// One scenario in this many of the metamorphic shape is an M3 case (a
/// nested-region budget triple), the rest are for M1, M2 and M4. Half: a
/// relation scenario yields about 0.85 M4 cases and 2 M1 and M2 cases, an M3
/// scenario one, so 10000 cases of each take about 12000 scenarios of each
/// kind.
const META_M3_ONE_IN: u64 = 2;

/// A task program that names nothing but its own: obligations, children and
/// regions are named after the task, so the task shares nothing with any
/// other (the isolation M1 permutes). It ends with every obligation it
/// reserved resolved and every child joined.
fn meta_private_program(rng: &mut Rng, me: &str, opens_region: bool) -> Vec<Value> {
    let mut steps = Vec::new();
    let mut open: Vec<String> = Vec::new();
    let mut children: Vec<String> = Vec::new();
    let (mut reserves, mut messages) = (0_u32, 0_u32);
    let mut region_open = false;
    for _ in 0..2 + rng.below(7) {
        match rng.below(13) {
            0 | 1 => steps.push(json!({"op": "yield"})),
            2 | 3 => steps.push(sleep_step(rng)),
            4 => steps.push(checkpoint_step(rng)),
            5 => {
                messages += 1;
                steps.push(json!({"op": "trace", "message": format!("{me}:m{messages}")}));
            }
            6 | 7 if open.len() < 2 => {
                reserves += 1;
                let name = format!("{me}.o{reserves}");
                open.push(name.clone());
                steps.push(
                    json!({"op": "reserve", "kind": *rng.pick(&OBLIGATION_KINDS), "as": name}),
                );
            }
            8 if !open.is_empty() => {
                let name = open.remove(0);
                steps.push(match rng.below(3) {
                    0 => json!({"op": "commit", "obligation": name}),
                    1 => json!({"op": "abort", "obligation": name,
                                "reason": *rng.pick(&ABORT_REASONS)}),
                    _ => json!({"op": "leak", "obligation": name}),
                });
            }
            9 if children.len() < 2 => {
                let name = format!("{me}.c{}", children.len() + 1);
                children.push(name.clone());
                steps.push(json!({"op": "spawn", "as": name, "program": child_program(rng)}));
            }
            10 => steps.push(json!({"op": "masked", "steps": [
                {"op": "is_cancel_requested"},
                {"op": "checkpoint", "on_cancel": "continue"},
                {"op": "trace", "message": format!("{me}:masked")},
            ]})),
            11 if opens_region && !region_open => {
                region_open = true;
                steps.push(json!({"op": "open_region", "as": format!("{me}.r1")}));
            }
            _ => steps.push(json!({"op": "is_cancel_requested"})),
        }
    }
    for name in open {
        steps.push(json!({"op": "commit", "obligation": name}));
    }
    for name in children {
        steps.push(json!({"op": "join", "task": name}));
    }
    if region_open {
        steps.push(json!({"op": "close_region", "region": format!("{me}.r1")}));
    }
    if rng.chance(15) {
        steps.push(
            json!({"op": "return", "outcome": {"tag": "err", "status": *rng.pick(&ERR_STATUSES)}}),
        );
    }
    steps
}

/// A budget of the outer regions of an M3 case (deadline and priority only:
/// a quota out there clamps the spawned task's own, which the two forms
/// reach in a different number of polls) or, with `quotas`, of the
/// innermost one.
fn meta_budget(rng: &mut Rng, quotas: bool) -> Value {
    if rng.chance(15) {
        return Value::Null;
    }
    let mut b = json!({"priority": rng.below(256)});
    if rng.chance(50) {
        b["deadline_ns"] = json!(50 * (1 + rng.below(40)));
    }
    if quotas && rng.chance(40) {
        b["poll_quota"] = json!(1 + rng.below(12));
    }
    if quotas && rng.chance(15) {
        b["cost_quota"] = json!(1 + rng.below(12));
    }
    b
}

/// The program of M3's `t.g`: the same text as test_metamorphic.c's
/// MM_M3_PROGRAM.
fn meta_m3_program() -> Value {
    let check = json!({"op": "checkpoint", "on_cancel": "return"});
    let y = json!({"op": "yield"});
    json!([
        check, y, check, y, check, y, check, y, check,
        {"op": "sleep", "ns": 600},
        check,
        {"op": "trace", "message": "x end"},
    ])
}

/// An M3 case: the nested form (a region with budget `a`, a region with
/// budget `b` inside it, a task in the inner one) under a region with budget
/// `top`. test_metamorphic.c reads the three budgets back and builds the
/// single-region form with `meet(a, b)` itself.
fn meta_m3_scenario(rng: &mut Rng, seed: u64, index: u64) -> Value {
    let top = meta_budget(rng, false);
    let a = meta_budget(rng, false);
    let b = meta_budget(rng, true);
    json!({
        "schema": "asx.scenario.v2",
        "id": format!("mm-m3-{seed}-{index}-001"),
        "description": "generated nested-region budget case (twin_run generate --shape meta, bd-9kll.12.1)",
        "unit": "budget",
        "rules": ["inv.determinism.replayable"],
        "seed": 42,
        "lab": {"max_steps": 4000},
        "regions": [{"name": "r.main", "parent": "root", "budget": null}],
        "channels": [],
        "sync": [],
        "tasks": [
            {"name": "t.0", "region": "r.main", "budget": null, "program": [
                {"op": "open_region", "as": "r.1", "budget": top},
                {"op": "spawn", "as": "t.c", "region": "r.1", "program": [
                    {"op": "open_region", "as": "r.2", "budget": a},
                    {"op": "spawn", "as": "t.d", "region": "r.2", "program": [
                        {"op": "open_region", "as": "r.3", "budget": b},
                        {"op": "spawn", "as": "t.g", "region": "r.3", "program": meta_m3_program()},
                        {"op": "join", "task": "t.g"},
                    ]},
                    {"op": "join", "task": "t.d"},
                ]},
                {"op": "join", "task": "t.c"},
            ]},
            {"name": "t.peer", "region": "r.main", "budget": {"priority": 100},
             "program": [{"op": "yield"}, {"op": "yield"}, {"op": "yield"}]},
        ],
        "script": [],
        "expect": {"kind": "ok"},
    })
}

/// A scenario for the metamorphic relations M1, M2 and M4 (bd-9kll.12.1):
/// 3 to 6 tasks in `r.main` whose programs name nothing but their own
/// obligations, children and regions (M1 permutes them, M2 adds a no-op
/// beside them), 0 or 2 tasks that share an mpsc channel (dependent: they
/// stay where they are), 1 to 3 tasks in each of `r.a` and `r.b`, and a
/// driver script of 1 to 3 cancels and advances of kinds other than User
/// (M4 repeats each with a weaker User request). No budget has a limit and
/// no region has limits, so the three relations apply.
fn meta_relation_scenario(rng: &mut Rng, seed: u64, index: u64) -> Value {
    let mut tasks = Vec::new();
    let private = 3 + rng.below(4);
    let mut opened = false;
    for i in 0..private {
        let me = format!("t.i{i}");
        let opens = !opened && rng.chance(25);
        opened |= opens;
        tasks.push(json!({
            "name": me,
            "region": "r.main",
            "budget": if rng.chance(15) { json!({"priority": *rng.pick(&STORM_PRIORITIES)}) } else { Value::Null },
            "program": meta_private_program(rng, &me, opens),
        }));
    }
    let mut channels = Vec::new();
    if rng.chance(50) {
        let values = 1 + rng.below(3);
        channels.push(
            json!({"name": "ch", "type": "mpsc", "capacity": 1 + rng.below(2),
                             "sender": "t.p", "receiver": "t.q"}),
        );
        let mut send = vec![json!({"op": "yield"})];
        let mut recv = Vec::new();
        for v in 0..values {
            send.push(json!({"op": "send", "channel": "ch", "value": v}));
            recv.push(json!({"op": "recv", "channel": "ch"}));
        }
        send.push(json!({"op": "close_sender", "channel": "ch"}));
        tasks.push(json!({"name": "t.p", "region": "r.main", "budget": null, "program": send}));
        tasks.push(json!({"name": "t.q", "region": "r.main", "budget": null, "program": recv}));
    }
    let mut names: Vec<String> = Vec::new();
    for region in ["r.a", "r.b"] {
        for i in 0..rng.below(4) {
            let me = format!("t.{}{i}", &region[2..]);
            let mut program = vec![
                sleep_step(rng),
                checkpoint_step(rng),
                json!({"op": "trace", "message": format!("{me}:woke")}),
            ];
            if rng.chance(50) {
                program.push(sleep_step(rng));
                program.push(checkpoint_step(rng));
            }
            names.push(me.clone());
            tasks.push(json!({"name": me, "region": region, "budget": null, "program": program}));
        }
    }
    // A task parked on a notify nobody signals keeps its region (r.a) from
    // closing after the cancel below, so the region's cancel reason, which
    // a weaker second request must leave alone (M4), is still in the
    // snapshot: a closed region carries none.
    let mut sync = Vec::new();
    let mut script = Vec::new();
    let mut at = 0;
    if rng.chance(35) {
        sync.push(json!({"name": "n", "type": "notify"}));
        tasks.push(
            json!({"name": "t.stuck", "region": "r.a", "budget": null, "program": [
                sleep_step(rng),
                {"op": "notify_wait", "notify": "n"},
            ]}),
        );
        at += rng.quantum * rng.below(4);
        script.push(
            json!({"at_ns": at, "op": {"op": "cancel_region", "region": "r.a",
            "kind": *rng.pick(&["Shutdown", "Timeout", "Deadline"])}}),
        );
    }
    for _ in 0..1 + rng.below(3) {
        at += rng.quantum * rng.below(6);
        let kind = *rng.pick(&["Shutdown", "Timeout", "Deadline", "User"]);
        let op = match rng.below(4) {
            0 => json!({"op": "cancel_region", "region": "r.a", "kind": kind}),
            1 => json!({"op": "cancel_region", "region": "r.b", "kind": kind}),
            2 if !names.is_empty() => {
                json!({"op": "cancel_task", "task": *rng.pick(&names), "kind": kind})
            }
            _ => json!({"op": "advance", "ns": rng.quantum * (1 + rng.below(4))}),
        };
        script.push(json!({"at_ns": at, "op": op}));
    }
    json!({
        "schema": "asx.scenario.v2",
        "id": format!("mm-rel-{seed}-{index}-001"),
        "description": "generated metamorphic-relation scenario (twin_run generate --shape meta, bd-9kll.12.1)",
        "unit": "scheduler",
        "rules": ["inv.determinism.replayable"],
        "seed": rng.next_u64() % 1_000_000,
        "lab": {"max_steps": 3000},
        "regions": [
            {"name": "r.main", "parent": "root", "budget": null},
            {"name": "r.a", "parent": "r.main", "budget": null},
            {"name": "r.b", "parent": "r.main", "budget": null},
        ],
        "channels": channels,
        "sync": sync,
        "tasks": tasks,
        "script": script,
        "expect": {"kind": "ok"},
    })
}

/// Scenario `index` of the metamorphic batch seeded `seed` (`--shape
/// meta`): one in [`META_M3_ONE_IN`] is an M3 case, the rest are
/// scenarios for M1, M2 and M4. test_metamorphic.c's `--fuzz` mode reads
/// them from stdin (`generate --jsonl`).
pub fn meta_scenario(seed: u64, index: u64) -> Value {
    let mut rng = Rng::new(seed ^ index.wrapping_mul(0xA24B_AED4_963E_E407) ^ META_SALT);
    if rng.below(META_M3_ONE_IN) == 0 {
        meta_m3_scenario(&mut rng, seed, index)
    } else {
        meta_relation_scenario(&mut rng, seed, index)
    }
}

/// Scenario `index` of the batch seeded `seed`.
pub fn scenario(seed: u64, index: u64) -> Value {
    if is_storm(seed, index) {
        return storm_scenario(seed, index);
    }
    let mut rng = Rng::new(seed ^ index.wrapping_mul(0xA24B_AED4_963E_E407));
    rng.quantum = time_quantum(seed, index);
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
        // Its budget does not reach the tasks created in it, only the
        // regions they open (create_task takes the task's own budget).
        let side_budget = if rng.chance(25) {
            some_budget(&mut rng)
        } else {
            Value::Null
        };
        regions.push(json!({"name": "r.side", "parent": parent, "budget": side_budget}));
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
            // Sometimes the last task also holds a sender clone: the
            // channel then closes at the later of the two senders' drops.
            broadcast: (several && rng.chance(15)).then(|| {
                let extra = rng.chance(30).then(|| last.clone());
                (
                    "bc".to_string(),
                    tasks[0].clone(),
                    tasks[1..].to_vec(),
                    extra,
                )
            }),
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
    if let Some((ch, tx, subs, extra)) = &world.broadcast {
        let mut decl = json!({"name": ch, "type": "broadcast", "capacity": 1 + rng.below(3),
                              "sender": tx, "subscribers": subs});
        if let Some(extra) = extra {
            decl["senders"] = json!([extra]);
        }
        channels.push(decl);
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
        // Keep the script's run-to-idle boundary when no limit was drawn,
        // but express its no-op with a schema-valid zero-time advance.
        // Do not redraw: later operations must keep the same RNG stream.
        if op.as_object().is_some_and(|fields| fields.len() == 2) {
            op = json!({"op": "advance", "ns": 0});
        }
        script.push(json!({"at_ns": 0, "op": op}));
    }
    for _ in 0..rng.below(3) {
        at += rng.quantum * rng.below(5);
        let op = match rng.below(3) {
            0 => {
                json!({"op": "cancel_task", "task": *rng.pick(&tasks), "kind": *rng.pick(&CANCEL_KINDS)})
            }
            1 => {
                json!({"op": "cancel_region", "region": *rng.pick(&region_names), "kind": *rng.pick(&CANCEL_KINDS)})
            }
            _ => json!({"op": "advance", "ns": rng.quantum * (1 + rng.below(4))}),
        };
        script.push(json!({"at_ns": at, "op": op}));
    }

    json!({
        "schema": "asx.scenario.v2",
        "id": format!("gen-{seed}-{index}-001"),
        "description": "generated (twin_run generate, bd-ij9w)",
        "unit": "scheduler",
        "rules": ["inv.determinism.replayable"],
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

#[cfg(test)]
mod tests {
    use super::*;

    /// (`return cancelled` steps anywhere in `v`, those that are the last step
    /// of a supervised child's own program).
    fn cancelled_returns(v: &Value) -> (u32, u32) {
        let is_cancelled_return = |s: &Value| {
            s.get("op") == Some(&json!("return"))
                && s.get("outcome").and_then(|o| o.get("tag")) == Some(&json!("cancelled"))
        };
        let mut any = 0;
        let mut own = 0;
        match v {
            Value::Object(fields) => {
                if is_cancelled_return(v) {
                    any += 1;
                }
                if fields.get("op") == Some(&json!("supervise")) {
                    for child in fields
                        .get("children")
                        .and_then(Value::as_array)
                        .into_iter()
                        .flatten()
                    {
                        let program = child.get("program").and_then(Value::as_array);
                        if let Some(last) = program.and_then(|p| p.last())
                            && is_cancelled_return(last)
                        {
                            own += 1;
                        }
                    }
                }
                for field in fields.values() {
                    let (a, o) = cancelled_returns(field);
                    any += a;
                    own += o;
                }
            }
            Value::Array(items) => {
                for item in items {
                    let (a, o) = cancelled_returns(item);
                    any += a;
                    own += o;
                }
            }
            _ => {}
        }
        (any, own)
    }

    /// bd-9kll.8.7: the generator emits `return cancelled`, only where the
    /// DSL defines it (the last step of a supervised child's own program),
    /// and does so deterministically.
    #[test]
    fn supervised_children_sometimes_return_cancelled_and_nowhere_else() {
        let mut own_total = 0;
        for index in 0..400 {
            let scenario_value = scenario(1, index);
            assert_eq!(
                scenario_value,
                scenario(1, index),
                "scenario {index} is not a pure function of its seed"
            );
            let (any, own) = cancelled_returns(&scenario_value);
            assert_eq!(
                any, own,
                "scenario {index} has a `return cancelled` outside a supervised child's own program"
            );
            own_total += own;
        }
        assert!(
            own_total > 0,
            "400 scenarios of seed 1 emitted no supervised child that returns cancelled"
        );
    }

    /// The cancelled returns come from a stream of their own: a scenario
    /// without a supervisor is what it was, step for step.
    #[test]
    fn a_scenario_without_a_supervisor_has_no_cancelled_return() {
        for index in 0..100 {
            let text = scenario(7, index).to_string();
            if !text.contains("\"supervise\"") {
                assert!(!text.contains("\"cancelled\""), "scenario {index}");
            }
        }
    }

    /// The indices below `count` of batch `seed` that are cancel storms.
    fn storm_indices(seed: u64, count: u64) -> Vec<u64> {
        (0..count).filter(|&index| is_storm(seed, index)).collect()
    }

    /// bd-9kll.4.10: the fuzz-differential lanes' own batches contain
    /// storms, at these indices. CI runs `make fuzz-differential
    /// FUZZ_V2_SEED=9 FUZZ_V2_COUNT=200`: 13 storms in 200. The Makefile's
    /// default is seed 1, count 100: 8. Whether scenario `index` is a storm
    /// is a pure function of `seed` and `index` (the storm stream's own
    /// salt), so these lists change only when STORM_SALT or STORM_ONE_IN do.
    #[test]
    fn the_lane_batches_emit_cancel_storms_at_known_indices() {
        assert_eq!(
            storm_indices(9, 200),
            vec![9, 24, 27, 30, 46, 52, 96, 178, 179, 183, 187, 191, 194]
        );
        assert_eq!(storm_indices(1, 100), vec![27, 45, 49, 55, 62, 79, 93, 96]);
    }

    /// One scenario in sixteen on average: 100 expected in 1600, and a fair
    /// draw lands within 60..=140 (seeds 1, 2 and 3 give 109, 77 and 100).
    #[test]
    fn about_one_scenario_in_sixteen_is_a_cancel_storm() {
        for seed in [1_u64, 2, 3] {
            let storms = storm_indices(seed, 1600).len();
            assert!(
                (60..=140).contains(&storms),
                "seed {seed}: {storms} storms in 1600"
            );
        }
    }

    /// The shape that reaches the lab's cancel streak limit (16): more
    /// sleepers than the limit, all still asleep at the instant the driver
    /// cancels their region, and spinners that wake at that instant and
    /// yield 20 to 60 times, so the ready lane is never empty while the
    /// cancel lane drains.
    #[test]
    fn every_storm_of_a_lane_batch_has_more_sleepers_than_the_streak_limit() {
        const STREAK_LIMIT: usize = 16;
        for index in storm_indices(9, 200) {
            let storm = scenario(9, index);
            let tasks = storm["tasks"].as_array().expect("a storm has tasks");
            let named = |prefix: &str| -> Vec<&Value> {
                tasks
                    .iter()
                    .filter(|t| t["name"].as_str().is_some_and(|n| n.starts_with(prefix)))
                    .collect()
            };
            let sleepers = named("t.s");
            let spinners = named("t.r");
            assert!(
                sleepers.len() > STREAK_LIMIT && sleepers.len() <= 40,
                "storm {index}: {} sleepers",
                sleepers.len()
            );
            assert!((1..=3).contains(&spinners.len()), "storm {index}");
            assert!(tasks.len() <= 43, "storm {index}: {} tasks", tasks.len());

            let cancel = &storm["script"][0];
            let at = cancel["at_ns"].as_u64().expect("the cancel instant");
            assert_eq!(cancel["op"]["op"], "cancel_region", "storm {index}");
            assert_eq!(cancel["op"]["region"], "r.storm", "storm {index}");
            assert!(
                matches!(cancel["op"]["kind"].as_str(), Some("User" | "Shutdown")),
                "storm {index}: a sleep acknowledges only User and Shutdown"
            );
            for sleeper in sleepers {
                let sleep = &sleeper["program"][0];
                assert_eq!(sleep["op"], "sleep", "storm {index}");
                assert!(
                    sleep["ns"].as_u64().expect("sleep ns") > at,
                    "storm {index}: a sleeper wakes before the cancel"
                );
            }
            for spinner in spinners {
                let program = spinner["program"].as_array().expect("a program");
                assert_eq!(program[0]["op"], "sleep_until", "storm {index}");
                assert_eq!(program[0]["at_ns"].as_u64(), Some(at), "storm {index}");
                let yields = &program[1..];
                assert!((20..=60).contains(&yields.len()), "storm {index}");
                assert!(yields.iter().all(|s| s["op"] == "yield"), "storm {index}");
            }
        }
    }

    /// A storm is a pure function of its seed and index, and it is the
    /// scenario `scenario` returns for that index (no other draw reaches it).
    #[test]
    fn a_storm_is_a_pure_function_of_its_seed_and_index() {
        let first = scenario(9, 9);
        assert_eq!(first, scenario(9, 9));
        assert_eq!(first["id"], "gen-9-9-001");
        assert_eq!(
            first["description"],
            "generated cancel storm (twin_run generate, bd-9kll.4.10)"
        );
    }

    /// The tasks of a timer-heavy scenario that wait on a deadline (not the
    /// releaser or the holder), each with the deadline in ns: the last wait
    /// before its first trace marker (a prelude sleep comes before it).
    fn timer_deadlines(scenario_value: &Value) -> Vec<u64> {
        scenario_value["tasks"]
            .as_array()
            .expect("tasks")
            .iter()
            .filter(|t| !matches!(t["name"].as_str(), Some("t.rel" | "t.hold")))
            .map(|t| {
                let program = t["program"].as_array().expect("a program");
                let wait = program
                    .iter()
                    .take_while(|s| s["op"] != "trace")
                    .last()
                    .expect("a wait before the trace marker");
                let op = wait["op"].as_str().unwrap_or("");
                assert!(
                    matches!(op, "sleep_until" | "sleep" | "timeout" | "mutex_lock_until"),
                    "unexpected wait {op}"
                );
                let field = match op {
                    "sleep_until" => "at_ns",
                    "mutex_lock_until" => "deadline_ns",
                    _ => "ns",
                };
                wait[field].as_u64().expect("a deadline")
            })
            .collect()
    }

    /// bd-9kll.7.1: the timer-heavy shape is a pure function of its seed and
    /// index, has an id of its own, and is what `--shape timers` writes.
    #[test]
    fn a_timer_scenario_is_a_pure_function_of_its_seed_and_index() {
        let first = timer_scenario(7100, 5);
        assert_eq!(first, timer_scenario(7100, 5));
        assert_ne!(first, timer_scenario(7100, 6));
        assert_ne!(first, scenario(7100, 5));
        assert_eq!(first["id"], "gen-tm7100-5-001");
        assert_eq!(first["unit"], "timers");
    }

    /// Every scenario of the fixed lane batch (seed 7100, 1000 scenarios)
    /// holds at least two timers in one 1 ms tick, stays within 40 tasks,
    /// and the batch reaches every level of the wheel and the overflow heap
    /// (more than a day ahead), uses every kind of wait, and has cancels of a
    /// region and of a task and advances past several deadlines.
    #[test]
    fn the_timer_lane_batch_shares_ticks_across_every_level() {
        const BATCH: u64 = 1000;
        let mut levels = [0_u32; 4];
        let (mut timeouts, mut lock_untils, mut preludes) = (0_u32, 0_u32, 0_u32);
        let (mut region_cancels, mut task_cancels, mut advances) = (0_u32, 0_u32, 0_u32);
        let mut other_ops = 0_u32;
        let mut timers = 0_usize;
        for index in 0..BATCH {
            let value = timer_scenario(7100, index);
            let tasks = value["tasks"].as_array().expect("tasks");
            assert!(tasks.len() <= 40, "scenario {index}: {} tasks", tasks.len());
            let deadlines = timer_deadlines(&value);
            timers += deadlines.len();
            let mut ticks: Vec<u64> = deadlines.iter().map(|d| d / MS).collect();
            ticks.sort_unstable();
            assert!(
                ticks.windows(2).any(|w| w[0] == w[1]),
                "scenario {index}: no two timers share a tick"
            );
            let latest = ticks.last().copied().unwrap_or(0);
            let level = match latest {
                0..=255 => 0,
                256..=65_535 => 1,
                65_536..=16_777_215 => 2,
                _ => 3,
            };
            levels[level] += 1;
            let text = value.to_string();
            timeouts += u32::from(text.contains("\"timeout\""));
            lock_untils += u32::from(text.contains("\"mutex_lock_until\""));
            preludes += u32::from(tasks.iter().any(|t| {
                t["program"][0]["op"] == "sleep" && t["program"][1]["op"] == "sleep_until"
            }));
            let script = value["script"].as_array().expect("script");
            let at: Vec<u64> = script
                .iter()
                .map(|e| e["at_ns"].as_u64().unwrap_or(0))
                .collect();
            assert!(
                at.windows(2).all(|w| w[0] <= w[1]),
                "scenario {index}: script order"
            );
            for entry in script {
                match entry["op"]["op"].as_str() {
                    Some("cancel_region") => region_cancels += 1,
                    Some("cancel_task") => task_cancels += 1,
                    Some("advance") => advances += 1,
                    _ => other_ops += 1,
                }
            }
        }
        assert_eq!(other_ops, 0, "script ops the shape does not draw");
        assert!(timers >= 10_000, "{timers} timers in {BATCH} scenarios");
        for (level, scenarios) in levels.iter().enumerate() {
            assert!(
                *scenarios >= 50,
                "level {level}: only {scenarios} scenarios end there"
            );
        }
        for (what, n) in [
            ("timeout", timeouts),
            ("mutex_lock_until", lock_untils),
            ("prelude sleep", preludes),
            ("cancel_region", region_cancels),
            ("cancel_task", task_cancels),
            ("advance", advances),
        ] {
            assert!(n >= 100, "{what}: {n} in {BATCH} scenarios");
        }
    }

    /// bd-9kll.12.1: the metamorphic shape is a pure function of its seed and
    /// index, and half of the scenarios are M3 cases.
    #[test]
    fn a_metamorphic_scenario_is_a_pure_function_and_half_are_m3_cases() {
        let mut m3 = 0;
        for index in 0..600 {
            let value = meta_scenario(12100, index);
            assert_eq!(value, meta_scenario(12100, index), "scenario {index}");
            let id = value["id"].as_str().expect("an id");
            if id.starts_with("mm-m3-") {
                m3 += 1;
            } else {
                assert!(id.starts_with("mm-rel-"), "scenario {index}: {id}");
            }
        }
        assert!((250..=350).contains(&m3), "{m3} M3 cases in 600");
    }

    /// The relation scenarios give M1 and M4 something to work on: at least
    /// two tasks that name only their own obligations, children and regions
    /// (so isolated), and a driver cancel of a kind other than User. No
    /// budget has a limit and no region has limits, so M2 and M4 apply.
    #[test]
    fn a_relation_scenario_has_isolated_tasks_and_cancels_no_budget_limits() {
        let mut with_weaker_target = 0;
        let mut with_pair = 0;
        for index in 0..400 {
            let value = meta_scenario(12100, index);
            if value["id"]
                .as_str()
                .is_some_and(|i| i.starts_with("mm-m3-"))
            {
                continue;
            }
            let text = value.to_string();
            for forbidden in ["region_limits", "poll_quota", "cost_quota", "deadline_ns"] {
                assert!(!text.contains(forbidden), "scenario {index}: {forbidden}");
            }
            let private = value["tasks"]
                .as_array()
                .expect("tasks")
                .iter()
                .filter(|t| t["name"].as_str().is_some_and(|n| n.starts_with("t.i")))
                .count();
            assert!(
                (3..=6).contains(&private),
                "scenario {index}: {private} private tasks"
            );
            let script = value["script"].as_array().expect("script");
            assert!(!script.is_empty() && script.len() <= 4, "scenario {index}");
            if script.iter().any(|e| {
                matches!(
                    e["op"]["kind"].as_str(),
                    Some("Shutdown" | "Timeout" | "Deadline")
                )
            }) {
                with_weaker_target += 1;
            }
            if value["channels"].as_array().is_some_and(|c| !c.is_empty()) {
                with_pair += 1;
            }
        }
        assert!(
            with_weaker_target >= 100,
            "{with_weaker_target} scenarios M4 applies to"
        );
        assert!(
            with_pair >= 50,
            "{with_pair} scenarios with a dependent pair"
        );
    }
}
