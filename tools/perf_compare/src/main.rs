//! perf_compare — asx against asupersync, one process, one minute
//! (bd-9kll.14.1).
//!
//! The same workloads run on asupersync's current-thread runtime (Rust,
//! this process) and on asx (C, through its C ABI: tools/perf_compare/c/
//! asx_workloads.c, built with the library's own flags). Samples alternate
//! between the engines, and the order within a pair alternates too, so both
//! see the same host in the same minute and a slow drift does not favor one.
//! A sample is one run of a workload of about N operations; the figure is
//! nanoseconds per operation. Per workload and engine the report gives
//! p50/p95/p99, the mean, and the coefficient of variation: a row with a cv
//! above 5% is flagged as noise and is not gated.
//!
//! What this measures and what it does not:
//! - Runtimes are built once, outside every clock. Setup inside a sample is
//!   outside the clock as well (the C side times its operations, the Rust
//!   side times from inside the parent future, as runtime_vs_tokio.rs does).
//! - asx is single-threaded with fixed arenas, so workloads that hold many
//!   live objects run in batches no larger than its arena (the batch sizes
//!   are in the report). asupersync's runtime allocates and has no such cap.
//!   The comparison is per operation, not per batch.
//! - The deterministic asx profile (virtual clock, sweep scheduler) and
//!   asupersync's wall-clock runtime do different work around the same
//!   operation; the report says so rather than hiding it.
//! - TCP request/response and HTTP/1.1 keep-alive (the other rows of
//!   runtime_vs_tokio.rs) need the POSIX live profile and a network; they
//!   are listed in the report as not measured.
//!
//! Usage: perf_compare [--out DIR] [--samples N] [--warmup N] [--scale F]
//!                     [--only a,b] [--history DIR] [--check-history]
//!                     [--update-history] [--quick]
//!
//! SPDX-License-Identifier: MIT

use std::hint::black_box;
use std::path::{Path, PathBuf};
use std::process::{Command, ExitCode};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use asupersync::Cx;
use asupersync::runtime::{Runtime, RuntimeBuilder};
use asupersync::time::TimerWheel;
use asupersync::types::{CancelReason, Time};
use serde_json::{Value, json};

/// The asupersync revision this crate links (Cargo.toml pins the same one).
const ASUPERSYNC_REV: &str = "5e60b1c4c53d62aaddae68de3ee7de4732f1755b";

/// A row whose coefficient of variation is above this is noise.
const NOISY_CV_PCT: f64 = 5.0;

/// The keep gate (bd-9kll.14.1 step 4): percent slower than the history's
/// latest run that fails the run, for the primary row, the geometric mean of
/// every gated row, and any single row.
const GATE_PRIMARY_PCT: f64 = 3.0;
const GATE_GEOMEAN_PCT: f64 = 5.0;
const GATE_ROW_PCT: f64 = 10.0;
const PRIMARY_ROW: &str = "spawn_join/asx";

unsafe extern "C" {
    fn asx_perf_spawn_join(n: u64, out_ns: *mut u64, out_ops: *mut u64) -> i32;
    fn asx_perf_yield(n: u64, out_ns: *mut u64, out_ops: *mut u64) -> i32;
    fn asx_perf_mpsc_ping_pong(n: u64, out_ns: *mut u64, out_ops: *mut u64) -> i32;
    fn asx_perf_cancel_latency(n: u64, out_ns: *mut u64, out_ops: *mut u64) -> i32;
    fn asx_perf_timer_churn(n: u64, out_ns: *mut u64, out_ops: *mut u64) -> i32;
    fn asx_perf_region_open_close(n: u64, out_ns: *mut u64, out_ops: *mut u64) -> i32;
    fn asx_perf_task_batch() -> u32;
    fn asx_perf_timer_batch() -> u32;
    fn asx_perf_region_batch() -> u32;
    fn asx_perf_describe(buf: *mut u8, cap: u32) -> u32;
}

/// One measured run: nanoseconds and the operations it covered.
#[derive(Clone, Copy)]
struct Sample {
    ns: u64,
    ops: u64,
}

