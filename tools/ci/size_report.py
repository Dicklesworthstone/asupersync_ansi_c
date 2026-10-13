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

--require-builds is the build gate and nothing else: the exit status is
nonzero when a library build failed, a quickstart did not link, or the
kernel archive failed its closure check, whatever the sizes are. The sizes
stay observe-only (only --strict gates them). It exists because no other CI
lane builds BUILD_TYPE=release: the release library did not compile for
hours (7150161 to 69671f1, -Werror=stringop-overflow at -O2) and nothing
noticed until the report ran.

Two modes need no build and are what keeps the report's own code honest:
--selftest renders a synthetic report of the shape measure() produces, and
a JSON round trip of it, and checks the figures that come out; --render
FILE renders a recorded report.json (a CI artifact) to stdout. The text
report crashed on a real run once (`'int' object is not subscriptable`: a
per-object list that the archive totals had overwritten with a count),
which no gate could see because every gate ran with a failing `make`.

Usage:
  tools/ci/size_report.py [--out DIR] [--profiles "CORE POSIX ..."]
      [--classes "default 1 2 3"] [--types "release debug"] [--cc gcc]
      [--jobs N] [--bss-top N] [--require-builds] [--strict]
  tools/ci/size_report.py --render REPORT.json [--bss-top N]
  tools/ci/size_report.py --selftest

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
            "bss": sum(r["bss"] for r in rows), "object_count": len(rows)}


def kernel_archive_entry(archive_bytes, kobjs):
    """The report's kernel_archive record: the archive size, the totals
    (text, data, bss, object_count) and, under "objects", every object's
    own row sorted by flash, largest first. measure() and --selftest both
    build it here, so the shape the text report reads is the shape that is
    tested."""
    objs = sorted(kobjs, key=lambda r: -(r["text"] + r["data"]))
    entry = {"bytes": archive_bytes, "objects": objs}
    entry.update(totals(objs))
    return entry


def object_label(name):
    """An object's path below the build's obj/ directory (runtime/scheduler.o),
    so two objects of one basename (hooks.o) stay apart."""
    if "/obj/" in name:
        return name.rsplit("/obj/", 1)[1]
    return os.path.basename(name)


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
    row["kernel_archive"] = kernel_archive_entry(os.path.getsize(kern_lib), kobjs)

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


def build_defects(rows):
    """What the build gate fails on whatever the sizes are: a build that
    failed, a quickstart that did not link, a kernel archive that reaches
    what the kernel-only build leaves out (check_kernel_only.sh)."""
    defects = []
    for row in rows:
        if "error" in row:
            defects.append("%s: %s" % (row["tag"], row["error"]))
            continue
        for name, key in (("kernel quickstart", "quickstart_kernel"),
                          ("full quickstart", "quickstart_full")):
            entry = row[key]
            if entry["status"] != "linked":
                defects.append("%s: %s did not link (undefined: %s)" % (
                    row["tag"], name, ", ".join(entry.get("undefined") or ["see log"])))
        if not row["closure_check"]["ok"]:
            defects.append("%s: kernel-only closure check failed" % row["tag"])
    return defects


def kib(value):
    return "      -" if value is None else "%7.1f" % (value / KIB)


def object_rows(row):
    """The per-object rows of a row's kernel archive, or None where the report
    recorded only a count (a report.json from before the list had its own
    key, which has nothing to print)."""
    objs = row["kernel_archive"].get("objects")
    return objs if isinstance(objs, list) else None


def text_report(report, bss_top=20):
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
    lines.append("largest kernel objects by flash, KiB (first row of each profile, class "
                 "default or lowest):")
    seen = set()
    for row in report["rows"]:
        if "error" in row or row["profile"] in seen:
            continue
        seen.add(row["profile"])
        objs = object_rows(row)
        if objs is None:
            lines.append("  %s: per-object sizes not recorded in this report" % row["tag"])
            continue
        top = sorted(objs, key=lambda o: -(o["text"] + o["data"]))[:8]
        lines.append("  %s: " % row["tag"] + ", ".join(
            "%s %s" % (object_label(o["name"]), kib(o["text"] + o["data"]).strip())
            for o in top))
    if bss_top > 0:
        lines.append("")
        lines.append("largest kernel objects by bss, KiB (top %d of every row; bss is what "
                     "an application's RAM floor is made of):" % bss_top)
        for row in report["rows"]:
            if "error" in row:
                continue
            objs = object_rows(row)
            if objs is None:
                lines.append("  %s: per-object sizes not recorded in this report" % row["tag"])
                continue
            ka = row["kernel_archive"]
            top = sorted(objs, key=lambda o: (-o["bss"], o["name"]))[:bss_top]
            lines.append("  %s (archive bss %s KiB over %d objects, %d with bss):" % (
                row["tag"], kib(ka["bss"]).strip(), len(objs),
                len([o for o in objs if o["bss"] > 0])))
            for rank, o in enumerate(top, 1):
                if o["bss"] == 0:
                    break
                lines.append("    %2d. %-40s bss %9s  flash %9s" % (
                    rank, object_label(o["name"]), kib(o["bss"]).strip(),
                    kib(o["text"] + o["data"]).strip()))
    return "\n".join(lines) + "\n"


