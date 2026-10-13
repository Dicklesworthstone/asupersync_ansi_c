#!/usr/bin/env bash
# native_record_replay.sh — RP3: record a real native HTTP server run, replay
# it bit-exactly (bd-9kll.11.3)
#
# Builds a live POSIX library and e2e_native_record_replay, then:
#   1. records the HTTP server (platform hooks wrapped by a journal) while a
#      small deterministic client load runs against it: a keep-alive
#      connection with two requests, a mid-request disconnect, a connection
#      reset in the middle of a request, and a held keep-alive connection;
#   2. sends SIGTERM, so the server drains and exits;
#   3. replays the journal N times (ASX_E2E_REPLAYS, default 100), each a
#      fresh process, and requires the trace digest and the number of
#      requests served to equal the recorded ones every time, with no real
#      socket call made during a replay;
#   4. replays a tampered copy and a copy under another asupersync revision,
#      and requires both to be refused with the seq or the field named.
#
# The JSONL log carries the journal's SHA-256, the recorded and replayed
# digests and, on a refusal, the divergence seq.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
export ASX_E2E_POLICY_ID="${ASX_E2E_POLICY_ID:-NATIVE-RECORD-REPLAY}"
export ASX_E2E_PROFILE="POSIX"
export ASX_E2E_DETERMINISTIC="0"
source "$SCRIPT_DIR/harness.sh"

e2e_init "native-record-replay" "E2E-NATIVE-RECORD-REPLAY"

# The same live POSIX library (and flags) as http_robustness.sh, so a suite
# run builds it once; run_all.sh fails a family that rewrites
# build/lib/libasx.a, so it has its own BUILD_DIR.
LIVE_DIR="build/e2e-http-live"
CAPS="-DASX_HTTP_SERVER_MAX_CONNS=80u -DASX_SERVER_MAX_CONNECTIONS=80u -DASX_MAX_TCP_STREAMS=96u"
CAPS="$CAPS -DASX_NATIVE_MAX_SOCKETS=128u -DASX_MAX_IO_TOKENS=128u -DASX_MAX_WAKERS=128u"
BIN="${E2E_ARTIFACT_DIR}/e2e_native_record_replay"
LOG="${E2E_ARTIFACT_DIR}/native_record_replay"
REPLAYS="${ASX_E2E_REPLAYS:-100}"
JOURNAL="${LOG}.journal"
REC_OUT="${LOG}.record.out"
LOAD_OUT="${LOG}.load.out"

# A replay that hangs (a divergence the scheduler spins on) is failed, not
# waited for: every replay runs under timeout(1) where it exists.
BOUND=""
if command -v timeout >/dev/null 2>&1; then
    BOUND="timeout 120"
elif command -v gtimeout >/dev/null 2>&1; then
    BOUND="gtimeout 120"
fi

finish() {
    set +e
    e2e_finish
    exit $?
}

if ! "${MAKE:-make}" -C "$E2E_PROJECT_ROOT" build PROFILE=POSIX DETERMINISTIC=0 \
    BUILD_DIR="$LIVE_DIR" CFLAGS="$CAPS" LDFLAGS="-lpthread" >"${LOG}.lib.log" 2>&1; then
    e2e_scenario "rr.lib_build" "live POSIX library build failed: ${LOG}.lib.log" "fail"
    finish
fi
e2e_scenario "rr.lib_build" "" "pass"

# The driver gets exactly the library's flags (struct layouts must match).
LIB_CFLAGS="$(sed -n 's/^ALL_CFLAGS=//p' "${E2E_PROJECT_ROOT}/${LIVE_DIR}/.build-config")"
mkdir -p "$(dirname "$BIN")"
# shellcheck disable=SC2086
if ! ${CC:-gcc} $LIB_CFLAGS -I"${E2E_PROJECT_ROOT}/include" \
    "${SCRIPT_DIR}/e2e_native_record_replay.c" \
    "${E2E_PROJECT_ROOT}/${LIVE_DIR}/lib/libasx.a" -lpthread -o "$BIN" 2>"${LOG}.build.log"; then
    e2e_scenario "rr.build" "driver build failed: ${LOG}.build.log" "fail"
    finish
fi
e2e_scenario "rr.build" "" "pass"

sha_of() {
    { sha256sum "$1" 2>/dev/null || shasum -a 256 "$1"; } | cut -d' ' -f1
}

# value_of FILE KEY: the first token after "KEY " on its (last) line.
value_of() { sed -n "s/^$2 \\([^ ]*\\).*/\\1/p" "$1" | tail -1; }