impl Sample {
    fn per_op(self) -> f64 {
        if self.ops == 0 {
            0.0
        } else {
            self.ns as f64 / self.ops as f64
        }
    }
}

type AsxFn = unsafe extern "C" fn(u64, *mut u64, *mut u64) -> i32;

fn call_asx(f: AsxFn, name: &str, n: u64) -> Result<Sample, String> {
    let mut ns = 0u64;
    let mut ops = 0u64;
    // SAFETY: the workload functions write both outputs before returning 0
    // and touch nothing else of this process's memory.
    let rc = unsafe { f(n, &mut ns, &mut ops) };
    if rc == 0 {
        Ok(Sample { ns, ops })
    } else {
        Err(format!("{name}: step {rc} of the asx workload failed"))
    }
}

fn asx_spawn_join(n: u64) -> Result<Sample, String> {
    call_asx(asx_perf_spawn_join, "spawn_join", n)
}
fn asx_yield(n: u64) -> Result<Sample, String> {
    call_asx(asx_perf_yield, "yield", n)
}
fn asx_ping_pong(n: u64) -> Result<Sample, String> {
    call_asx(asx_perf_mpsc_ping_pong, "mpsc_ping_pong", n)
}
fn asx_cancel(n: u64) -> Result<Sample, String> {
    call_asx(asx_perf_cancel_latency, "cancel_latency", n)
}
fn asx_timers(n: u64) -> Result<Sample, String> {
    call_asx(asx_perf_timer_churn, "timer_churn", n)
}
fn asx_regions(n: u64) -> Result<Sample, String> {
    call_asx(asx_perf_region_open_close, "region_open_close", n)
}

// ---------------------------------------------------------------------
// asupersync's side: the patterns of benches/runtime_vs_tokio.rs
// (spawn_join, yield, mpsc_ping_pong) and spawn_throughput.rs (abort).
// ---------------------------------------------------------------------

fn nanos(d: Duration) -> u64 {
    u64::try_from(d.as_nanos()).unwrap_or(u64::MAX)
}

/// A parent spawns `n` trivial tasks with Cx::spawn, keeps every handle, and
/// joins them all (all `n` live at once: the runtime has no arena to fill).
fn rust_spawn_join(rt: &Runtime, n: u64) -> Sample {
    let count = n as usize;
    let elapsed = rt.block_on(rt.handle().spawn(async move {
        let cx = Cx::current().expect("a spawned task has a Cx");
        let start = Instant::now();
        let mut handles = Vec::with_capacity(count);
        for i in 0..count {
            handles.push(cx.spawn(move |_cx| async move { i }).expect("Cx::spawn"));
        }
        let mut sum = 0usize;
        for mut handle in handles {
            sum = sum.wrapping_add(handle.join(&cx).await.expect("join"));
        }
        black_box(sum);
        start.elapsed()
    }));
    Sample {
        ns: nanos(elapsed),
        ops: n,
    }
}

/// One task yields `n` times.
fn rust_yield(rt: &Runtime, n: u64) -> Sample {
    let count = n as usize;
    let elapsed = rt.block_on(rt.handle().spawn(async move {
        let start = Instant::now();
        for _ in 0..count {
            asupersync::runtime::yield_now().await;
        }
        start.elapsed()
    }));
    Sample {
        ns: nanos(elapsed),
        ops: n,
    }
}

/// Two tasks exchange a value `n` times over capacity-1 channels.
fn rust_ping_pong(rt: &Runtime, n: u64) -> Sample {
    let elapsed = rt.block_on(rt.handle().spawn(async move {
        let cx = Cx::current().expect("a spawned task has a Cx");
        let (ping_tx, mut ping_rx) = asupersync::channel::mpsc::channel::<u64>(1);
        let (pong_tx, mut pong_rx) = asupersync::channel::mpsc::channel::<u64>(1);
        let mut echo = cx
            .spawn(move |cx| async move {
                while let Ok(value) = ping_rx.recv(&cx).await {
                    if pong_tx.send(&cx, value + 1).await.is_err() {
                        break;
                    }
                }
            })
            .expect("spawn echo task");
        let start = Instant::now();
        let mut acc = 0u64;
        for i in 0..n {
            ping_tx.send(&cx, i).await.expect("ping");
            acc = acc.wrapping_add(pong_rx.recv(&cx).await.expect("pong"));
        }
        let elapsed = start.elapsed();
        drop(ping_tx);
        let _ = echo.join(&cx).await;
        black_box(acc);
        elapsed
    }));
    Sample {
        ns: nanos(elapsed),
        ops: n,
    }
}

