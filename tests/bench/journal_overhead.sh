#!/usr/bin/env bash
# journal_overhead.sh — RP1's overhead budget: an HTTP/1.1 keep-alive server
# with the boundary journal off and on (bd-9kll.11.1)
#
# Builds a live POSIX library (release, -O2) in its own build directory and
# tests/bench/bench_journal_http.c against it, then runs the bench: the same
# keep-alive workload in every mode, interleaved, N rounds, reporting
# throughput, journal records and bytes per request, and the throughput loss
# against no journal. The RP1 budget is at most 10% (mode "null": recording
# to a sink that does no I/O); "file" adds fsync per commit and is
# informational.
#
#   make bench-journal-overhead
#   make bench-journal-overhead BENCH_JOURNAL_ARGS="--requests 50000 --rounds 9"
#   make bench-journal-overhead BENCH_JOURNAL_ARGS="--modes off,null,file --gate"
#
# The arguments are bench_journal_http's: --requests N, --rounds R, --modes
# off,null,file, --budget-pct P, --gate (exit 1 over budget), --dir DIR (the
# "file" mode's scratch directory, default /tmp; its file is unlinked once
# opened), --timeout-s S (a run without progress ends after S seconds plus
# N/2000, default 60, and the bench exits 1 instead of waiting). On a worker:
# rch exec -- make bench-journal-overhead.
#
# Exit: the bench's (0; 1 with --gate over budget or on a failure; 2 when
# the build has no native sockets).
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="${BENCH_JOURNAL_BUILD_DIR:-build/bench-journal}"
BIN="${ROOT}/${BUILD}/bench_journal_http"

mkdir -p "${ROOT}/${BUILD}"

echo "[asx] bench-journal-overhead: building the live POSIX library (${BUILD})..." >&2
"${MAKE:-make}" -C "$ROOT" build PROFILE=POSIX DETERMINISTIC=0 BUILD_TYPE=release \
    BUILD_DIR="$BUILD" LDFLAGS="-lpthread" >"${ROOT}/${BUILD}/lib.log" 2>&1 || {
    echo "[asx] bench-journal-overhead: the library build failed; see ${BUILD}/lib.log" >&2
    tail -20 "${ROOT}/${BUILD}/lib.log" >&2
    exit 1
}

# The bench gets exactly the library's flags (struct layouts must match).
LIB_CFLAGS="$(sed -n 's/^ALL_CFLAGS=//p' "${ROOT}/${BUILD}/.build-config")"
# shellcheck disable=SC2086
${CC:-cc} $LIB_CFLAGS -Wno-unused-result -I"${ROOT}/include" \
    "${ROOT}/tests/bench/bench_journal_http.c" \
    "${ROOT}/${BUILD}/lib/libasx.a" -lpthread -o "$BIN"

echo "[asx] bench-journal-overhead: running..." >&2
# BENCH_JOURNAL_ARGS is deliberately word-split into arguments.
# shellcheck disable=SC2086
exec "$BIN" ${BENCH_JOURNAL_ARGS:-}
