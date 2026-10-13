#!/usr/bin/env python3
"""size_report.py — footprint of the kernel-only build per profile and class
(bd-9kll.13.3, owner decision DEC-011/D6; plan section 11).

For every profile x resource class x build type this builds, with the
project Makefile:

  * the kernel-only archive (make kernel-only: the library without the
    conformance interpreter, the CLI and the protocol models, compiled with
    -DASX_KERNEL_ONLY=1) and the full library archive;
  * examples/ex_quickstart.c linked against each of them.

and reads the sizes back with `size`: text/data/bss per object file of the
kernel archive, the archive totals, and the linked quickstart ELF (what an
application pays). The footprint is compared with the plan's targets:

  ASX_PROFILE_CORE             kernel-only < 64 KiB   (hard fail < 128 KiB)
  ASX_PROFILE_EMBEDDED_ROUTER  with trace < 128 KiB   (hard fail < 256 KiB)

The compared figure is text + data (what sits in flash; bss is RAM and is
reported beside it). KiB means 1024 bytes. The report is observe-only: it
exits 0 whatever it finds unless --strict is given, and it never edits a
baseline (tools/ci/size_cold_start_baselines.json is not read or written).

The numbers are those of the build host's compiler and target; a
cross-compiled target differs. A link that fails is recorded as such with
the first undefined symbols, not as a size.

Usage:
  tools/ci/size_report.py [--out DIR] [--profiles "CORE POSIX ..."]
      [--classes "default 1 2 3"] [--types "release debug"] [--cc gcc]
      [--jobs N] [--strict]

SPDX-License-Identifier: MIT
"""

import argparse
import datetime
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
KIB = 1024

# Plan section 11 footprint targets, KiB: (target, hard fail threshold).
TARGETS = {
    "CORE": (64, 128),
    "EMBEDDED_ROUTER": (128, 256),
}

# Extra make arguments a profile needs to build and link on the host, as
# the Makefile's own build-<profile> targets give them.
PROFILE_ARGS = {
    "POSIX": ["DETERMINISTIC=0", "LDFLAGS=-lpthread -lrt",
              "CFLAGS=-DASX_LOCKFREE_SINGLE_THREAD=0"],
    "PARALLEL": ["CFLAGS=-DASX_LOCKFREE_SINGLE_THREAD=0"],
}

BUILD_TIMEOUT_S = 3600


def run(cmd, log_path, extra_env=None):
    """Run a command, tee its combined output to log_path; return (rc, text)."""
    env = dict(os.environ)
    if extra_env:
        env.update(extra_env)
    try:
        proc = subprocess.run(cmd, cwd=ROOT, env=env, capture_output=True, text=True,
                              timeout=BUILD_TIMEOUT_S, check=False)
        rc, out = proc.returncode, proc.stdout + proc.stderr
    except subprocess.TimeoutExpired as exc:
        rc, out = 124, "timeout after %ds\n%s" % (BUILD_TIMEOUT_S, exc.stdout or "")
    os.makedirs(os.path.dirname(log_path), exist_ok=True)
    with open(log_path, "w", encoding="utf-8") as fh:
        fh.write(" ".join(cmd) + "\n" + out)
    return rc, out


SIZE_LINE = re.compile(r"^\s*(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+[0-9a-fA-F]+\s+(\S.*)$")


def size_of(paths):
    """`size` (Berkeley) per file: [{'name', 'text', 'data', 'bss'}]."""
    rows = []
    for i in range(0, len(paths), 64):
        argv = ["size"]
        argv.extend(paths[i:i + 64])
        proc = subprocess.run(argv, capture_output=True, text=True, timeout=300, check=False)
        for line in proc.stdout.splitlines():
            m = SIZE_LINE.match(line)
            if m:
                rows.append({"name": m.group(5), "text": int(m.group(1)),
                             "data": int(m.group(2)), "bss": int(m.group(3))})
    return rows


def totals(rows):
    return {"text": sum(r["text"] for r in rows), "data": sum(r["data"] for r in rows),
            "bss": sum(r["bss"] for r in rows), "objects": len(rows)}


def objects_under(directory):
    found = []
    for base, _dirs, files in os.walk(directory):
        for name in files:
            if name.endswith(".o"):
                found.append(os.path.join(base, name))
    return sorted(found)


def undefined_symbols(link_output):
    syms = re.findall(r"undefined reference to [`']([^'`]+)'", link_output)
    seen = []
    for s in syms:
        if s not in seen:
            seen.append(s)
    return seen[:8]