/// `n` times: a started task loops on a checkpoint; the clock runs from the
/// abort request to the join returning (the task observed the cancel and
/// completed). Spawning is outside the clock.
fn rust_cancel_latency(rt: &Runtime, n: u64) -> Sample {
    let count = n as usize;
    let total = rt.block_on(rt.handle().spawn(async move {
        let cx = Cx::current().expect("a spawned task has a Cx");
        let mut total = Duration::ZERO;
        for _ in 0..count {
            let mut child = cx
                .spawn(move |child| async move {
                    loop {
                        if let Err(error) = child.checkpoint() {
                            break Err::<(), asupersync::Error>(error);
                        }
                        asupersync::runtime::yield_now().await;
                    }
                })
                .expect("Cx::spawn");
            // Let the child run once, so it has started.
            asupersync::runtime::yield_now().await;
            let start = Instant::now();
            child.abort_with_reason(CancelReason::user("perf_compare cancel latency"));
            let outcome = child.join(&cx).await;
            total += start.elapsed();
            black_box(&outcome);
        }
        total
    }));
    Sample {
        ns: nanos(total),
        ops: n,
    }
}

/// Arm `batch` timers on a wheel, cancel them all, repeat until `n` have
/// been armed and cancelled. The batch is asx's arena size, so both wheels
/// hold the same number of live timers.
fn rust_timer_churn(_rt: &Runtime, n: u64) -> Sample {
    let batch = (unsafe { asx_perf_timer_batch() } as usize).max(1);
    let waker = std::task::Waker::noop().clone();
    let mut wheel = TimerWheel::new();
    let mut handles = Vec::with_capacity(batch);
    let mut total = Duration::ZERO;
    let mut done = 0u64;
    while done < n {
        handles.clear();
        let start = Instant::now();
        for i in 0..batch {
            handles.push(wheel.register(Time::from_millis(100 + i as u64), waker.clone()));
        }
        for handle in &handles {
            black_box(wheel.cancel(handle));
        }
        total += start.elapsed();
        done += batch as u64;
    }
    Sample {
        ns: nanos(total),
        ops: done,
    }
}

// ---------------------------------------------------------------------
// The workload table
// ---------------------------------------------------------------------

struct Workload {
    name: &'static str,
    unit: &'static str,
    /// Operations a sample aims at (scaled by --scale).
    ops: u64,
    asx: fn(u64) -> Result<Sample, String>,
    rust: Option<fn(&Runtime, u64) -> Sample>,
    /// Why the Rust engine has no row, when it has none.
    rust_note: &'static str,
}

fn workloads() -> Vec<Workload> {
    vec![
        Workload {
            name: "spawn_join",
            unit: "one spawn plus one join of a trivial task",
            ops: 512,
            asx: asx_spawn_join,
            rust: Some(rust_spawn_join),
            rust_note: "",
        },
        Workload {
            name: "yield",
            unit: "one yield of a running task",
            ops: 20_000,
            asx: asx_yield,
            rust: Some(rust_yield),
            rust_note: "",
        },
        Workload {
            name: "mpsc_ping_pong",
            unit: "one round trip over two capacity-1 channels",
            ops: 2_000,
            asx: asx_ping_pong,
            rust: Some(rust_ping_pong),
            rust_note: "",
        },
        Workload {
            name: "cancel_latency",
            unit: "one cancel request to the task completed",
            ops: 200,
            asx: asx_cancel,
            rust: Some(rust_cancel_latency),
            rust_note: "",
        },
        Workload {
            name: "timer_churn",
            unit: "one timer armed and cancelled on a wheel",
            ops: 4_096,
            asx: asx_timers,
            rust: Some(rust_timer_churn),
            rust_note: "",
        },
        Workload {
            name: "region_open_close",
            unit: "one child region opened and closed",
            ops: 512,
            asx: asx_regions,
            rust: None,
            rust_note: "asupersync opens a child region through Scope::region, which takes \
                        &mut RuntimeState; the runtime's public API does not expose it, so only a \
                        lab-state variant could measure it (not built yet)",
        },
    ]
}