# ---------------------------------------------------------------- record
"$BIN" record "$JOURNAL" >"$REC_OUT" 2>"${REC_OUT}.err" &
SERVER_PID=$!
PORT=""
for _ in $(seq 1 200); do
    PORT="$(sed -n 's/^LISTENING //p' "$REC_OUT")"
    [ -n "$PORT" ] && break
    kill -0 "$SERVER_PID" 2>/dev/null || break
    sleep 0.05
done
if [ -z "$PORT" ]; then
    e2e_scenario "rr.record_server" "server did not start: $(cat "${REC_OUT}.err")" "fail"
    kill -KILL "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    finish
fi

"$BIN" load "$PORT" >"$LOAD_OUT" 2>"${LOAD_OUT}.err" &
LOAD_PID=$!
READY=0
for _ in $(seq 1 200); do
    if grep -q '^READY' "$LOAD_OUT" 2>/dev/null; then
        READY=1
        break
    fi
    kill -0 "$LOAD_PID" 2>/dev/null || break
    sleep 0.05
done

# SIGTERM only once every client but the held one is done: the drain then
# closes exactly that idle connection. A drain that never ends is killed
# after 10 s instead of hanging the suite.
kill -TERM "$SERVER_PID" 2>/dev/null || true
for _ in $(seq 1 200); do
    kill -0 "$SERVER_PID" 2>/dev/null || break
    sleep 0.05
done
kill -KILL "$SERVER_PID" 2>/dev/null || true
SERVER_RC=0
wait "$SERVER_PID" 2>/dev/null || SERVER_RC=$?
LOAD_RC=0
for _ in $(seq 1 100); do
    kill -0 "$LOAD_PID" 2>/dev/null || break
    sleep 0.05
done
kill -KILL "$LOAD_PID" 2>/dev/null || true
wait "$LOAD_PID" 2>/dev/null || LOAD_RC=$?

if [ "$READY" -eq 1 ] && [ "$LOAD_RC" -eq 0 ] && grep -q '^DRAINED' "$LOAD_OUT"; then
    e2e_scenario "rr.load_clients" "keep-alive, mid-request disconnect, reset, held connection drained" "pass"
else
    e2e_scenario "rr.load_clients" "ready=${READY} rc=${LOAD_RC}: $(tr '\n' ' ' <"$LOAD_OUT") $(cat "${LOAD_OUT}.err")" "fail"
fi

DIGEST="$(value_of "$REC_OUT" DIGEST)"
SERVED="$(sed -n 's/^STATS served=\([0-9]*\).*/\1/p' "$REC_OUT" | tail -1)"
ACCEPTED="$(sed -n 's/^STATS .*accepted=\([0-9]*\).*/\1/p' "$REC_OUT" | tail -1)"
if [ "$SERVER_RC" -eq 0 ] && [ -n "$DIGEST" ] && [ -s "$JOURNAL" ] && grep -q '^DRAIN' "$REC_OUT"; then
    e2e_scenario "rr.record_server" "served=${SERVED} accepted=${ACCEPTED} journal=$(wc -c <"$JOURNAL") bytes sha256=$(sha_of "$JOURNAL")" "pass" "$DIGEST"
else
    e2e_scenario "rr.record_server" "rc=${SERVER_RC} digest='${DIGEST}': $(tr '\n' ' ' <"$REC_OUT") $(cat "${REC_OUT}.err")" "fail"
    finish
fi

# The load is fixed: four connections, and at least the four complete
# requests (A: two, C: one, D: one). A half-request is not counted.
if [ "${ACCEPTED:-0}" -eq 4 ] && [ "${SERVED:-0}" -ge 4 ]; then
    e2e_scenario "rr.load_observed" "accepted=${ACCEPTED} served=${SERVED}" "pass"
else
    e2e_scenario "rr.load_observed" "accepted=${ACCEPTED:-?} served=${SERVED:-?}, wanted accepted=4 served>=4" "fail"
fi

# ---------------------------------------------------------------- replay
# First replay: its own scenario, so a failure shows the whole output.
FIRST="${LOG}.replay1.out"
RC=0
$BOUND "$BIN" replay "$JOURNAL" "$DIGEST" "$SERVED" >"$FIRST" 2>"${FIRST}.err" || RC=$?
if [ "$RC" -eq 0 ] && [ "$(value_of "$FIRST" REPLAY_DIGEST)" = "$DIGEST" ]; then
    e2e_scenario "rr.replay_first" "digest equals the recorded one" "pass" "$DIGEST"
else
    e2e_scenario "rr.replay_first" "rc=${RC}: $(tr '\n' ' ' <"$FIRST") $(cat "${FIRST}.err")" "fail"