def make_cmd(profile, cls, btype, cc, jobs, build_dir, goals):
    cmd = ["make", "-C", ROOT, "--no-print-directory", "-j", str(jobs),
           "BUILD_DIR=" + build_dir, "PROFILE=" + profile, "BUILD_TYPE=" + btype,
           "CC=" + cc]
    if cls != "default":
        cmd.append("RESOURCE_CLASS=" + cls)
    cmd.extend(PROFILE_ARGS.get(profile, []))
    cmd.extend(goals)
    return cmd


def link_result(path, rc, out):
    if rc == 0 and os.path.isfile(path):
        rows = size_of([path])
        if rows:
            r = rows[0]
            return {"status": "linked", "text": r["text"], "data": r["data"], "bss": r["bss"],
                    "file_bytes": os.path.getsize(path)}
    return {"status": "link-failed", "undefined": undefined_symbols(out),
            "tail": out.strip().splitlines()[-3:]}


def measure(profile, cls, btype, args):
    tag = "%s-%s-%s" % (profile, "R" + cls if cls != "default" else "default", btype)
    base = os.path.join(args.out, "work", tag)
    logs = os.path.join(args.out, "logs", tag)
    row = {"profile": profile, "class": cls, "build_type": btype, "tag": tag}

    full_dir = os.path.join(base, "full")
    kern_dir = os.path.join(base, "kernel")
    full_lib = os.path.join(full_dir, "lib", "libasx.a")
    kern_lib = os.path.join(kern_dir, "kernel-only", "lib", "libasx_kernel.a")
    full_app = os.path.join(full_dir, "examples", "ex_quickstart")
    kern_app = os.path.join(kern_dir, "kernel-only", "bin", "ex_quickstart")

    rc, out = run(make_cmd(profile, cls, btype, args.cc, args.jobs, full_dir, [full_lib]),
                  os.path.join(logs, "full-lib.log"))
    if rc != 0:
        row["error"] = "full library build failed: " + " | ".join(out.strip().splitlines()[-2:])
        return row
    row["full_archive"] = {"bytes": os.path.getsize(full_lib)}
    full_objs = size_of(objects_under(os.path.join(full_dir, "obj")))
    row["full_archive"].update(totals(full_objs))

    rc, out = run(make_cmd(profile, cls, btype, args.cc, args.jobs, kern_dir, ["kernel-only"]),
                  os.path.join(logs, "kernel-lib.log"))
    if rc != 0 or not os.path.isfile(kern_lib):
        row["error"] = "kernel-only build failed: " + " | ".join(out.strip().splitlines()[-2:])
        return row
    kobjs = size_of(objects_under(os.path.join(kern_dir, "kernel-only", "obj")))
    kobjs.sort(key=lambda r: -(r["text"] + r["data"]))
    row["kernel_archive"] = {"bytes": os.path.getsize(kern_lib), "objects": kobjs}
    row["kernel_archive"].update(totals(kobjs))

    rc, out = run(["bash", os.path.join(ROOT, "tools", "ci", "check_kernel_only.sh"),
                   full_lib, kern_lib], os.path.join(logs, "closure-check.log"))
    row["closure_check"] = {"ok": rc == 0, "output": out.strip().splitlines()[-6:]}

    rc, out = run(make_cmd(profile, cls, btype, args.cc, args.jobs, full_dir, [full_app]),
                  os.path.join(logs, "full-quickstart.log"))
    row["quickstart_full"] = link_result(full_app, rc, out)
    rc, out = run(make_cmd(profile, cls, btype, args.cc, args.jobs, kern_dir,
                           ["kernel-only-quickstart"]),
                  os.path.join(logs, "kernel-quickstart.log"))
    row["quickstart_kernel"] = link_result(kern_app, rc, out)
    return row


def flash(entry):
    if not entry or "text" not in entry or "data" not in entry:
        return None
    return entry["text"] + entry["data"]


def verdict(profile, value):
    """meets-target / over-target / over-hard / n/a for a flash figure."""
    if profile not in TARGETS or value is None:
        return "n/a"
    target, hard = TARGETS[profile]
    if value < target * KIB:
        return "meets-target"
    if value < hard * KIB:
        return "over-target"
    return "over-hard"


def judge(row):
    profile = row["profile"]
    row["targets_kib"] = list(TARGETS[profile]) if profile in TARGETS else None
    karch = flash(row.get("kernel_archive"))
    klink = flash(row.get("quickstart_kernel"))
    flnk = flash(row.get("quickstart_full"))
    row["flash_bytes"] = {"kernel_archive": karch, "kernel_quickstart": klink,
                          "full_quickstart": flnk}
    row["verdict"] = {"kernel_archive": verdict(profile, karch),
                      "kernel_quickstart": verdict(profile, klink),
                      "full_quickstart": verdict(profile, flnk)}


def kib(value):
    return "      -" if value is None else "%7.1f" % (value / KIB)


