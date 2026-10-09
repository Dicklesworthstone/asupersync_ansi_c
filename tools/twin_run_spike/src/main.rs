//! W1.4s go/no-go spike (bd-9kll.2.16).
//!
//! Question: can a crate outside asupersync drive its lab runtime at the
//! pinned rev well enough to be the Rust side of the twin-run oracle? Each
//! capability the W1.4 design relies on is exercised for real and reported as
//! one line, `CAP <id> <ok|FAIL> <detail>`, so the feasibility doc can cite
//! this program's output rather than a reading of the source.
//!
//! Compiling at all settles visibility: every API used here is public at the
//! pinned rev without the `test-internals` feature.

use asupersync::channel::{mpsc, oneshot};
use asupersync::lab::runtime::{ForcedSchedule, ForcedScheduleDecodeLimits, ForcedScheduleLimits};
use asupersync::lab::{LabConfig, LabRuntime};
use asupersync::trace::TraceEvent;
use asupersync::trace::canonicalize::{canonicalize, trace_fingerprint};
use asupersync::trace::independence::independent;
use asupersync::{Budget, Cx};

const SEED: u64 = 42;
const MAX_STEPS: u64 = 100_000;
const MAX_DISPATCHES: usize = 10_000;

fn config() -> LabConfig {
    LabConfig::new(SEED)
        .worker_count(1)
        .trace_capacity(1 << 16)
        .max_steps(MAX_STEPS)
}

fn report(id: &str, ok: bool, detail: impl AsRef<str>) -> bool {
    println!("CAP {id} {} {}", if ok { "ok" } else { "FAIL" }, detail.as_ref());
    ok
}

/// The spike scenario: a producer and a consumer over a bounded mpsc channel
/// (capacity 2, so the producer must wait), and a oneshot carrying the sum to
/// a third task, all in one root region. Returns the join handles, which are
/// kept alive until the run is over.
fn build(lab: &mut LabRuntime) -> impl Sized + use<> {
    let root = lab.state.create_root_region(Budget::INFINITE);
    let (tx, mut rx) = mpsc::channel::<u32>(2);
    let (sum_tx, mut sum_rx) = oneshot::channel::<u32>();

    let (producer, producer_join) = lab
        .state
        .create_task(root, Budget::INFINITE, async move {
            let cx = Cx::current().expect("producer cx");
            for i in 0..4u32 {
                tx.send(&cx, i).await.expect("mpsc send");
            }
        })
        .expect("create producer");
    let (consumer, consumer_join) = lab
        .state
        .create_task(root, Budget::INFINITE, async move {
            let cx = Cx::current().expect("consumer cx");
            let mut sum = 0u32;
            for _ in 0..4 {
                sum += rx.recv(&cx).await.expect("mpsc recv");
            }
            sum_tx.send(&cx, sum).expect("oneshot send");
        })
        .expect("create consumer");
    let (waiter, waiter_join) = lab
        .state
        .create_task(root, Budget::INFINITE, async move {
            let cx = Cx::current().expect("waiter cx");
            let sum = sum_rx.recv(&cx).await.expect("oneshot recv");
            assert_eq!(sum, 6, "0 + 1 + 2 + 3");
        })
        .expect("create waiter");

    let mut scheduler = lab.scheduler.lock();
    scheduler.schedule(producer, 0);
    scheduler.schedule(consumer, 0);
    scheduler.schedule(waiter, 0);
    (producer_join, consumer_join, waiter_join)
}

fn kind_histogram(events: &[TraceEvent]) -> String {
    let mut counts: Vec<(String, usize)> = Vec::new();
    for event in events {
        let name = format!("{:?}", event.kind);
        let name = name.split(['(', ' ', '{']).next().unwrap_or("").to_string();
        match counts.iter_mut().find(|(n, _)| *n == name) {
            Some((_, c)) => *c += 1,
            None => counts.push((name, 1)),
        }
    }
    counts
        .iter()
        .map(|(n, c)| format!("{n}={c}"))
        .collect::<Vec<_>>()
        .join(",")
}