/// Rows of runtime_vs_tokio.rs and the bead's plan that are not run.
const NOT_MEASURED: &[(&str, &str)] = &[
    (
        "tcp_rr",
        "needs the POSIX live profile and a loopback server task on both sides",
    ),
    (
        "http1_keepalive",
        "needs the POSIX live profile; asupersync's Http1Listener against asx's http server driver",
    ),
];

// ---------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------

#[derive(Clone, Default)]
struct Stats {
    count: usize,
    min: f64,
    p50: f64,
    p95: f64,
    p99: f64,
    mean: f64,
    stddev: f64,
    cv_pct: f64,
}

fn percentile(sorted: &[f64], p: f64) -> f64 {
    if sorted.is_empty() {
        return 0.0;
    }
    let idx = (p / 100.0 * (sorted.len() - 1) as f64).round() as usize;
    sorted[idx.min(sorted.len() - 1)]
}

fn stats_of(values: &[f64]) -> Stats {
    if values.is_empty() {
        return Stats::default();
    }
    let mut sorted = values.to_vec();
    sorted.sort_by(|a, b| a.partial_cmp(b).unwrap_or(std::cmp::Ordering::Equal));
    let n = sorted.len() as f64;
    let mean = sorted.iter().sum::<f64>() / n;
    let var = if sorted.len() > 1 {
        sorted.iter().map(|v| (v - mean) * (v - mean)).sum::<f64>() / (n - 1.0)
    } else {
        0.0
    };
    let stddev = var.sqrt();
    Stats {
        count: sorted.len(),
        min: sorted[0],
        p50: percentile(&sorted, 50.0),
        p95: percentile(&sorted, 95.0),
        p99: percentile(&sorted, 99.0),
        mean,
        stddev,
        cv_pct: if mean > 0.0 {
            stddev / mean * 100.0
        } else {
            0.0
        },
    }
}

fn stats_json(s: &Stats) -> Value {
    json!({
        "samples": s.count,
        "min_ns_per_op": s.min,
        "p50_ns_per_op": s.p50,
        "p95_ns_per_op": s.p95,
        "p99_ns_per_op": s.p99,
        "mean_ns_per_op": s.mean,
        "stddev_ns_per_op": s.stddev,
        "cv_pct": s.cv_pct,
        "noisy": s.cv_pct > NOISY_CV_PCT,
    })
}

// ---------------------------------------------------------------------
// Configuration, host fingerprint, time
// ---------------------------------------------------------------------

struct Config {
    out: PathBuf,
    samples: usize,
    warmup: usize,
    scale: f64,
    only: Option<Vec<String>>,
    history: Option<PathBuf>,
    check_history: bool,
    update_history: bool,
}

fn parse_args() -> Result<Config, String> {
    let mut cfg = Config {
        out: PathBuf::from("build/perf-compare"),
        samples: 30,
        warmup: 3,
        scale: 1.0,
        only: None,
        history: None,
        check_history: false,
        update_history: false,
    };
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        let mut value = |name: &str| args.next().ok_or_else(|| format!("{name} needs a value"));
        match arg.as_str() {
            "--out" => cfg.out = PathBuf::from(value("--out")?),
            "--samples" => {
                cfg.samples = value("--samples")?
                    .parse()
                    .map_err(|_| "--samples is not a number".to_string())?
            }
            "--warmup" => {
                cfg.warmup = value("--warmup")?
                    .parse()
                    .map_err(|_| "--warmup is not a number".to_string())?
            }
            "--scale" => {
                cfg.scale = value("--scale")?
                    .parse()
                    .map_err(|_| "--scale is not a number".to_string())?
            }
            "--only" => {
                cfg.only = Some(
                    value("--only")?
                        .split(',')
                        .map(|s| s.trim().to_string())
                        .filter(|s| !s.is_empty())
                        .collect(),
                )
            }
            "--history" => cfg.history = Some(PathBuf::from(value("--history")?)),
            "--check-history" => cfg.check_history = true,
            "--update-history" => cfg.update_history = true,
            "--quick" => {
                cfg.samples = 5;
                cfg.warmup = 1;
                cfg.scale = 0.1;
            }
            other => return Err(format!("unknown argument {other}")),
        }
    }
    if cfg.samples < 2 {
        return Err("--samples must be at least 2 (a cv needs two)".to_string());
    }
    if !(cfg.scale > 0.0) {
        return Err("--scale must be positive".to_string());
    }
    Ok(cfg)
}

