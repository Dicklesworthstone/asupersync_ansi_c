#!/usr/bin/env python3
"""parser_differential.py — differential parser fuzzing, Rust vs C
(bd-9kll.16.2, plan section 10.7).

The same bytes go to asupersync's parser (pinned rev 5e60b1c4c, through
`twin_run parse-diff`, tools/twin_run/src/parsediff.rs) and to the C parser
(the ASX_DIFF_* mode of tests/fuzz/fuzz_parsers.c). Each prints one JSON
outcome per input; this script compares them:

  1. accepted or rejected (a complete message or frame versus anything
     else);
  2. when both accept: method and target (or status and reason), version,
     headers, trailers, body length and body digest, and the bytes the first
     message used; for a WebSocket frame, fin, rsv, opcode, masked, mask
     key, payload length and digest, and the bytes used;
  3. when both reject a frame: the close code the receiver fails the
     connection with (it goes on the wire; a cut-short frame has none).

The targets: http-request, http-response (a case per request method, GET,
HEAD and CONNECT, since the method frames the body) and ws-frame (a case per
receiver role, client and server, since the masking rule differs).

Two rejections are not a divergence (the parsers name their reasons
differently); the reasons are recorded in the statistics only.

`run` does, for one HTTP request corpus:

  * `twin_run parse-fuzz http-request --seed S --count N --out DIR` writes the
    inputs (a fixed list of well-formed requests, then seeded mutations of
    them: deterministic for a given S and N) and the Rust outcomes;
  * the C driver runs the same files;
  * each differing input gets a signature (below), inputs are grouped by it,
    and the smallest member of each group is minimized with a batched
    delta-debugging pass that keeps the signature unchanged;
  * report.txt / report.json list every group: signature, count, one
    example input, the minimized input and both outcomes; beads.sh holds one
    `br create` line per group (not run: a bead per divergence is recorded by
    whoever triages the run).

Signatures (stable across runs and across minimization):

  rust-panic                         the Rust parser panicked
  c-crash                            the C driver died on the input
  accept/reject:rust=<d> c=<d>       the first side accepted, the second did
                                     not (c-accepts or rust-accepts, with the
                                     rejecting side's reason)
  both-accept:<field>[:<detail>]     both accepted, <field> differs
  both-reject:close-code:<r>/<c>     both rejected a frame, with different
                                     close codes (Rust / C)

Modes: observe (default) prints and exits 0; gate exits 1 when a signature
is not listed in --known (`signature | bead | note` per line), so a new
divergence fails and a recorded one does not. Exit 2 is a tool error (a
runner failed, the corpus is empty), never a pass.

Aids for looking behind a divergence that hides others (each is recorded
in the report header, so a report says what it assumed):
  --fold-header-case    compare header names case-insensitively
  --c-require-host 0    the C parser does not require Host (the Rust one
                        does not, so an accept/reject split on it is one
                        signature that hides everything else on those inputs)

--selftest needs neither binary: it runs the comparison, grouping and
minimization over a pair of synthetic parsers and checks what comes out.
"""

import argparse
import json
import os
import shlex
import shutil
import subprocess
import sys

BATCH = 400  # files per process launch (argv length, and a crash's blast radius)
BATCH_TIMEOUT = 300  # seconds for one batch
ONE_TIMEOUT = 20  # seconds for one input, when a batch has to be split


# ---------------------------------------------------------------------------
# Comparison
# ---------------------------------------------------------------------------


def is_accept(outcome):
    return outcome.get("kind") == "complete"


def reject_reason(outcome):
    return str(outcome.get("detail", outcome.get("kind", "?")))


def rust_view(text):
    """The string asupersync's decoder makes of a header value whose bytes the
    C side printed as the Latin-1 code points of `text`: valid UTF-8 stays
    UTF-8, anything else is decoded byte for byte as Latin-1. The Rust side
    prints the string it holds, so the C bytes are given the same decoding
    before the two are compared."""
    try:
        return text.encode("latin-1").decode("utf-8")
    except UnicodeError:
        return text


def header_diff(rust, c, fold):
    """The detail of the first difference between two header lists."""
    if len(rust) != len(c):
        return "count"
    name_case = False
    for (rn, rv), (cn, cv) in zip(rust, c):
        cv = rust_view(cv)
        if fold:
            rn, cn = rn.lower(), cn.lower()
        if rn != cn:
            if rn.lower() == cn.lower():
                name_case = True
                continue
            return "name"
        if rv != cv:
            return "value-whitespace" if rv.strip() == cv.strip() else "value"
    return "name-case" if name_case else None


FIELDS = (
    "method",
    "target",
    "status",
    "reason",
    "version",
    "headers",
    "trailers",
    "body_len",
    "body_fnv",
    "fin",
    "rsv",
    "opcode",
    "masked",
    "mask_key",
    "payload_len",
    "payload_fnv",
    "consumed",
)


def accepted_difference(rust, c, fold_header_case):
    """The signature of the first field two accepted messages differ in (a
    field a message kind does not have is absent from both and compares
    equal)."""
    found = None
    for field in FIELDS:
        rv, cv = rust.get(field), c.get(field)
        if field in ("headers", "trailers"):
            detail = header_diff(rv or [], cv or [], fold_header_case)
            if detail is not None:
                found = "both-accept:%s:%s" % (field, detail)
        elif rv != cv:
            found = "both-accept:%s" % field
        if found is not None:
            break
    return found


def rejected_difference(rust, c):
    """The signature of two rejections that differ where it is observable: the
    close code a WebSocket receiver fails the connection with goes on the wire
    (an HTTP rejection carries none, and a frame cut short has none)."""
    rc, cc = rust.get("close_code"), c.get("close_code")
    if rc is not None and cc is not None and rc != cc:
        return "both-reject:close-code:%s/%s" % (rc, cc)
    return None


