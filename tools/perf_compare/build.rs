//! Links the asx side of the comparison and records the compiler that builds
//! this crate.
//!
//! The Makefile target `perf-compare` builds two archives with the library's
//! own flags (the workloads read the arena sizes, which are macros, so they
//! cannot be compiled with other flags than the library): libasx.a and
//! libasx_workloads.a. It tells this script where they are:
//!
//!   ASX_LIB_DIR        the directory of libasx.a (default: <repo>/build/lib)
//!   ASX_WORKLOADS_DIR  the directory of libasx_workloads.a
//!                      (default: <repo>/build/perf-compare)
//!
//! A missing archive fails the build with the command that makes it.

use std::env;
use std::path::PathBuf;
use std::process::Command;

fn dir_from_env(var: &str, default: PathBuf) -> PathBuf {
    println!("cargo:rerun-if-env-changed={var}");
    env::var(var).map(PathBuf::from).unwrap_or(default)
}

fn main() -> Result<(), String> {
    let manifest = PathBuf::from(
        env::var("CARGO_MANIFEST_DIR").map_err(|_| "perf_compare build: no CARGO_MANIFEST_DIR")?,
    );
    let root = manifest.join("..").join("..");

    let lib_dir = dir_from_env("ASX_LIB_DIR", root.join("build").join("lib"));
    let workloads_dir = dir_from_env("ASX_WORKLOADS_DIR", root.join("build").join("perf-compare"));
    for (dir, file) in [
        (&lib_dir, "libasx.a"),
        (&workloads_dir, "libasx_workloads.a"),
    ] {
        let path = dir.join(file);
        println!("cargo:rerun-if-changed={}", path.display());
        if !path.is_file() {
            return Err(format!(
                "perf_compare build: {} is missing; run `make perf-compare` from the repository \
                 root, which builds it and then this program",
                path.display()
            ));
        }
    }
    // The workloads archive comes first: it needs libasx.a to resolve.
    println!("cargo:rustc-link-search=native={}", workloads_dir.display());
    println!("cargo:rustc-link-search=native={}", lib_dir.display());
    println!("cargo:rustc-link-lib=static=asx_workloads");
    println!("cargo:rustc-link-lib=static=asx");
    println!("cargo:rustc-link-lib=m");
    println!("cargo:rerun-if-changed=build.rs");

    let rustc = env::var("RUSTC").unwrap_or_else(|_| "rustc".to_string());
    let output = Command::new(&rustc)
        .arg("-Vv")
        .output()
        .map_err(|e| format!("perf_compare build: cannot run {rustc} -Vv: {e}"))?;
    if !output.status.success() {
        return Err(format!("perf_compare build: {rustc} -Vv failed"));
    }
    let text = String::from_utf8(output.stdout)
        .map_err(|_| "perf_compare build: rustc -Vv is not UTF-8".to_string())?;
    let release = text
        .lines()
        .find_map(|l| l.strip_prefix("release:"))
        .map(str::trim)
        .ok_or("perf_compare build: rustc -Vv has no release line")?;
    let host = text
        .lines()
        .find_map(|l| l.strip_prefix("host:"))
        .map(str::trim)
        .ok_or("perf_compare build: rustc -Vv has no host line")?;
    println!("cargo:rustc-env=PERF_COMPARE_RUSTC_RELEASE={release}");
    println!("cargo:rustc-env=PERF_COMPARE_RUSTC_HOST={host}");
    println!("cargo:rerun-if-env-changed=RUSTC");
    Ok(())
}