fn read_first_line_with(path: &str, key: &str) -> Option<String> {
    let text = std::fs::read_to_string(path).ok()?;
    text.lines()
        .find(|l| l.starts_with(key))
        .and_then(|l| l.split_once(':'))
        .map(|(_, v)| v.trim().to_string())
}

/// The output of `git <args>` run in `cwd`, trimmed; None if it fails.
fn git_output(args: &[&str], cwd: &Path) -> Option<String> {
    let out = Command::new("git")
        .args(args)
        .current_dir(cwd)
        .output()
        .ok()?;
    if !out.status.success() {
        return None;
    }
    Some(String::from_utf8_lossy(&out.stdout).trim().to_string())
}

/// Seconds since the epoch as an ISO-8601 UTC string (days to civil date,
/// Hinnant's algorithm), so no date crate is needed.
fn utc_iso(secs: u64) -> String {
    let days = (secs / 86_400) as i64;
    let rem = secs % 86_400;
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1_460 + doe / 36_524 - doe / 146_096) / 365;
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = if m <= 2 { y + 1 } else { y };
    format!(
        "{y:04}-{m:02}-{d:02}T{:02}:{:02}:{:02}Z",
        rem / 3_600,
        rem % 3_600 / 60,
        rem % 60
    )
}

fn describe_asx() -> String {
    let mut buf = vec![0u8; 512];
    // SAFETY: the callee writes at most `cap` bytes, NUL-terminated.
    let len = unsafe { asx_perf_describe(buf.as_mut_ptr(), buf.len() as u32) } as usize;
    String::from_utf8_lossy(&buf[..len.min(buf.len())]).to_string()
}

fn host_fingerprint(repo: &Path) -> Value {
    let cpu = read_first_line_with("/proc/cpuinfo", "model name").unwrap_or_default();
    let kernel = std::fs::read_to_string("/proc/sys/kernel/osrelease")
        .map(|s| s.trim().to_string())
        .unwrap_or_default();
    let commit = git_output(&["rev-parse", "--short=12", "HEAD"], repo).unwrap_or_default();
    let dirty = git_output(&["status", "--porcelain", "--untracked-files=no"], repo)
        .map(|s| !s.is_empty())
        .unwrap_or(false);
    json!({
        "cpu_model": cpu,
        "logical_cpus": std::thread::available_parallelism().map(|n| n.get()).unwrap_or(0),
        "kernel": kernel,
        "os": std::env::consts::OS,
        "arch": std::env::consts::ARCH,
        "rustc_release": env!("PERF_COMPARE_RUSTC_RELEASE"),
        "rustc_host": env!("PERF_COMPARE_RUSTC_HOST"),
        "asupersync_rev": ASUPERSYNC_REV,
        "asx_commit": commit,
        "asx_tree_dirty": dirty,
        "asx_library": describe_asx(),
    })
}

// ---------------------------------------------------------------------
// Running
// ---------------------------------------------------------------------

struct Row {
    workload: &'static str,
    unit: &'static str,
    ops_per_sample: u64,
    asx: Stats,
    asx_errors: Vec<String>,
    rust: Option<Stats>,
    rust_note: &'static str,
}