def classify_pair(rust, c, fold_header_case=False):
    """None when the two outcomes agree, else the divergence's signature."""
    rust_accepts, c_accepts = is_accept(rust), is_accept(c)
    if rust.get("kind") == "panic":
        found = "rust-panic"
    elif rust.get("kind") == "crash":
        found = "rust-crash"
    elif c.get("kind") == "crash":
        found = "c-crash"
    elif rust_accepts != c_accepts:
        found = "%s/%s-rejects:%s" % (
            "rust-accepts" if rust_accepts else "c-accepts",
            "c" if rust_accepts else "rust",
            reject_reason(c if rust_accepts else rust),
        )
    elif rust_accepts:
        found = accepted_difference(rust, c, fold_header_case)
    else:
        found = rejected_difference(rust, c)
    return found


# ---------------------------------------------------------------------------
# Minimization: batched delta debugging
# ---------------------------------------------------------------------------


def minimize(data, test_batch, max_rounds=400):
    """The smallest input (1-minimal up to max_rounds batches) for which
    test_batch says True. test_batch takes a list of candidates and returns
    one bool each, so a round is one launch of each parser."""
    cur = data
    n = 2
    rounds = 0
    while len(cur) >= 2 and rounds < max_rounds:
        rounds += 1
        size = max(1, -(-len(cur) // n))
        spans = [(i, min(i + size, len(cur))) for i in range(0, len(cur), size)]
        cands = [cur[:a] + cur[b:] for a, b in spans]
        hit = next((k for k, ok in enumerate(test_batch(cands)) if ok), None)
        if hit is not None:
            cur = cands[hit]
            n = max(n - 1, 2)
        elif n >= len(cur):
            break
        else:
            n = min(len(cur), n * 2)
    return cur


def simplify(data, test_batch, filler=b"a", max_rounds=8):
    """Replace bytes of `data` with `filler` wherever the test still holds, so
    two minimal inputs that differ only in bytes the divergence does not depend
    on become one. Each round tries every single-byte replacement in one batch,
    applies all that held together if the result still holds, else the first."""
    cur = data
    for _ in range(max_rounds):
        spots = [i for i in range(len(cur)) if cur[i : i + 1] != filler]
        if not spots:
            break
        cands = [cur[:i] + filler + cur[i + 1 :] for i in spots]
        kept = [i for i, ok in zip(spots, test_batch(cands)) if ok]
        if not kept:
            break
        merged = bytearray(cur)
        for i in kept:
            merged[i : i + 1] = filler
        if test_batch([bytes(merged)])[0]:
            cur = bytes(merged)
        else:
            cur = cur[: kept[0]] + filler + cur[kept[0] + 1 :]
    return cur


# ---------------------------------------------------------------------------
# Running the two parsers
# ---------------------------------------------------------------------------


def parse_lines(text):
    """{basename of input: outcome} from JSON lines of {"input", "outcome"}."""
    found = {}
    # Split on "\n" only: str.splitlines() also breaks at U+0085, U+2028 and
    # the like, which a header value printed as Latin-1 may hold, unescaped.
    for line in text.split("\n"):
        if not line.strip():
            continue
        try:
            rec = json.loads(line)
            found[os.path.basename(rec["input"])] = rec["outcome"]
        except (ValueError, KeyError, TypeError) as err:
            raise ValueError("not an outcome line: %.80r" % line) from err
    return found


def launch(argv, env, timeout):
    """(exit status, stdout) of one run; the status is None on a timeout, so
    a parser that hangs on an input is a finding and not a stuck job."""
    try:
        done = subprocess.run(
            argv,
            capture_output=True,
            encoding="utf-8",
            errors="replace",
            env=env,
            check=False,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return None, ""
    return done.returncode, done.stdout


def outcomes_of(stdout):
    try:
        return parse_lines(stdout)
    except ValueError:
        return {}


def run_batches(argv_prefix, files, env=None):
    """Run `argv_prefix + files` in batches; a batch that fails is re-run one
    file at a time so the failing input is named. Returns {basename: outcome}
    with {"kind": "crash"} for a file whose run died, hung or printed nothing."""
    out = {}
    for i in range(0, len(files), BATCH):
        chunk = files[i : i + BATCH]
        status, stdout = launch(argv_prefix + chunk, env, BATCH_TIMEOUT)
        got = outcomes_of(stdout)
        if status == 0 and all(os.path.basename(f) in got for f in chunk):
            out.update(got)
            continue
        for path in chunk:
            status, stdout = launch(argv_prefix + [path], env, ONE_TIMEOUT)
            name = os.path.basename(path)
            single = outcomes_of(stdout)
            if status == 0 and name in single:
                out[name] = single[name]
            else:
                out[name] = {
                    "kind": "crash",
                    "detail": "timeout" if status is None else "exit %d" % status,
                }
    return out


RESPONSE_METHODS = ("GET", "HEAD", "CONNECT")

# The cases one input of a target is: a response is framed by the method of
# the request it answers, a frame is received by a client or by a server (the
# masking rule differs). Each is compared and reduced separately.
TARGET_LABELS = {
    "http-response": RESPONSE_METHODS,
    "ws-frame": ("client", "server"),
}

# The prefix of a family key for a target with labels (`response@GET ...`).
KEY_PREFIX = {"http-response": "response", "ws-frame": "ws-frame"}


def flatten_by_label(outcomes, labels):
    """{name: {LABEL: outcome}} -> {name@LABEL: outcome}. A crash or a hang
    has no labels and is the outcome of every one."""
    flat = {}
    for name, outcome in outcomes.items():
        for label in labels:
            if outcome.get("kind") == "crash":
                flat["%s@%s" % (name, label)] = outcome
            else:
                flat["%s@%s" % (name, label)] = outcome.get(
                    label, {"kind": "error", "detail": "no outcome for " + label}
                )
    return flat


def key_target(key):
    """The target a family key belongs to (a key without a prefix is a
    request's)."""
    for target, prefix in KEY_PREFIX.items():
        if key.startswith(prefix + "@"):
            return target
    return "http-request"


class Backend:
    """The two parsers, as functions from file paths to outcome maps. For a
    target with labels (TARGET_LABELS) the maps are keyed name@LABEL."""

    def __init__(self, twin_cmd, c_driver, c_env, target="http-request"):
        self.twin_cmd = twin_cmd
        self.c_driver = c_driver
        self.c_env = c_env
        self.target = target

    def _shaped(self, outcomes):
        if self.target in TARGET_LABELS:
            return flatten_by_label(outcomes, TARGET_LABELS[self.target])
        return outcomes

    def rust(self, files):
        return self._shaped(
            run_batches(self.twin_cmd + ["parse-diff", self.target], files)
        )

    def c(self, files):
        return self._shaped(run_batches([self.c_driver], files, env=self.c_env))


# ---------------------------------------------------------------------------
# One run
# ---------------------------------------------------------------------------


def escaped(data):
    return data.decode("latin-1").encode("unicode_escape").decode("ascii")


def classify(inputs, rust, c, fold):
    """{signature: [name, ...]} and the reject-reason statistics."""
    groups = {}
    stats = {"accepted_both": 0, "rejected_both": 0, "diverged": 0}
    for name in inputs:
        r, k = rust[name], c[name]
        sig = classify_pair(r, k, fold)
        if sig is None:
            stats["accepted_both" if is_accept(r) else "rejected_both"] += 1
        else:
            stats["diverged"] += 1
            groups.setdefault(sig, []).append(name)
    return groups, stats


def case_key(base, method):
    """The key of a file's outcome: the file, and the request method for a
    response (name@METHOD)."""
    return "%s@%s" % (base, method) if method else base


def make_tester(want, backend, workdir, fold, method=None):
    """A test_batch for minimize()/simplify(): True for each candidate whose
    divergence class is still `want` (for a response, under `method`).
    Candidates are written to fixed names under workdir (overwritten each
    round, nothing accumulates)."""
    os.makedirs(workdir, exist_ok=True)

    def test_batch(cands):
        paths = []
        for k, cand in enumerate(cands):
            path = os.path.join(workdir, "cand_%d.bin" % k)
            with open(path, "wb") as fh:
                fh.write(cand)
            paths.append(path)
        rust = backend.rust(paths)
        c = backend.c(paths)
        return [
            classify_pair(
                rust[case_key(os.path.basename(p), method)],
                c[case_key(os.path.basename(p), method)],
                fold,
            )
            == want
            for p in paths
        ]

    return test_batch


def reduce_group(want, members, backend, workdir, fold, limit):
    """{(minimized bytes, method): [count, example name]} for the members of
    one divergence class (a member is named file or file@METHOD): each of up
    to `limit` members (the smallest first) is minimized and then simplified,
    so inputs that differ only in bytes the divergence does not depend on
    collapse into one class of root cause. Returns the classes and the number
    of members left unreduced."""
    testers = {}
    classes = {}
    seen = {}
    for name, data in members[:limit]:
        method = name.partition("@")[2] or None
        key = (data, method)
        if key not in seen:
            if method not in testers:
                testers[method] = make_tester(want, backend, workdir, fold, method)
            # A frame is binary: a NUL fills a byte whose value does not
            # matter, where text is filled with "a".
            filler = b"\x00" if getattr(backend, "target", "") == "ws-frame" else b"a"
            seen[key] = simplify(minimize(data, testers[method]), testers[method], filler)
        entry = classes.setdefault((seen[key], method), [0, name])
        entry[0] += 1
    return classes, max(0, len(members) - limit)


def target_form(method, target):
    """The unusual form of a request target, or None for the ordinary ones."""
    found = None
    if method == b"CONNECT":
        if target.endswith(b":"):
            found = "authority-empty-port"
    elif target.startswith(b"//"):
        found = "double-slash"
    elif not target.startswith(b"/") and b"://" in target:
        rest = target.split(b"://", 1)[1]
        ends = [i for i in (rest.find(ch) for ch in (b"/", b"?", b"#")) if i >= 0]
        if (min(ends) if ends else len(rest)) == 0:
            found = "absolute-empty-authority"
    return found


def request_shape(data):
    """The feature of a (minimized) request that stays after reduction: empty
    lines before the request line, and an unusual target form. It splits a
    signature whose reject reason is coarse (one `BadRequestLine` covers
    several root causes) into the families a bead is recorded for."""
    tags = []
    body = data
    if body[:2] == b"\r\n" or body[:1] == b"\n":
        tags.append("leading-empty-line")
        while body[:2] == b"\r\n" or body[:1] == b"\n":
            body = body[2:] if body[:2] == b"\r\n" else body[1:]
    tokens = body.split(b"\n", 1)[0].rstrip(b"\r").split(b" ")
    form = target_form(tokens[0], tokens[1]) if len(tokens) == 3 else None
    if form:
        tags.append("target=" + form)
    return ",".join(tags)


def response_shape(data):
    """The feature of a (minimized) response that stays after reduction:
    empty lines before the status line, a status that is not three digits or
    that carries no body by itself (1xx, 204, 304), a missing reason phrase."""
    tags = []
    body = data
    if body[:2] == b"\r\n" or body[:1] == b"\n":
        tags.append("leading-empty-line")
        while body[:2] == b"\r\n" or body[:1] == b"\n":
            body = body[2:] if body[:2] == b"\r\n" else body[1:]
    tokens = body.split(b"\n", 1)[0].rstrip(b"\r").split(b" ", 2)
    if len(tokens) >= 2:
        status = tokens[1]
        if len(status) != 3 or not status.isdigit():
            tags.append("status=malformed")
        elif status[:1] == b"1":
            tags.append("status=1xx")
        elif status in (b"204", b"304"):
            tags.append("status=" + status.decode("ascii"))
        if len(tokens) == 2:
            tags.append("no-reason")
    return ",".join(tags)


def ws_shape(data):
    """The feature of a (minimized) frame that stays after reduction, read
    from its first two bytes: a set reserved bit, a reserved or a control
    opcode, FIN clear, and the length form (7-bit lengths are the plain
    case). It splits a rejection whose reason both parsers name coarsely into
    the root causes a bead is recorded for."""
    if len(data) < 2:
        return "short"
    tags = []
    first, second = data[0], data[1]
    opcode = first & 0x0F
    if first & 0x70:
        tags.append("rsv")
    if opcode in (3, 4, 5, 6, 7) or opcode >= 0x0B:
        tags.append("opcode=reserved")
    elif opcode >= 8:
        tags.append("control")
        if not first & 0x80:
            tags.append("fin=0")
    if second & 0x7F == 126:
        tags.append("len=16bit")
    elif second & 0x7F == 127:
        tags.append("len=64bit")
    return ",".join(tags)


def shape_of(target, data):
    if target == "http-response":
        return response_shape(data)
    if target == "ws-frame":
        return ws_shape(data)
    return request_shape(data)


def family_key(sig, shape, label=None, target="http-response"):
    """`<signature> [<shape>]`, prefixed `response@GET ` or `ws-frame@client `
    for a target whose input is several cases."""
    key = "%s [%s]" % (sig, shape) if shape else sig
    return "%s@%s %s" % (KEY_PREFIX[target], label, key) if label else key


def build_rows(groups, inputs_dir, backend, workdir, fold, limit, target="http-request"):
    """One row per divergence family (signature plus the message shape that
    remains after reduction, plus the request method for a response): its
    input count and its classes, each a minimized input with how many inputs
    reduced to it, one example, and both parsers' outcomes on the minimized
    input. With limit 0 nothing is reduced and the first member's own bytes
    are its class."""
    os.makedirs(workdir, exist_ok=True)
    rows = []
    for sig in sorted(groups):
        members = []
        order = sorted(
            groups[sig],
            key=lambda n: (os.path.getsize(os.path.join(inputs_dir, n.partition("@")[0])), n),
        )
        for name in order:
            with open(os.path.join(inputs_dir, name.partition("@")[0]), "rb") as fh:
                members.append((name, fh.read()))
        if limit > 0:
            classes, unreduced = reduce_group(sig, members, backend, workdir, fold, limit)
        else:
            first_method = members[0][0].partition("@")[2] or None
            classes = {(members[0][1], first_method): [1, members[0][0]]}
            unreduced = len(members) - 1
        by_shape = {}
        for (small, method), entry in classes.items():
            by_shape.setdefault((shape_of(target, small), method), {})[small] = entry
        by_name = dict(members)
        for n, (shape, method) in enumerate(sorted(by_shape, key=lambda k: (k[0], k[1] or ""))):
            keyed = sorted(
                by_shape[(shape, method)].items(), key=lambda kv: (-kv[1][0], len(kv[0]), kv[0])
            )
            paths = []
            for k, (small, _) in enumerate(keyed):
                path = os.path.join(workdir, "class_%d.bin" % k)
                with open(path, "wb") as fh:
                    fh.write(small)
                paths.append(path)
            rust, c = backend.rust(paths), backend.c(paths)
            out_classes = []
            for k, (small, (count, example)) in enumerate(keyed):
                base = case_key("class_%d.bin" % k, method)
                out_classes.append(
                    {
                        "minimized": escaped(small),
                        "minimized_hex": small.hex(),
                        "count": count,
                        "example": "%s (%d bytes) %s"
                        % (example, len(by_name[example]), escaped(by_name[example])),
                        "rust": rust[base],
                        "c": c[base],
                    }
                )
            rows.append(
                {
                    "signature": sig,
                    "shape": shape,
                    "method": method,
                    "key": family_key(sig, shape, method, target),
                    "count": sum(cls["count"] for cls in out_classes)
                    + (unreduced if n == 0 else 0),
                    "unreduced": unreduced if n == 0 else 0,
                    "classes": out_classes,
                }
            )
    return rows


def load_known(path):
    known = {}
    if path is None or not os.path.exists(path):
        return known
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = [p.strip() for p in line.split("|")]
            known[parts[0]] = parts[1:] if len(parts) > 1 else []
    return known


def render(header, rows, stats, known):
    lines = list(header)
    lines.append(
        "cases: %d  both-accept-equal/reject: %d/%d  diverged: %d  families: %d"
        % (
            stats["total"],
            stats["accepted_both"],
            stats["rejected_both"],
            stats["diverged"],
            len(rows),
        )
    )
    for row in rows:
        note = known.get(row["key"])
        tag = "known %s" % (note[0] if note else "") if row["key"] in known else "NEW"
        lines.append("")
        lines.append(
            "[%s] %s  (%d input(s), %d class(es)%s)"
            % (
                tag,
                row["key"],
                row["count"],
                len(row["classes"]),
                ", %d not reduced" % row["unreduced"] if row["unreduced"] else "",
            )
        )
        for cls in row["classes"]:
            lines.append("  - %d input(s) reduce to: %s" % (cls["count"], cls["minimized"]))
            lines.append("      example : %s" % cls["example"])
            lines.append("      rust    : %s" % json.dumps(cls["rust"], sort_keys=True))
            lines.append("      c       : %s" % json.dumps(cls["c"], sort_keys=True))
    return "\n".join(lines) + "\n"


def beads_script(rows, known, seed, count, target="http-request"):
    out = ["#!/usr/bin/env bash", "# One bead per divergence not yet recorded; review, then run."]
    for row in rows:
        if row["key"] in known:
            continue
        title = "parser-diff %s: %s" % (target, row["key"])
        desc = [
            "Differential parser fuzzing (bd-9kll.16.2), seed %d count %d: asupersync "
            "5e60b1c4c and the C parser disagree on %d input(s), %d class(es) after "
            "minimization." % (seed, count, row["count"], len(row["classes"]))
        ]
        for cls in row["classes"]:
            desc.append(
                "Minimized (%d input(s)): %s\n  Rust: %s\n  C: %s"
                % (
                    cls["count"],
                    cls["minimized"],
                    json.dumps(cls["rust"], sort_keys=True),
                    json.dumps(cls["c"], sort_keys=True),
                )
            )
        out.append(
            "br create --title=%s --type=bug --priority=2 --description=%s"
            % (shlex.quote(title), shlex.quote("\n".join(desc)))
        )
    return "\n".join(out) + "\n"


def do_run(args):
    out = args.out
    os.makedirs(out, exist_ok=True)
    twin_cmd = shlex.split(args.twin_run)
    twin_exe = shutil.which(twin_cmd[0]) if twin_cmd else None
    c_exe = shutil.which(args.c_driver)
    if twin_exe is None or c_exe is None:
        sys.stderr.write(
            "parser_differential: not executable: %s\n"
            % (twin_cmd[0] if twin_exe is None and twin_cmd else args.c_driver)
        )
        return 2
    twin_cmd = [twin_exe] + twin_cmd[1:]
    args.c_driver = c_exe
    env = dict(os.environ)
    if args.c_require_host is not None:
        env["ASX_DIFF_REQUIRE_HOST"] = args.c_require_host
    target = args.target
    backend = Backend(twin_cmd, args.c_driver, env, target)

    status, _ = launch(
        twin_cmd
        + ["parse-fuzz", target, "--seed", str(args.seed), "--count", str(args.count)]
        + ["--out", out],
        None,
        BATCH_TIMEOUT,
    )
    if status != 0:
        sys.stderr.write("parser_differential: twin_run parse-fuzz failed (status %s)\n" % status)
        return 2
    inputs_dir = os.path.join(out, "inputs")
    names = sorted(os.listdir(inputs_dir))
    if not names:
        sys.stderr.write("parser_differential: the corpus is empty\n")
        return 2
    paths = [os.path.join(inputs_dir, n) for n in names]
    with open(os.path.join(out, "rust.jsonl"), encoding="utf-8") as fh:
        rust = parse_lines(fh.read())
    if len(rust) != len(names) or any(n not in rust for n in names):
        sys.stderr.write("parser_differential: the Rust outcomes do not cover the corpus\n")
        return 2
    c = backend.c(paths)
    if target in TARGET_LABELS:
        rust = flatten_by_label(rust, TARGET_LABELS[target])
        cases = sorted(rust)
    else:
        cases = names
    fold = args.fold_header_case
    groups, stats = classify(cases, rust, c, fold)
    stats["total"] = len(cases)

    rows = build_rows(groups, inputs_dir, backend, os.path.join(out, "min"), fold,
                      0 if args.no_minimize else args.max_minimize, target)

    known = load_known(args.known)
    header = [
        "parser differential: %s, seed %d, count %d" % (target, args.seed, args.count),
        "rust: asupersync 5e60b1c4c (twin_run parse-diff); c: %s" % args.c_driver,
        "assumptions: fold-header-case=%s c-require-host=%s"
        % (fold, "default" if args.c_require_host is None else args.c_require_host),
    ]
    text = render(header, rows, stats, known)
    with open(os.path.join(out, "report.txt"), "w", encoding="utf-8") as fh:
        fh.write(text)
    with open(os.path.join(out, "report.json"), "w", encoding="utf-8") as fh:
        json.dump(
            {
                "seed": args.seed,
                "count": args.count,
                "fold_header_case": fold,
                "c_require_host": args.c_require_host,
                "stats": stats,
                "signatures": rows,
            },
            fh,
            indent=1,
            sort_keys=True,
        )
    with open(os.path.join(out, "beads.sh"), "w", encoding="utf-8") as fh:
        fh.write(beads_script(rows, known, args.seed, args.count, target))
    sys.stdout.write(text)

    seen = [r["key"] for r in rows]
    new = [key for key in seen if key not in known]
    # A key of a target with labels is `response@<METHOD> ...` or
    # `ws-frame@<role> ...`; a run reports as stale only the keys of its own
    # target, as the other targets' keys are not its to see.
    stale = [key for key in known if key not in seen and key_target(key) == target]
    for key in stale:
        sys.stdout.write(
            "known, not observed in this run (fixed, or seen only in another "
            "configuration or seed): %s\n" % key
        )
    if args.mode == "gate" and new:
        sys.stdout.write("parser_differential: FAIL, %d divergence(s) not recorded\n" % len(new))
        return 1
    sys.stdout.write(
        "parser_differential: %s, %d signature(s), %d not recorded\n"
        % (args.mode, len(rows), len(new))
    )
    return 0


# ---------------------------------------------------------------------------
# Self-test: synthetic parsers, no binary
# ---------------------------------------------------------------------------


class FakeBackend:
    """Pretend parsers over bytes. Rust accepts anything starting with 'G'.
    C accepts the same unless the input holds 'XY' (rejects with 'xy'),
    and reports method 'GET' in lower case when the input holds 'Z'."""

    @staticmethod
    def _rust(data):
        if not data:
            return {"kind": "empty"}
        if data[:1] != b"G":
            return {"kind": "error", "detail": "BadStart"}
        return {
            "kind": "complete",
            "method": "GET",
            "target": "/",
            "version": "HTTP/1.1",
            "headers": [["Host", "a"]],
            "trailers": [],
            "body_len": 0,
            "body_fnv": "0",
            "consumed": len(data),
        }

    @staticmethod
    def _c(data):
        out = FakeBackend._rust(data)
        if out["kind"] != "complete":
            return {"kind": "error", "detail": "bad-start-line"}
        if b"XY" in data:
            return {"kind": "error", "detail": "xy"}
        if b"Z" in data:
            out["method"] = "get"
        return out

    @staticmethod
    def _read(path):
        with open(path, "rb") as fh:
            return fh.read()

    def rust(self, files):
        return {os.path.basename(f): self._rust(self._read(f)) for f in files}

    def c(self, files):
        return {os.path.basename(f): self._c(self._read(f)) for f in files}


class FakeResponseBackend:
    """Pretend response parsers. Rust accepts anything starting with 'H' for
    every request method; C does too, except that it rejects an input holding
    'B' when the method is HEAD (a body where HEAD has none)."""

    @staticmethod
    def _outcome(data, c_side, method):
        if not data:
            return {"kind": "empty"}
        if data[:1] != b"H":
            return {"kind": "error", "detail": "BadStatus"}
        if c_side and method == "HEAD" and b"B" in data:
            return {"kind": "error", "detail": "head-body"}
        return {
            "kind": "complete",
            "status": 200,
            "reason": "OK",
            "version": "HTTP/1.1",
            "headers": [],
            "trailers": [],
            "body_len": 0,
            "body_fnv": "0",
            "consumed": len(data),
        }

    def _side(self, files, c_side):
        out = {}
        for path in files:
            with open(path, "rb") as fh:
                data = fh.read()
            out[os.path.basename(path)] = {
                m: self._outcome(data, c_side, m) for m in RESPONSE_METHODS
            }
        return flatten_by_label(out, RESPONSE_METHODS)

    def rust(self, files):
        return self._side(files, False)

    def c(self, files):
        return self._side(files, True)


def selftest():
    failures = []

    def check(cond, what):
        if not cond:
            failures.append(what)

    ok = {"kind": "complete", "method": "GET", "target": "/", "version": "HTTP/1.1",
          "headers": [["Host", "a"]], "trailers": [], "body_len": 0, "body_fnv": "0", "consumed": 1}
    check(classify_pair(ok, dict(ok)) is None, "equal outcomes agree")
    check(classify_pair({"kind": "error", "detail": "A"}, {"kind": "error", "detail": "b"}) is None,
          "two rejections are not a divergence")
    check(classify_pair({"kind": "empty"}, {"kind": "empty"}) is None, "two empties agree")
    check(classify_pair(ok, {"kind": "error", "detail": "missing-host"})
          == "rust-accepts/c-rejects:missing-host", "accept/reject names the rejecter's reason")
    check(classify_pair({"kind": "error", "detail": "InvalidHeader"}, ok)
          == "c-accepts/rust-rejects:InvalidHeader", "reject/accept names the rejecter's reason")
    check(classify_pair({"kind": "panic"}, ok) == "rust-panic", "a panic is its own signature")
    check(classify_pair(ok, {"kind": "crash", "detail": "exit -11"}) == "c-crash", "a crash too")
    other = dict(ok, method="get")
    check(classify_pair(ok, other) == "both-accept:method", "method differs")
    other = dict(ok, headers=[["host", "a"]])
    check(classify_pair(ok, other) == "both-accept:headers:name-case", "header name case")
    check(classify_pair(ok, other, fold_header_case=True) is None, "folded header case agrees")
    other = dict(ok, headers=[["Host", "a "]])
    check(classify_pair(ok, other) == "both-accept:headers:value-whitespace", "header value spacing")
    other = dict(ok, headers=[["Host", "a"], ["X", "1"]])
    check(classify_pair(ok, other) == "both-accept:headers:count", "header count")
    # The Rust value is its decoded string; the C side prints wire bytes as
    # Latin-1 code points, so UTF-8 C3 A9 and the lone byte E9 both mean U+00E9.
    rust_e = dict(ok, headers=[["X", "é"]])
    check(classify_pair(rust_e, dict(ok, headers=[["X", "Ã©"]])) is None,
          "a UTF-8 value is the decoded string")
    check(classify_pair(rust_e, dict(ok, headers=[["X", "é"]])) is None,
          "a non-UTF-8 value is decoded as Latin-1")
    check(classify_pair(rust_e, dict(ok, headers=[["X", "e"]])) == "both-accept:headers:value",
          "a different value still differs")
    check(rust_view("Ã") == "Ã", "a lone UTF-8 lead byte is decoded as Latin-1")
    other = dict(ok, body_len=1)
    check(classify_pair(ok, other) == "both-accept:body_len", "body length")
    other = dict(ok, consumed=2)
    check(classify_pair(ok, other) == "both-accept:consumed", "consumed bytes")

    # ddmin: the property is "contains XY"; the minimum is exactly that.
    calls = []

    def has_xy(cands):
        calls.append(len(cands))
        return [b"XY" in cand for cand in cands]

    small = minimize(b"GET /aaaaXbbbYXYcccc HTTP/1.1\r\n\r\n", has_xy)
    check(small == b"XY", "ddmin keeps exactly the property's bytes, got %r" % small)
    check(minimize(b"X", has_xy) == b"X", "a one-byte input is returned as is")

    # A whole run over the fake parsers through the real grouping and
    # minimization code.
    import tempfile

    with tempfile.TemporaryDirectory() as tmp:
        names = {
            "00000.bin": b"GET / HTTP/1.1\r\n\r\n",
            "00001.bin": b"GET /aXYb HTTP/1.1\r\n\r\n",
            "00002.bin": b"GET /Z HTTP/1.1\r\n\r\n",
            "00003.bin": b"POST / HTTP/1.1\r\n\r\n",
            "00004.bin": b"GET /XYXY HTTP/1.1\r\n\r\n",
        }
        paths = []
        for name, data in names.items():
            path = os.path.join(tmp, name)
            with open(path, "wb") as fh:
                fh.write(data)
            paths.append(path)
        backend = FakeBackend()
        rust, c = backend.rust(paths), backend.c(paths)
        groups, stats = classify(sorted(names), rust, c, False)
        check(sorted(groups) == ["both-accept:method", "rust-accepts/c-rejects:xy"],
              "grouping found %r" % sorted(groups))
        check(groups.get("rust-accepts/c-rejects:xy") == ["00001.bin", "00004.bin"],
              "the xy group holds both inputs")
        check(stats["diverged"] == 3 and stats["rejected_both"] == 1
              and stats["accepted_both"] == 1, "statistics %r" % stats)
        members = [("00001.bin", names["00001.bin"]), ("00004.bin", names["00004.bin"])]
        classes, unreduced = reduce_group("rust-accepts/c-rejects:xy", members, backend,
                                          os.path.join(tmp, "min"), False, 10)
        check(classes == {(b"GXY", None): [2, "00001.bin"]} and unreduced == 0,
              "both xy inputs reduce to one class that keeps the G the Rust side needs, got %r"
              % classes)
        classes, unreduced = reduce_group("rust-accepts/c-rejects:xy", members, backend,
                                          os.path.join(tmp, "min"), False, 1)
        check(classes == {(b"GXY", None): [1, "00001.bin"]} and unreduced == 1,
              "the limit leaves the rest unreduced, got %r %r" % (classes, unreduced))
        classes, _ = reduce_group("both-accept:method", [("00002.bin", names["00002.bin"])],
                                  backend, os.path.join(tmp, "min"), False, 10)
        check(classes == {(b"GZ", None): [1, "00002.bin"]},
              "the method divergence reduces to GZ, got %r" % classes)
        rows = build_rows(groups, tmp, backend, os.path.join(tmp, "min"), False, 10)
        check([r["signature"] for r in rows] == sorted(groups)
              and rows[1]["count"] == 2 and rows[1]["classes"][0]["count"] == 2
              and rows[1]["classes"][0]["minimized"] == "GXY"
              and rows[1]["classes"][0]["rust"]["kind"] == "complete"
              and rows[1]["classes"][0]["c"]["detail"] == "xy",
              "rows carry each class with both outcomes, got %r" % rows)

    # simplify keeps the bytes the property needs and fills the rest.
    needs_gx = lambda cands: [c[:1] == b"G" and b"X" in c for c in cands]  # noqa: E731
    check(simplify(b"GbXcd", needs_gx) == b"GaXaa", "simplify fills what the test does not need")
    check(simplify(b"aaaa", needs_gx) == b"aaaa", "an input already of filler is left alone")

    # A response is a case per request method, compared and reduced apart.
    flat = flatten_by_label({"a.bin": {"GET": {"kind": "empty"}, "HEAD": {"kind": "x"}}},
                            RESPONSE_METHODS)
    check(flat["a.bin@GET"] == {"kind": "empty"} and flat["a.bin@HEAD"] == {"kind": "x"}
          and flat["a.bin@CONNECT"]["detail"] == "no outcome for CONNECT",
          "flatten_by_label names a case per method, got %r" % sorted(flat))
    crash = flatten_by_label({"b.bin": {"kind": "crash", "detail": "exit -11"}},
                             RESPONSE_METHODS)
    check(all(v["kind"] == "crash" for v in crash.values()) and len(crash) == 3,
          "a crash is the outcome of every method")
    check(case_key("a.bin", None) == "a.bin" and case_key("a.bin", "HEAD") == "a.bin@HEAD",
          "case_key")
    check(family_key("sig", "", "HEAD") == "response@HEAD sig"
          and family_key("sig", "status=1xx", "GET") == "response@GET sig [status=1xx]"
          and family_key("sig", "x") == "sig [x]"
          and family_key("sig", "rsv", "client", "ws-frame") == "ws-frame@client sig [rsv]",
          "family_key with a label")
    check(key_target("response@GET sig") == "http-response"
          and key_target("ws-frame@server sig [rsv]") == "ws-frame"
          and key_target("sig [leading-empty-line]") == "http-request",
          "key_target names the target of a family key")

    # A frame is a case per receiver role; two rejections that differ in the
    # close code are a divergence, a cut-short frame (no code) is not.
    frame = {"kind": "complete", "fin": True, "rsv": 0, "opcode": 1, "masked": False,
             "mask_key": "", "payload_len": 0, "payload_fnv": "0", "consumed": 2}
    for field, other in (("fin", False), ("rsv", 4), ("opcode", 2), ("masked", True),
                         ("mask_key", "00000000"), ("payload_len", 1), ("payload_fnv", "1")):
        check(classify_pair(frame, dict(frame, **{field: other})) == "both-accept:" + field,
              "a frame that differs in %s is a both-accept divergence" % field)
    check(classify_pair({"kind": "error", "detail": "ControlFrameTooLarge", "close_code": 1009},
                        {"kind": "error", "detail": "header-check", "close_code": 1002})
          == "both-reject:close-code:1009/1002", "different close codes diverge")
    check(classify_pair({"kind": "error", "detail": "x", "close_code": 1002},
                        {"kind": "error", "detail": "y", "close_code": 1002}) is None
          and classify_pair({"kind": "error", "detail": "incomplete"},
                            {"kind": "error", "detail": "header-check", "close_code": 1002})
          is None, "equal codes, or no code on one side, are not a divergence")
    ws_shapes = {
        b"\x81\x05": "",
        b"\xc1\x00": "rsv",
        b"\x83\x00": "opcode=reserved",
        b"\x8b\x00": "opcode=reserved",
        b"\x09\x00": "control,fin=0",
        b"\x89\x7e": "control,len=16bit",
        b"\x82\xff": "len=64bit",
        b"\x81": "short",
    }
    for raw, want in ws_shapes.items():
        check(ws_shape(raw) == want, "ws_shape(%r) = %r" % (raw, ws_shape(raw)))
    with tempfile.TemporaryDirectory() as tmp:
        rb = FakeResponseBackend()
        names = {"00000.bin": b"HTTP/1.1 200 OK", "00001.bin": b"HxxBxx", "00002.bin": b"junk"}
        for name, data in names.items():
            with open(os.path.join(tmp, name), "wb") as fh:
                fh.write(data)
        files = [os.path.join(tmp, n) for n in sorted(names)]
        rust, c = rb.rust(files), rb.c(files)
        cases = sorted(rust)
        groups, stats = classify(cases, rust, c, False)
        check(sorted(groups) == ["rust-accepts/c-rejects:head-body"]
              and groups["rust-accepts/c-rejects:head-body"] == ["00001.bin@HEAD"],
              "only the HEAD case of the input with a body diverges, got %r" % groups)
        check(stats["total"] if "total" in stats else True, "stats")
        rows = build_rows(groups, tmp, rb, os.path.join(tmp, "min"), False, 10,
                          "http-response")
        check(len(rows) == 1 and rows[0]["key"] == "response@HEAD rust-accepts/c-rejects:head-body"
              and rows[0]["classes"][0]["minimized"] == "HB"
              and rows[0]["classes"][0]["c"]["detail"] == "head-body",
              "the response row is keyed by method and reduced to HB, got %r" % rows)

    # Two accepted responses that differ in the status or the reason.
    ok = FakeResponseBackend._outcome(b"H", False, "GET")
    check(classify_pair(ok, dict(ok, status=201)) == "both-accept:status",
          "a different status is a both-accept divergence")
    check(classify_pair(ok, dict(ok, reason="Okay")) == "both-accept:reason",
          "a different reason is a both-accept divergence")
    check(classify_pair(ok, dict(ok)) is None, "equal responses agree")

    shapes = {
        b"HTTP/1.1 200 OK\r\n\r\n": "",
        b"\r\nHTTP/1.1 200 OK\r\n\r\n": "leading-empty-line",
        b"HTTP/1.1 200\r\n\r\n": "no-reason",
        b"HTTP/1.1 20 OK\r\n\r\n": "status=malformed",
        b"HTTP/1.1 +200 OK\r\n\r\n": "status=malformed",
        b"HTTP/1.1 100 Continue\r\n\r\n": "status=1xx",
        b"HTTP/1.1 204 No Content\r\n\r\n": "status=204",
        b"HTTP/1.1 304 \r\n\r\n": "status=304",
    }
    for raw, want in shapes.items():
        check(response_shape(raw) == want,
              "response_shape(%r) = %r" % (raw, response_shape(raw)))
    check(shape_of("http-response", b"HTTP/1.1 200\r\n\r\n") == "no-reason"
          and shape_of("http-request", b"GET // HTTP/1.1\r\n\r\n") == "target=double-slash",
          "shape_of dispatches on the target")

    # The request shape that splits one coarse reject reason into families.
    shapes = {
        b"\r\nGET / HTTP/1.1\r\n\r\n": "leading-empty-line",
        b"\nGET // HTTP/1.1\r\n\r\n": "leading-empty-line,target=double-slash",
        b"GET // HTTP/1.1\r\n\r\n": "target=double-slash",
        b"GET a:/// HTTP/1.0\r\n\r\n": "target=absolute-empty-authority",
        b"CONNECT a: HTTP/1.1\r\n\r\n": "target=authority-empty-port",
        b"GET http://h/p?q HTTP/1.1\r\n\r\n": "",
        b"CONNECT a:443 HTTP/1.1\r\n\r\n": "",
        b"GXY": "",
        b"": "",
    }
    for raw, want in shapes.items():
        check(request_shape(raw) == want, "request_shape(%r) = %r" % (raw, request_shape(raw)))

    # Outcome lines hold code points that str.splitlines() would split at.
    odd = '{"input":"a.bin","outcome":{"kind":"error","detail":"x\u0085y z\\u000bw"}}\n'
    got = parse_lines(odd + odd.replace("a.bin", "b.bin"))
    check(sorted(got) == ["a.bin", "b.bin"] and got["a.bin"]["detail"] == "x\u0085y z\x0bw",
          "outcome lines are split at newlines only, got %r" % sorted(got))

    known_file = None
    with tempfile.TemporaryDirectory() as tmp:
        known_file = os.path.join(tmp, "known.txt")
        with open(known_file, "w", encoding="utf-8") as fh:
            fh.write("# comment\n\nboth-accept:method | bd-1 | c lowercases\n")
        known = load_known(known_file)
        check(list(known) == ["both-accept:method"] and known["both-accept:method"][0] == "bd-1",
              "known list parses")
    check(load_known("/nonexistent/known.txt") == {}, "a missing known list is empty")

    if failures:
        for what in failures:
            sys.stderr.write("selftest FAIL: %s\n" % what)
        return 1
    sys.stdout.write("parser_differential selftest: PASS\n")
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--selftest", action="store_true")
    sub = ap.add_subparsers(dest="cmd")
    run = sub.add_parser("run", help="run one corpus through both parsers and compare")
    run.add_argument("--twin-run", required=True, help="twin_run command (binary or cargo run)")
    run.add_argument("--c-driver", required=True, help="the ASX_DIFF_* binary of --target")
    run.add_argument("--target", choices=("http-request", "http-response", "ws-frame"),
                     default="http-request",
                     help="the parser to compare (a response is a case per request method, "
                     "a frame a case per receiver role)")
    run.add_argument("--out", required=True)
    run.add_argument("--seed", type=int, default=1)
    run.add_argument("--count", type=int, default=2000)
    run.add_argument("--mode", choices=("observe", "gate"), default="observe")
    run.add_argument("--known", default=None)
    run.add_argument("--fold-header-case", action="store_true")
    run.add_argument("--c-require-host", choices=("0", "1"), default=None)
    run.add_argument("--no-minimize", action="store_true")
    run.add_argument("--max-minimize", type=int, default=150,
                     help="inputs reduced per signature (the smallest first)")
    args = ap.parse_args(argv)
    if args.selftest:
        return selftest()
    if args.cmd == "run":
        return do_run(args)
    ap.print_usage(sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
