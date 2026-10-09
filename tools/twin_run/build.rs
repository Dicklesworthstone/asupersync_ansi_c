//! Records the compiler that builds twin_run, so fixture provenance names the
//! toolchain that produced the capture rather than whatever `rustc` happens to
//! be on PATH when the capture runs. Any failure here fails the build.

use std::process::Command;

fn main() -> Result<(), String> {
    let rustc = std::env::var("RUSTC").unwrap_or_else(|_| "rustc".to_string());
    let output = Command::new(&rustc)
        .arg("-Vv")
        .output()
        .map_err(|e| format!("twin_run build: cannot run {rustc} -Vv: {e}"))?;
    if !output.status.success() {
        return Err(format!("twin_run build: {rustc} -Vv failed"));
    }
    let text = String::from_utf8(output.stdout).map_err(|_| "twin_run build: rustc -Vv is not UTF-8".to_string())?;
    for (key, var) in [
        ("commit-hash:", "TWIN_RUN_RUSTC_COMMIT_HASH"),
        ("release:", "TWIN_RUN_RUSTC_RELEASE"),
        ("host:", "TWIN_RUN_RUSTC_HOST"),
    ] {
        let value = text
            .lines()
            .find_map(|l| l.strip_prefix(key))
            .map(str::trim)
            .ok_or_else(|| format!("twin_run build: rustc -Vv has no {key} line"))?;
        println!("cargo:rustc-env={var}={value}");
    }
    println!("cargo:rerun-if-env-changed=RUSTC");
    Ok(())
}