fn run_workload(w: &Workload, rt: &Runtime, cfg: &Config) -> Row {
    let ops = ((w.ops as f64 * cfg.scale).round() as u64).max(1);
    let mut asx_vals = Vec::with_capacity(cfg.samples);
    let mut rust_vals = Vec::with_capacity(cfg.samples);
    let mut asx_errors = Vec::new();

    for round in 0..(cfg.warmup + cfg.samples) {
        let measured = round >= cfg.warmup;
        // Alternate which engine goes first inside a pair.
        let asx_first = round % 2 == 0;
        for step in 0..2 {
            let run_asx = (step == 0) == asx_first;
            if run_asx {
                match (w.asx)(ops) {
                    Ok(s) if measured => asx_vals.push(s.per_op()),
                    Ok(_) => {}
                    Err(e) => {
                        if asx_errors.len() < 4 {
                            asx_errors.push(e);
                        }
                    }
                }
            } else if let Some(f) = w.rust {
                let s = f(rt, ops);
                if measured {
                    rust_vals.push(s.per_op());
                }
            }
        }
    }
    Row {
        workload: w.name,
        unit: w.unit,
        ops_per_sample: ops,
        asx: stats_of(&asx_vals),
        asx_errors,
        rust: w.rust.map(|_| stats_of(&rust_vals)),
        rust_note: w.rust_note,
    }
}

fn row_json(r: &Row) -> Value {
    let ratio = match (&r.rust, r.asx.p50) {
        (Some(rs), a) if rs.p50 > 0.0 && a > 0.0 => json!(a / rs.p50),
        _ => Value::Null,
    };
    json!({
        "workload": r.workload,
        "unit": r.unit,
        "ops_per_sample": r.ops_per_sample,
        "asx": stats_json(&r.asx),
        "asx_errors": r.asx_errors,
        "asupersync": r.rust.as_ref().map(stats_json),
        "asupersync_note": r.rust_note,
        "asx_over_asupersync_p50": ratio,
    })
}

fn fmt_ns(v: f64) -> String {
    if v >= 1_000.0 {
        format!("{:.2}us", v / 1_000.0)
    } else {
        format!("{v:.1}ns")
    }
}

fn text_report(host: &Value, started: &str, rows: &[Row], cfg: &Config) -> String {
    let mut out = String::new();
    out.push_str(&format!(
        "perf_compare (bd-9kll.14.1)  asx {} on {}  started {started}\n",
        host["asx_commit"].as_str().unwrap_or("?"),
        host["cpu_model"].as_str().unwrap_or("?"),
    ));
    out.push_str(&format!(
        "{} samples of the workload per engine after {} warm-up, engines alternating; ns per operation\n",
        cfg.samples, cfg.warmup
    ));
    out.push_str(&format!(
        "asx library: {}\n\n",
        host["asx_library"].as_str().unwrap_or("")
    ));
    out.push_str(&format!(
        "{:<20} {:<11} {:>10} {:>10} {:>10} {:>10} {:>7}  {}\n",
        "workload", "engine", "p50", "p95", "p99", "mean", "cv%", "note"
    ));
    for r in rows {
        let line = |engine: &str, s: &Stats, note: &str| {
            format!(
                "{:<20} {:<11} {:>10} {:>10} {:>10} {:>10} {:>7.1}  {}\n",
                r.workload,
                engine,
                fmt_ns(s.p50),
                fmt_ns(s.p95),
                fmt_ns(s.p99),
                fmt_ns(s.mean),
                s.cv_pct,
                note
            )
        };
        let noisy = |s: &Stats| {
            if s.cv_pct > NOISY_CV_PCT {
                "NOISY (cv > 5%)"
            } else {
                ""
            }
        };
        if r.asx.count == 0 {
            out.push_str(&format!(
                "{:<20} {:<11} FAILED: {}\n",
                r.workload,
                "asx",
                r.asx_errors
                    .first()
                    .map(String::as_str)
                    .unwrap_or("no sample")
            ));
        } else {
            out.push_str(&line("asx", &r.asx, noisy(&r.asx)));
        }
        match &r.rust {
            Some(s) => out.push_str(&line("asupersync", s, noisy(s))),
            None => out.push_str(&format!(
                "{:<20} {:<11} not measured: {}\n",
                r.workload, "asupersync", r.rust_note
            )),
        }
        if let Some(s) = &r.rust {
            if s.p50 > 0.0 && r.asx.p50 > 0.0 {
                out.push_str(&format!(
                    "{:<20} {:<11} asx p50 / asupersync p50 = {:.2}\n",
                    "",
                    "",
                    r.asx.p50 / s.p50
                ));
            }
        }
    }
    out.push_str("\nnot run:\n");
    for (name, why) in NOT_MEASURED {
        out.push_str(&format!("  {name}: {why}\n"));
    }
    out
}