def synthetic_report():
    """A report of exactly the shape measure() and judge() produce, from made
    up objects: no build, no `size`. Two profiles, the default class and R1,
    one of them with a quickstart that did not link and one object name that
    two directories share (hooks.o)."""
    def obj(name, text, data, bss):
        return {"name": "/w/kernel-only/obj/" + name, "text": text, "data": data, "bss": bss}

    def linked(text, data, bss):
        return {"status": "linked", "text": text, "data": data, "bss": bss,
                "file_bytes": text + data + 4096}

    rows = []
    for profile, cls, scale in (("CORE", "default", 4), ("CORE", "1", 1),
                                ("EMBEDDED_ROUTER", "default", 4)):
        kobjs = [obj("runtime/scheduler.o", 40000, 100, 2048 * scale),
                 obj("runtime/hooks.o", 3000, 0, 64),
                 obj("platform/posix/hooks.o", 2000, 0, 0),
                 obj("net/net.o", 90000, 2000, 200000 * scale),
                 obj("core/status.o", 1500, 20, 0)]
        row = {"profile": profile, "class": cls, "build_type": "release",
               "tag": "%s-%s-release" % (profile, "R" + cls if cls != "default" else "default"),
               "full_archive": dict({"bytes": 1000000}, **totals(kobjs)),
               "kernel_archive": kernel_archive_entry(900000, kobjs),
               "closure_check": {"ok": True, "output": []},
               "quickstart_kernel": linked(70000, 500, 100000 * scale),
               "quickstart_full": linked(150000, 900, 300000 * scale)}
        judge(row)
        rows.append(row)
    rows[2]["quickstart_full"] = {"status": "link-failed", "undefined": ["asx_gone"], "tail": []}
    rows[2]["closure_check"] = {"ok": False, "output": ["asx_conformance_run reached"]}
    rows.append({"profile": "FREESTANDING", "class": "default", "build_type": "release",
                 "tag": "FREESTANDING-default-release",
                 "error": "full library build failed: make: *** Error 2"})
    return {"schema": "asx.size_report.v1", "generated_at": "2026-10-13T00:00:00Z",
            "commit": "0123456789ab", "dirty": False, "cc": "gcc", "rows": rows,
            "targets_kib": {k: list(v) for k, v in TARGETS.items()}}