fn main() {
    let mut all_ok = true;

    // 1. Construct LabRuntime from LabConfig {seed, worker_count, ...}.
    let mut lab = LabRuntime::new(config());
    all_ok &= report("1_lab_construct", true, format!("seed={SEED} workers=1"));

    // 4a. Forced-schedule recording must start before the first step.
    let started = lab.start_forced_schedule_recording(MAX_DISPATCHES);
    all_ok &= report("4a_forced_record_start", started.is_ok(), format!("{started:?}"));

    // 2 + 7 (partial). Real async bodies in a real region, using Cx::current()
    // and mpsc/oneshot from inside lab tasks.
    let handles = build(&mut lab);
    let steps = lab.run_until_quiescent();
    let quiescent = lab.is_quiescent();
    all_ok &= report(
        "2_spawn_and_run",
        quiescent,
        format!("steps={steps} quiescent={quiescent}"),
    );
    drop(handles);

    // 3. The full trace buffer as TraceEvent values with readable fields.
    let events = lab.trace().snapshot();
    let seq_monotonic = events.windows(2).all(|w| w[0].seq < w[1].seq);
    all_ok &= report(
        "3_trace_events",
        !events.is_empty() && seq_monotonic,
        format!(
            "events={} seq_monotonic={seq_monotonic} kinds=[{}]",
            events.len(),
            kind_histogram(&events)
        ),
    );

    // 5. The schedule certificate.
    let cert_hash = lab.certificate().hash();
    let cert_decisions = lab.certificate().decisions();
    all_ok &= report(
        "5_certificate",
        cert_decisions > 0,
        format!("hash={cert_hash:#018x} decisions={cert_decisions}"),
    );

    // 6. Foata canonical form, fingerprint and the independence relation.
    let foata = canonicalize(&events);
    let fingerprint = trace_fingerprint(&events);
    let independent_adjacent = events
        .windows(2)
        .filter(|w| independent(&w[0], &w[1]))
        .count();
    all_ok &= report(
        "6_foata_independence",
        foata.len() == events.len() && foata.fingerprint() == fingerprint,
        format!(
            "foata_depth={} foata_len={} fingerprint={fingerprint:#018x} independent_adjacent_pairs={independent_adjacent}",
            foata.depth(),
            foata.len()
        ),
    );

    // 4b. Finish recording and serialize the receipt canonically.
    let schedule: ForcedSchedule = match lab.finish_forced_schedule_recording() {
        Ok(schedule) => schedule,
        Err(err) => {
            report("4b_forced_record_finish", false, format!("{err:?}"));
            std::process::exit(1);
        }
    };
    let bytes = match schedule.to_canonical_bytes() {
        Ok(bytes) => bytes,
        Err(err) => {
            report("4b_forced_record_finish", false, format!("{err:?}"));
            std::process::exit(1);
        }
    };
    // A committed receipt is consumed from its bytes, so replay the decoded copy.
    let decoded = ForcedSchedule::try_from_canonical_bytes(
        &bytes,
        ForcedScheduleDecodeLimits::new(1 << 20, MAX_DISPATCHES, 1 << 20),
    );
    let decoded_matches = decoded.as_ref().is_ok_and(|d| *d == schedule);
    all_ok &= report(
        "4b_forced_record_finish",
        schedule.terminal_quiescent() && decoded_matches,
        format!(
            "dispatches={} terminal_steps={} canonical_bytes={} decode_roundtrip={decoded_matches}",
            schedule.dispatches().len(),
            schedule.terminal_steps(),
            bytes.len()
        ),
    );
    let schedule = decoded.unwrap_or(schedule);

    // 4c. Replay the exact receipt on a fresh runtime with the same setup.
    let mut replay = LabRuntime::new(config());
    let replay_handles = build(&mut replay);
    let forced = replay.run_forced_schedule(
        &schedule,
        ForcedScheduleLimits::new(MAX_DISPATCHES, MAX_STEPS),
    );
    drop(replay_handles);
    match forced {
        Ok(r) => {
            let replay_fingerprint = trace_fingerprint(&replay.trace().snapshot());
            all_ok &= report(
                "4c_forced_replay",
                r.quiescent && r.schedule_hash == cert_hash && replay_fingerprint == fingerprint,
                format!(
                    "dispatches={} steps={} schedule_hash_match={} trace_fingerprint_match={}",
                    r.dispatches,
                    r.steps,
                    r.schedule_hash == cert_hash,
                    replay_fingerprint == fingerprint
                ),
            );
        }
        Err(err) => all_ok &= report("4c_forced_replay", false, format!("{err:?}")),
    }

    // Seed determinism: an unforced rerun with the same seed must reproduce
    // the certificate and the trace fingerprint.
    let mut rerun = LabRuntime::new(config());
    let rerun_handles = build(&mut rerun);
    rerun.run_until_quiescent();
    drop(rerun_handles);
    let rerun_fingerprint = trace_fingerprint(&rerun.trace().snapshot());
    all_ok &= report(
        "seed_determinism",
        rerun.certificate().hash() == cert_hash && rerun_fingerprint == fingerprint,
        format!(
            "certificate_match={} trace_fingerprint_match={}",
            rerun.certificate().hash() == cert_hash,
            rerun_fingerprint == fingerprint
        ),
    );

    println!("SPIKE {}", if all_ok { "GO" } else { "NO-GO" });
    if !all_ok {
        std::process::exit(1);
    }
}