// ---------------------------------------------------------------------
// History ratchet
// ---------------------------------------------------------------------

fn history_file(dir: &Path) -> PathBuf {
    dir.join("perf_compare.latest.json")
}

/// Rows the gate looks at: asx p50 by "workload/asx", not noisy.
fn gated_rows(rows: &[Row]) -> Vec<(String, f64, bool)> {
    rows.iter()
        .filter(|r| r.asx.count > 0)
        .map(|r| {
            (
                format!("{}/asx", r.workload),
                r.asx.p50,
                r.asx.cv_pct > NOISY_CV_PCT,
            )
        })
        .collect()
}

/// Compare with the history's latest run; returns the failure lines.
fn check_history(prev: &Value, now: &[(String, f64, bool)]) -> (Vec<String>, Vec<String>) {
    let mut failures = Vec::new();
    let mut notes = Vec::new();
    let mut logs = Vec::new();
    let empty = serde_json::Map::new();
    let before = prev["asx_p50_ns_per_op"].as_object().unwrap_or(&empty);
    for (name, p50, noisy) in now {
        let Some(old) = before.get(name).and_then(Value::as_f64) else {
            notes.push(format!("{name}: no earlier figure"));
            continue;
        };
        if old <= 0.0 || *p50 <= 0.0 {
            continue;
        }
        let change = (p50 - old) / old * 100.0;
        if *noisy {
            notes.push(format!("{name}: {change:+.1}% (noisy row, not gated)"));
            continue;
        }
        logs.push((name.clone(), (p50 / old).ln()));
        if change > GATE_ROW_PCT {
            failures.push(format!(
                "{name}: {change:+.1}% slower (limit {GATE_ROW_PCT}%)"
            ));
        } else if name == PRIMARY_ROW && change > GATE_PRIMARY_PCT {
            failures.push(format!(
                "{name}: primary row {change:+.1}% slower (limit {GATE_PRIMARY_PCT}%)"
            ));
        } else {
            notes.push(format!("{name}: {change:+.1}%"));
        }
    }
    if !logs.is_empty() {
        let mean_log = logs.iter().map(|(_, l)| l).sum::<f64>() / logs.len() as f64;
        let geo = (mean_log.exp() - 1.0) * 100.0;
        if geo > GATE_GEOMEAN_PCT {
            failures.push(format!(
                "geometric mean {geo:+.1}% slower over {} rows (limit {GATE_GEOMEAN_PCT}%)",
                logs.len()
            ));
        } else {
            notes.push(format!(
                "geometric mean {geo:+.1}% over {} rows",
                logs.len()
            ));
        }
    }
    (failures, notes)
}

// ---------------------------------------------------------------------
// main
// ---------------------------------------------------------------------

fn main() -> ExitCode {
    match run() {
        Ok(code) => code,
        Err(message) => {
            eprintln!("perf_compare: {message}");
            ExitCode::from(2)
        }
    }
}