fi

# No real socket call: the adapters' counter did not move during a replay.
if [ "$(value_of "$FIRST" REAL_NET_CALLS)" = "0" ]; then
    e2e_scenario "rr.replay_opens_no_socket" "real network calls during replay: 0" "pass"
else
    e2e_scenario "rr.replay_opens_no_socket" "real network calls during replay: $(value_of "$FIRST" REAL_NET_CALLS)" "fail"
fi

# N replays, each a fresh process, every digest bit-equal.
SAME=0
FIRST_BAD=""
for i in $(seq 1 "$REPLAYS"); do
    OUT="${LOG}.replay_n.out"
    RC=0
    $BOUND "$BIN" replay "$JOURNAL" "$DIGEST" "$SERVED" >"$OUT" 2>/dev/null || RC=$?
    if [ "$RC" -eq 0 ] && [ "$(value_of "$OUT" REPLAY_DIGEST)" = "$DIGEST" ]; then
        SAME=$((SAME + 1))
    elif [ -z "$FIRST_BAD" ]; then
        FIRST_BAD="replay ${i}: $(tr '\n' ' ' <"$OUT")"
    fi
done
if [ "$SAME" -eq "$REPLAYS" ]; then
    e2e_scenario "rr.replay_bit_exact" "${SAME}/${REPLAYS} replays equal the recorded digest" "pass" "$DIGEST"
else
    e2e_scenario "rr.replay_bit_exact" "${SAME}/${REPLAYS} equal; first failure: ${FIRST_BAD}" "fail"
fi

# --------------------------------------------------------------- refusals
# A flipped byte in the middle of the stream breaks its chain: refused with
# the seq of the chain that caught it, before any program runs.
TAMPERED="${LOG}.tampered.journal"
cp "$JOURNAL" "$TAMPERED"
SIZE="$(wc -c <"$TAMPERED")"
MID=$((SIZE / 2))
BYTE="$(dd if="$TAMPERED" bs=1 skip="$MID" count=1 2>/dev/null | od -An -tu1 | tr -d ' ')"
printf "$(printf '\\%03o' $(((BYTE + 1) % 256)))" |
    dd of="$TAMPERED" bs=1 seek="$MID" count=1 conv=notrunc 2>/dev/null
RC=0
$BOUND "$BIN" replay "$TAMPERED" "$DIGEST" "$SERVED" >"${LOG}.tampered.out" 2>/dev/null || RC=$?
# CORRUPT for a changed payload byte (the chain catches it); INCOMPLETE if
# the changed byte was a length field that makes the stream appear cut.
if [ "$RC" -ne 0 ] && grep -Eq '^REPLAY_REFUSED (CORRUPT|INCOMPLETE) seq=[0-9]' "${LOG}.tampered.out"; then
    e2e_scenario "rr.tampered_refused" "$(grep '^REPLAY_REFUSED' "${LOG}.tampered.out") (byte ${MID} of ${SIZE} changed)" "pass"
else
    e2e_scenario "rr.tampered_refused" "rc=${RC}: $(tr '\n' ' ' <"${LOG}.tampered.out")" "fail"
fi

# A journal naming another asupersync revision is refused at its header.
RC=0
$BOUND "$BIN" replay "$JOURNAL" "$DIGEST" "$SERVED" "not-this-revision" >"${LOG}.rev.out" 2>/dev/null || RC=$?
if [ "$RC" -ne 0 ] && grep -q '^REPLAY_REFUSED BUILD_MISMATCH seq=0' "${LOG}.rev.out"; then
    e2e_scenario "rr.other_revision_refused" "$(grep '^REPLAY_REFUSED' "${LOG}.rev.out")" "pass"
else
    e2e_scenario "rr.other_revision_refused" "rc=${RC}: $(tr '\n' ' ' <"${LOG}.rev.out")" "fail"
fi

# A wrong expected digest is a mismatch, not a pass (the comparison is live).
RC=0
$BOUND "$BIN" replay "$JOURNAL" "0000000000000000" "$SERVED" >"${LOG}.wrongdigest.out" 2>/dev/null || RC=$?
if [ "$RC" -ne 0 ] && grep -q '^REPLAY_MISMATCH digest=DIFFERENT' "${LOG}.wrongdigest.out"; then
    e2e_scenario "rr.digest_comparison_is_live" "a wrong expected digest fails the replay" "pass"
else
    e2e_scenario "rr.digest_comparison_is_live" "rc=${RC}: $(tr '\n' ' ' <"${LOG}.wrongdigest.out")" "fail"
fi

finish
