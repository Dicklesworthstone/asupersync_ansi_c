#!/bin/sh
# compare_replay.sh -- the bare-metal replay against the host's
# (make test-baremetal-qemu, bd-9kll.15.7).
#
# NATIVE.txt is `asx-conformance replay <fixtures>` on the host; QEMU.txt is
# what the proof image printed through semihosting for the same fixtures in
# the same order. Each fixture is one line, "<id> <semantic digest>
# <projection digest>" (or "ERROR <id>: ...", or "SKIP <id>: ..." where the
# target's profile forbids what the scenario requires, DEC-008), and the
# last line counts them. The harness's own lines start with "bm:" and are
# not compared; a fixture the target skipped is removed from the host side.
#
# Usage: compare_replay.sh NATIVE.txt QEMU.txt [--expect-differ ID|FILE]
#
#   without --expect-differ: the two must be identical, and must hold at
#     least one digest line (an empty output proves nothing); exit 0 or 1.
#   with --expect-differ ID: the planted control. The two must differ on
#     the line of fixture ID and on no other; exit 0 when so, else 1.
#   with --expect-differ FILE: the same for every id of FILE (one per line,
#     '#' comments): resource class R1's list of fixtures it does not
#     reproduce (tests/baremetal/exceeds_r1.txt). An id of the list that is
#     identical is a stale entry and fails.
#
# SPDX-License-Identifier: MIT

set -eu

[ $# -ge 2 ] || { echo "usage: compare_replay.sh NATIVE.txt QEMU.txt [--expect-differ ID|FILE]" >&2; exit 2; }
native=$1
qemu=$2
shift 2
control=
if [ $# -ge 2 ] && [ "$1" = "--expect-differ" ]; then
    control=$2
fi

want="$qemu.expected"
got="$qemu.got"
skipped="$qemu.skipped"

grep -v '^bm:' "$qemu" | tr -d '\r' > "$got"
sed -n 's/^SKIP \([^ :]*\):.*/\1/p' "$got" > "$skipped"
if [ -s "$skipped" ]; then
    # Drop the host's line of each skipped fixture, and the skipped lines.
    awk 'NR==FNR { skip[$1] = 1; next } !($1 in skip)' "$skipped" "$native" > "$want"
    grep -v '^SKIP ' "$got" > "$got.noskip"
    got="$got.noskip"
else
    cp "$native" "$want"
fi

digests=$(grep -c ' sha256:.* sha256:' "$want" || true)
if [ "$digests" -eq 0 ]; then
    echo "compare_replay: FAIL (the host's output holds no digest line: nothing to compare)"
    exit 1
fi

if [ -z "$control" ]; then
    if cmp -s "$want" "$got"; then
        echo "compare_replay: identical ($digests digest line(s), $(grep -c '^ERROR ' "$want" || true) ERROR line(s) on both, $(grep -c . "$skipped" || true) skipped as forbidden in the profile)"
        exit 0
    fi
    echo "compare_replay: FAIL (the target's output differs from the host's; first differences, < host, > target):"
    diff "$want" "$got" | head -20
    exit 1
fi

# --expect-differ: the ids in the set must differ from the host's line and
# every other fixture must be identical. The count line ("replayed N
# fixture(s), F failed") moves with an ERROR and is left out of both sides.
grep -v '^replayed ' "$want" > "$want.lines"
grep -v '^replayed ' "$got" > "$got.lines"
if [ "$(wc -l < "$want.lines")" -ne "$(wc -l < "$got.lines")" ]; then
    echo "compare_replay: FAIL (the line counts differ, $(wc -l < "$want.lines") host, $(wc -l < "$got.lines") target)"
    exit 1
fi
if [ -f "$control" ]; then
    sed -e '/^[[:space:]]*#/d' -e '/^[[:space:]]*$/d' "$control" > "$qemu.differ"
else
    echo "$control" > "$qemu.differ"
fi
# Line i of both files is the same fixture. A line's id is its first word,
# or the second (without the colon) of an "ERROR <id>: ..." line.
awk '
    NR == FNR { must[$1] = 1; next }
    FILENAME == ARGV[2] { w[FNR] = $0; wn = FNR; next }
    { g[FNR] = $0; gn = FNR }
    function id_of(s,   a) {
        split(s, a, " ")
        if (a[1] == "ERROR") { sub(/:$/, "", a[2]); return a[2] }
        return a[1]
    }
    END {
        same_ok = 0; differ_ok = 0; bad = 0
        for (i = 1; i <= wn; i++) {
            id = id_of(w[i])
            if (id in must) {
                if (w[i] != g[i]) differ_ok++
                else { print "  stale: " id " is identical to the host (it must differ)"; bad++ }
                seen[id] = 1
            } else {
                if (w[i] == g[i]) same_ok++
                else { print "  unexpected difference: " id; bad++ }
            }
        }
        for (id in must) if (!(id in seen)) { print "  not in the replay: " id; bad++ }
        printf "same=%d differ=%d bad=%d\n", same_ok, differ_ok, bad
        exit (bad == 0 ? 0 : 1)
    }' "$qemu.differ" "$want.lines" "$got.lines" > "$qemu.differ.report" || {
    echo "compare_replay: FAIL (--expect-differ: the lines are not as required)"
    head -12 "$qemu.differ.report"
    exit 1
}
echo "compare_replay: as expected ($(tail -1 "$qemu.differ.report"): the listed fixture(s) differ from the host, all others are identical)"
exit 0