fn run() -> Result<ExitCode, String> {
    let cfg = parse_args()?;
    let repo = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("..")
        .join("..");
    let rt = RuntimeBuilder::current_thread()
        .build()
        .map_err(|e| format!("cannot build asupersync's current-thread runtime: {e:?}"))?;

    let started_secs = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    let started = utc_iso(started_secs);
    let wall = Instant::now();
    let host = host_fingerprint(&repo);

    let mut rows = Vec::new();
    for w in workloads() {
        if let Some(only) = &cfg.only {
            if !only.iter().any(|n| n == w.name) {
                continue;
            }
        }
        eprintln!("perf_compare: {} ...", w.name);
        rows.push(run_workload(&w, &rt, &cfg));
    }
    let duration = wall.elapsed();

    // SAFETY: plain reads of the library's compile-time arena sizes.
    let (task_batch, timer_batch, region_batch) = unsafe {
        (
            asx_perf_task_batch(),
            asx_perf_timer_batch(),
            asx_perf_region_batch(),
        )
    };
    let report = json!({
        "schema": "asx.perf_compare.v1",
        "generated_at": started,
        "duration_s": duration.as_secs_f64(),
        "host": host,
        "method": {
            "samples": cfg.samples,
            "warmup": cfg.warmup,
            "scale": cfg.scale,
            "alternating": true,
            "noisy_cv_pct": NOISY_CV_PCT,
            "asx_batches": {
                "tasks": task_batch,
                "timers": timer_batch,
                "regions": region_batch,
            },
        },
        "rows": rows.iter().map(row_json).collect::<Vec<_>>(),
        "not_measured": NOT_MEASURED
            .iter()
            .map(|(n, w)| json!({"workload": n, "reason": w}))
            .collect::<Vec<_>>(),
    });
    let text = text_report(&report["host"], &started, &rows, &cfg);

    std::fs::create_dir_all(&cfg.out).map_err(|e| format!("{}: {e}", cfg.out.display()))?;
    let json_path = cfg.out.join("report.json");
    let text_path = cfg.out.join("report.txt");
    let pretty = serde_json::to_string_pretty(&report).map_err(|e| e.to_string())?;
    std::fs::write(&json_path, format!("{pretty}\n"))
        .map_err(|e| format!("{}: {e}", json_path.display()))?;
    std::fs::write(&text_path, &text).map_err(|e| format!("{}: {e}", text_path.display()))?;
    print!("{text}");
    println!(
        "perf_compare: wrote {} and {}",
        json_path.display(),
        text_path.display()
    );

    let failed_rows: Vec<&Row> = rows.iter().filter(|r| r.asx.count == 0).collect();
    if !failed_rows.is_empty() {
        for r in &failed_rows {
            eprintln!(
                "perf_compare: FAIL: {} produced no asx sample ({})",
                r.workload,
                r.asx_errors
                    .first()
                    .map(String::as_str)
                    .unwrap_or("no error")
            );
        }
        return Ok(ExitCode::from(1));
    }

    let mut exit = ExitCode::SUCCESS;
    if let Some(dir) = &cfg.history {
        let file = history_file(dir);
        let now = gated_rows(&rows);
        let previous: Option<Value> = std::fs::read_to_string(&file)
            .ok()
            .and_then(|s| serde_json::from_str(&s).ok());
        let mut kept = true;
        if let Some(prev) = &previous {
            let (failures, notes) = check_history(prev, &now);
            for n in &notes {
                println!("perf_compare: history: {n}");
            }
            for f in &failures {
                println!("perf_compare: history: FAIL: {f}");
            }
            if !failures.is_empty() {
                kept = false;
                if cfg.check_history {
                    exit = ExitCode::from(1);
                }
            }
        } else {
            println!(
                "perf_compare: history: no earlier run in {}",
                file.display()
            );
        }
        if cfg.update_history {
            if kept {
                let rows_json: serde_json::Map<String, Value> = now
                    .iter()
                    .map(|(n, p50, _)| (n.clone(), json!(p50)))
                    .collect();
                let latest = json!({
                    "schema": "asx.perf_compare.history.v1",
                    "generated_at": report["generated_at"],
                    "asx_commit": report["host"]["asx_commit"],
                    "asx_p50_ns_per_op": rows_json,
                });
                std::fs::create_dir_all(dir).map_err(|e| format!("{}: {e}", dir.display()))?;
                let body = serde_json::to_string_pretty(&latest).map_err(|e| e.to_string())?;
                std::fs::write(&file, format!("{body}\n"))
                    .map_err(|e| format!("{}: {e}", file.display()))?;
                println!("perf_compare: history: updated {}", file.display());
            } else {
                println!("perf_compare: history: not updated (the run was rejected)");
            }
        }
    } else if cfg.check_history || cfg.update_history {
        return Err("--check-history and --update-history need --history DIR".to_string());
    }
    Ok(exit)
}