def selftest_checks(check):
    report = synthetic_report()
    first = report["rows"][0]
    ka = first["kernel_archive"]
    check(isinstance(ka["objects"], list) and len(ka["objects"]) == 5,
          "kernel_archive.objects is the per-object list")
    check(ka["object_count"] == 5, "kernel_archive.object_count is the count")
    check(ka["bss"] == sum(o["bss"] for o in ka["objects"]), "archive bss is the object sum")
    check([o["name"] for o in ka["objects"]][0].endswith("net/net.o"),
          "objects are sorted by flash, largest first")
    check(first["flash_bytes"]["kernel_archive"] == ka["text"] + ka["data"],
          "judge: the archive's flash figure is text + data")
    check(first["verdict"]["kernel_archive"] == "over-hard", "judge: 135 KB is over CORE's 128 KiB")
    check(verdict("CORE", 63 * KIB) == "meets-target", "verdict: under 64 KiB meets the target")
    check(verdict("CORE", 64 * KIB) == "over-target", "verdict: 64 KiB is over the target")
    check(verdict("CORE", 128 * KIB) == "over-hard", "verdict: 128 KiB is over the hard limit")
    check(verdict("EMBEDDED_ROUTER", 127 * KIB) == "meets-target", "verdict: ROUTER target 128 KiB")
    check(verdict("POSIX", 1) == "n/a", "verdict: a profile without a target is n/a")

    defects = build_defects(report["rows"])
    check(len(defects) == 3, "build_defects: the error row, the unlinked quickstart and the "
                             "closure failure (got %d)" % len(defects))

    # The recorded-report path: what a CI artifact (report.json) goes through.
    recorded = json.loads(json.dumps(report, sort_keys=True))
    text = text_report(recorded, 20)
    check(text == text_report(report, 20), "a JSON round trip renders the same text")
    for want in ("CORE-default-release", "CORE-R1-release", "EMBEDDED_ROUTER-default-release",
                 "FREESTANDING-default-release", "ERROR full library build failed",
                 "closure check FAILED",
                 "full quickstart: undefined asx_gone", "over-hard"):
        check(want in text, "text report has %r" % want)
    flash_lines = [l for l in text.splitlines() if l.startswith("  CORE-default-release: ")]
    check(len(flash_lines) == 1 and flash_lines[0].split(": ", 1)[1].startswith("net/net.o "),
          "the flash list names net/net.o first")
    check("platform/posix/hooks.o" in text and "runtime/hooks.o" in text,
          "two hooks.o stay apart by their directory")
    section = text.split("largest kernel objects by bss", 1)
    check(len(section) == 2, "the bss section is printed")
    if len(section) == 2:
        ranked = [l for l in section[1].splitlines() if l.startswith("     1. ")]
        check(len(ranked) == 3 and all("net/net.o" in l for l in ranked),
              "each row's bss ranking starts with net/net.o")
        check("platform/posix/hooks.o" not in section[1],
              "an object with no bss is not listed in the bss ranking")
    one = text_report(recorded, 1)
    check(len([l for l in one.split("largest kernel objects by bss", 1)[1].splitlines()
               if l.startswith("     1. ")]) == 3 and "     2. " not in one,
          "--bss-top 1 prints one object per row")
    check("largest kernel objects by bss" not in text_report(recorded, 0),
          "--bss-top 0 drops the bss section")

    # A report.json written before the object list had its own key held a
    # count under "objects"; it renders, saying so, instead of crashing.
    old = json.loads(json.dumps(report))
    for row in old["rows"]:
        if "kernel_archive" in row:
            row["kernel_archive"]["objects"] = row["kernel_archive"].pop("object_count")
    old_text = text_report(old, 20)
    check("per-object sizes not recorded in this report" in old_text,
          "an old-shape report renders and says its objects were not recorded")


def selftest():
    """Checks the report's code paths without a build; returns the exit status.
    An exception is a failed check too, so a broken path is reported as one."""
    failures = []
    checks = [0]

    def check(cond, what):
        checks[0] += 1
        if not cond:
            failures.append(what)

    try:
        selftest_checks(check)
    except Exception as exc:  # pylint: disable=broad-except
        failures.append("raised %s: %s" % (type(exc).__name__, exc))

    if failures:
        print("size_report selftest: FAIL (%d of %d checks)" % (len(failures), checks[0]))
        for what in failures:
            print("  " + what)
        return 1
    print("size_report selftest: PASS (%d checks)" % checks[0])
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--selftest", action="store_true",
                    help="check the report's own code on a synthetic report; builds nothing")
    ap.add_argument("--render", metavar="REPORT.json",
                    help="print the text report of a recorded report.json; builds nothing")
    ap.add_argument("--bss-top", type=int, default=20,
                    help="objects listed per row in the bss ranking (0 = none; default 20)")
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "size-report"))
    ap.add_argument("--profiles", default="CORE POSIX FREESTANDING EMBEDDED_ROUTER")
    ap.add_argument("--classes", default="default 1 2 3")
    ap.add_argument("--types", default="release")
    ap.add_argument("--cc", default="gcc")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--require-builds", action="store_true",
                    help="exit nonzero on a build, link or closure failure; sizes stay observed")
    ap.add_argument("--strict", action="store_true")
    args = ap.parse_args()

    if args.selftest:
        return selftest()
    if args.render:
        with open(args.render, encoding="utf-8") as fh:
            sys.stdout.write(text_report(json.load(fh), args.bss_top))
        return 0

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
    text = text_report(report, args.bss_top)
    with open(os.path.join(args.out, "report.txt"), "w", encoding="utf-8") as fh:
        fh.write(text)
    sys.stdout.write(text)
    print("[asx] size-report: wrote %s and %s" % (os.path.join(args.out, "report.json"),
                                                  os.path.join(args.out, "report.txt")))

    defects = build_defects(rows)
    if args.require_builds and defects:
        print("[asx] size-report: FAIL (builds): %d defect(s)" % len(defects))
        for defect in defects:
            print("  " + defect)
        return 1
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