def text_report(report):
    lines = []
    lines.append("asx footprint report (bd-9kll.13.3, DEC-011/D6)  commit %s%s  cc %s  %s" % (
        report["commit"], " (dirty tree)" if report["dirty"] else "", report["cc"],
        report["generated_at"]))
    lines.append("flash = text + data in KiB; ram = bss in KiB; targets: CORE kernel-only "
                 "64 (hard 128), EMBEDDED_ROUTER 128 (hard 256)")
    lines.append("")
    lines.append("%-34s %9s %9s %14s %14s %12s" % (
        "config", "kern.arch", "arch.bss", "kern.quickst.", "full quickst.", "verdict"))
    lines.append("%-34s %9s %9s %14s %14s %12s" % (
        "", "flash", "ram", "flash / ram", "flash / ram", "(archive)"))
    for row in report["rows"]:
        if "error" in row:
            lines.append("%-34s ERROR %s" % (row["tag"], row["error"]))
            continue
        ka = row["kernel_archive"]
        qk = row["quickstart_kernel"]
        qf = row["quickstart_full"]

        def q(entry):
            if entry["status"] != "linked":
                return "link-failed"
            return "%s /%s" % (kib(entry["text"] + entry["data"]), kib(entry["bss"]))

        lines.append("%-34s %9s %9s %14s %14s %12s" % (
            row["tag"], kib(ka["text"] + ka["data"]).strip(), kib(ka["bss"]).strip(),
            q(qk), q(qf), row["verdict"]["kernel_archive"]))
        if not row["closure_check"]["ok"]:
            lines.append("    closure check FAILED: " + "; ".join(row["closure_check"]["output"]))
        for entry, name in ((qk, "kernel"), (qf, "full")):
            if entry["status"] != "linked":
                lines.append("    %s quickstart: undefined %s" % (
                    name, ", ".join(entry.get("undefined") or ["(see log)"])))
    lines.append("")
    lines.append("largest kernel objects (first row of each profile, class default or lowest):")
    seen = set()
    for row in report["rows"]:
        if "error" in row or row["profile"] in seen:
            continue
        seen.add(row["profile"])
        top = row["kernel_archive"]["objects"][:8]
        lines.append("  %s: " % row["tag"] + ", ".join(
            "%s %s" % (os.path.basename(o["name"]), kib(o["text"] + o["data"]).strip())
            for o in top))
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "size-report"))
    ap.add_argument("--profiles", default="CORE POSIX FREESTANDING EMBEDDED_ROUTER")
    ap.add_argument("--classes", default="default 1 2 3")
    ap.add_argument("--types", default="release")
    ap.add_argument("--cc", default="gcc")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--strict", action="store_true")
    args = ap.parse_args()

    commit = subprocess.run(["git", "rev-parse", "--short=12", "HEAD"], cwd=ROOT,
                            capture_output=True, text=True, timeout=60, check=False).stdout.strip()
    dirty = bool(subprocess.run(["git", "status", "--porcelain", "--untracked-files=no"],
                                cwd=ROOT, capture_output=True, text=True, timeout=60,
                                check=False).stdout.strip())
    rows = []
    for profile in args.profiles.split():
        for cls in args.classes.split():
            for btype in args.types.split():
                print("[asx] size-report: %s class=%s %s ..." % (profile, cls, btype),
                      flush=True)
                row = measure(profile, cls, btype, args)
                if "error" not in row:
                    judge(row)
                rows.append(row)

    report = {
        "schema": "asx.size_report.v1",
        "generated_at": datetime.datetime.now(datetime.timezone.utc).strftime(
            "%Y-%m-%dT%H:%M:%SZ"),
        "commit": commit, "dirty": dirty, "cc": args.cc,
        "metric": "flash = text + data of the Berkeley `size` output, KiB = 1024 bytes",
        "targets_kib": {k: list(v) for k, v in TARGETS.items()},
        "rows": rows,
    }
    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, "report.json"), "w", encoding="utf-8") as fh:
        json.dump(report, fh, indent=1, sort_keys=True)
        fh.write("\n")
    text = text_report(report)
    with open(os.path.join(args.out, "report.txt"), "w", encoding="utf-8") as fh:
        fh.write(text)
    sys.stdout.write(text)
    print("[asx] size-report: wrote %s and %s" % (os.path.join(args.out, "report.json"),
                                                  os.path.join(args.out, "report.txt")))

    broken = [r for r in rows if "error" in r]
    over_hard = [r for r in rows if "error" not in r and
                 r["verdict"]["kernel_archive"] == "over-hard"]
    if args.strict and (broken or over_hard):
        print("[asx] size-report: FAIL (strict): %d build error(s), %d over the hard threshold" %
              (len(broken), len(over_hard)))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
