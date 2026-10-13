#!/usr/bin/env bash
# native_crash_continuity.sh — RP4: a recorded native HTTP server is killed
# mid-stream; the replay of what survived reproduces every committed step
# and ends at the cut (bd-9kll.11.4)
#
# Builds a live POSIX library and e2e_native_record_replay, then:
#   1. records a reference run to completion (persist at chains, fsync per
#      commit through the POSIX file store) under the fixed client load of
#      native_record_replay.sh, and replays it as a recovery replay: it must
#      be accepted as complete, bit-exact, with its checkpoints verified;
#   2. KILL_POINTS times (ASX_E2E_KILL_POINTS, default 50; offsets from an
#      LCG seeded with ASX_E2E_SEED) records a fresh run that SIGKILLs itself
#      when its stream reaches a random byte offset: the file is exactly that
#      long, its tail torn wherever the offset fell, with no flush or finish;
#   3. replays each survivor with replay-prefix: the committed prefix (every
#      record through the last chain that verifies) is served with no socket
#      opened, every committed record is reproduced, the run's trace
#      checkpoints are verified as it goes, and it ends at the cut with the
#      seq of the first record that was not committed;
#   4. compares the last checkpoint the replay verified with the one the
#      recorder logged when it committed that chain (two independent
#      records of the same trace position and digest);
#   5. damages a byte inside one survivor's committed prefix: the prefix
#      must end before the damaged window (CORRUPT, or INCOMPLETE when the
#      byte was a length that now runs past the end).
#
# The kill is a SIGKILL of the recording process, so what the test shows is
# crash continuity of a killed process: the page cache keeps what the sink
# wrote. The fsync at each commit point is the policy for a kernel crash or
# power loss, which this test cannot inflict.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
export ASX_E2E_POLICY_ID="${ASX_E2E_POLICY_ID:-NATIVE-CRASH-CONTINUITY}"
export ASX_E2E_PROFILE="POSIX"
export ASX_E2E_DETERMINISTIC="0"
source "$SCRIPT_DIR/harness.sh"

e2e_init "native-crash-continuity" "E2E-NATIVE-CRASH-CONTINUITY"

# The same live POSIX library (and flags) as native_record_replay.sh and
# http_robustness.sh, so a suite run builds it once; run_all.sh fails a
# family that rewrites build/lib/libasx.a, so it has its own BUILD_DIR.
LIVE_DIR="build/e2e-http-live"
CAPS="-DASX_HTTP_SERVER_MAX_CONNS=80u -DASX_SERVER_MAX_CONNECTIONS=80u -DASX_MAX_TCP_STREAMS=96u"
CAPS="$CAPS -DASX_NATIVE_MAX_SOCKETS=128u -DASX_MAX_IO_TOKENS=128u -DASX_MAX_WAKERS=128u"
BIN="${E2E_ARTIFACT_DIR}/e2e_native_record_replay"
LOG="${E2E_ARTIFACT_DIR}/native_crash_continuity"
KILL_POINTS="${ASX_E2E_KILL_POINTS:-50}"

# A replay that hangs is failed, not waited for.
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
    e2e_scenario "cc.lib_build" "live POSIX library build failed: ${LOG}.lib.log" "fail"
    finish
fi
e2e_scenario "cc.lib_build" "" "pass"

# The driver gets exactly the library's flags (struct layouts must match).
LIB_CFLAGS="$(sed -n 's/^ALL_CFLAGS=//p' "${E2E_PROJECT_ROOT}/${LIVE_DIR}/.build-config")"
mkdir -p "$(dirname "$BIN")"
# shellcheck disable=SC2086
if ! ${CC:-gcc} $LIB_CFLAGS -I"${E2E_PROJECT_ROOT}/include" \
    "${SCRIPT_DIR}/e2e_native_record_replay.c" \
    "${E2E_PROJECT_ROOT}/${LIVE_DIR}/lib/libasx.a" -lpthread -o "$BIN" 2>"${LOG}.build.log"; then
    e2e_scenario "cc.build" "driver build failed: ${LOG}.build.log" "fail"
    finish
fi
e2e_scenario "cc.build" "" "pass"

# value_of FILE KEY: the first token after "KEY " on its (last) line.
value_of() { sed -n "s/^$2 \\([^ ]*\\).*/\\1/p" "$1" | tail -n 1; }

# field LINE NAME: the value of NAME=value in a status line.
field() {
    printf '%s\n' "$1" | awk -v k="$2" '{
        for (i = 1; i <= NF; i++) {
            split($i, a, "=")
            if (a[1] == k) { print a[2]; exit }
        }
    }'
}

# status_line FILE PREFIX: the last line of FILE that begins with "PREFIX ",
# without that word.
status_line() { sed -n "s/^$2 //p" "$1" | tail -n 1; }

file_size() { wc -c <"$1" | tr -d ' '; }

# run_one TAG KILL_AT: one recorded run of the server under the client load,
# killing itself at stream offset KILL_AT (0: never; it then drains on SIGTERM
# as in native_record_replay.sh). Sets RUN_JOURNAL, RUN_CKPT, RUN_OUT and
# RUN_STARTED, RUN_RC (the server's exit status), RUN_KILLED (it died of its
# own SIGKILL, not of ours).
run_one() {
    local tag="$1" kill_at="$2" server port i load forced
    RUN_JOURNAL="${LOG}.${tag}.journal"
    RUN_CKPT="${LOG}.${tag}.ckpt"
    RUN_OUT="${LOG}.${tag}.out"
    RUN_STARTED=0
    RUN_RC=0
    RUN_KILLED=0
    forced=0
    "$BIN" record-crash "$RUN_JOURNAL" "$RUN_CKPT" "$kill_at" >"$RUN_OUT" 2>"${RUN_OUT}.err" &
    server=$!
    port=""
    for i in $(seq 1 200); do
        port="$(sed -n 's/^LISTENING //p' "$RUN_OUT")"
        [ -n "$port" ] && break
        kill -0 "$server" 2>/dev/null || break
        sleep 0.05
    done
    if [ -z "$port" ]; then
        kill -KILL "$server" 2>/dev/null || true
        wait "$server" 2>/dev/null || RUN_RC=$?
        return 0
    fi
    RUN_STARTED=1
    "$BIN" load "$port" >"${RUN_OUT}.load" 2>"${RUN_OUT}.load.err" &
    load=$!
    # Until the server dies (the kill point) or the load has everything but
    # the held connection in place.
    for i in $(seq 1 400); do
        kill -0 "$server" 2>/dev/null || break
        if grep -q '^READY' "${RUN_OUT}.load" 2>/dev/null; then break; fi
        kill -0 "$load" 2>/dev/null || break
        sleep 0.05
    done
    if kill -0 "$server" 2>/dev/null; then
        kill -TERM "$server" 2>/dev/null || true
        for i in $(seq 1 200); do
            kill -0 "$server" 2>/dev/null || break
            sleep 0.05
        done
        if kill -0 "$server" 2>/dev/null; then
            forced=1
            kill -KILL "$server" 2>/dev/null || true
        fi
    fi
    wait "$server" 2>/dev/null || RUN_RC=$?
    for i in $(seq 1 100); do
        kill -0 "$load" 2>/dev/null || break
        sleep 0.05
    done
    kill -KILL "$load" 2>/dev/null || true
    wait "$load" 2>/dev/null || true
    if [ "$RUN_RC" -eq 137 ] && [ "$forced" -eq 0 ]; then RUN_KILLED=1; fi
    return 0
}

# --------------------------------------------------------- reference run
run_one ref 0
REF_DIGEST="$(value_of "$RUN_OUT" DIGEST)"
REF_BYTES="$(value_of "$RUN_OUT" JOURNAL)"
REF_COMMITS="$(sed -n 's/^JOURNAL .* commits=\([0-9]*\).*/\1/p' "$RUN_OUT" | tail -n 1)"
if [ "$RUN_STARTED" -eq 1 ] && [ "$RUN_RC" -eq 0 ] && [ -n "$REF_DIGEST" ] &&
    [ "${REF_BYTES:-0}" -gt 0 ] && [ -s "$RUN_CKPT" ]; then
    e2e_scenario "cc.reference_run" "journal=${REF_BYTES} bytes commits=${REF_COMMITS:-?} ckpt_lines=$(wc -l <"$RUN_CKPT" | tr -d ' ')" "pass" "$REF_DIGEST"
else
    e2e_scenario "cc.reference_run" "started=${RUN_STARTED} rc=${RUN_RC} digest='${REF_DIGEST}': $(tr '\n' ' ' <"$RUN_OUT") $(cat "${RUN_OUT}.err")" "fail"
    finish
fi

# The complete journal through the recovery replay: accepted as complete,
# the same trace digest, every checkpoint on the way verified.
REF_REPLAY="${LOG}.ref.replay.out"
RC=0
$BOUND "$BIN" replay-prefix "$RUN_JOURNAL" >"$REF_REPLAY" 2>"${REF_REPLAY}.err" || RC=$?
REF_PREFIX="$(status_line "$REF_REPLAY" PREFIX)"
REF_PROGRESS="$(status_line "$REF_REPLAY" PREFIX_PROGRESS)"
if [ "$RC" -eq 0 ] && [ "$(field "$REF_PREFIX" complete)" = "1" ] &&
    grep -q '^REPLAY_OK' "$REF_REPLAY" &&
    [ "$(value_of "$REF_REPLAY" REPLAY_DIGEST)" = "$REF_DIGEST" ] &&
    [ "$(field "$REF_PROGRESS" checkpoints)" -gt 0 ]; then
    e2e_scenario "cc.complete_run_replays_exactly" "checkpoints verified=$(field "$REF_PROGRESS" checkpoints) records=$(field "$REF_PREFIX" records)" "pass" "$REF_DIGEST"
else
    e2e_scenario "cc.complete_run_replays_exactly" "rc=${RC}: $(tr '\n' ' ' <"$REF_REPLAY") $(cat "${REF_REPLAY}.err")" "fail"
fi

# ------------------------------------------------------------ kill points
# LCG over [49, reference size / 3): inside the header's successor and the
# first third of the run, where a run of the same load always reaches.
RANGE=$((REF_BYTES / 3 - 49))
if [ "$RANGE" -lt 1 ]; then RANGE=1; fi
X="$E2E_SEED"
KILLED=0
COMPLETED=0
NOT_STARTED=0
OFFSET_BAD=""
CUT_BAD=""
CKPT_BAD=""
CKPT_CHECKED=0
EMPTY_PREFIX=0
BEST_TAG=""
BEST_BYTES=0
for i in $(seq 1 "$KILL_POINTS"); do
    X=$(((X * 1103515245 + 12345) & 2147483647))
    K=$((49 + (X >> 8) % RANGE))
    TAG="k${i}"
    run_one "$TAG" "$K"
    if [ "$RUN_STARTED" -eq 0 ]; then
        NOT_STARTED=$((NOT_STARTED + 1))
        continue
    fi
    OUT="${LOG}.${TAG}.replay.out"
    RC=0
    $BOUND "$BIN" replay-prefix "$RUN_JOURNAL" >"$OUT" 2>"${OUT}.err" || RC=$?
    PREFIX="$(status_line "$OUT" PREFIX)"
    PROGRESS="$(status_line "$OUT" PREFIX_PROGRESS)"
    CUT="$(status_line "$OUT" REPLAY_CUT)"
    if [ "$RUN_KILLED" -eq 1 ]; then
        KILLED=$((KILLED + 1))
        # The file is exactly the stream up to the kill offset.
        SIZE="$(file_size "$RUN_JOURNAL")"
        if [ "$SIZE" != "$K" ] && [ -z "$OFFSET_BAD" ]; then
            OFFSET_BAD="kill ${i}: wanted ${K} bytes, the file has ${SIZE}"
        fi
        # The replay ended at the cut: with the seq the prefix names, every
        # committed record consumed, no native network call.
        if [ "$RC" -ne 0 ] || [ -z "$CUT" ] ||
            [ "$(field "$PREFIX" complete)" != "0" ] ||
            [ "$(field "$CUT" seq)" != "$(field "$PREFIX" cut_seq)" ] ||
            [ "$(field "$PROGRESS" served)" != "$(field "$PREFIX" records)" ] ||
            [ "$(value_of "$OUT" REAL_NET_CALLS)" != "0" ]; then
            if [ -z "$CUT_BAD" ]; then
                CUT_BAD="kill ${i} at ${K}: rc=${RC} $(tr '\n' ' ' <"$OUT") $(cat "${OUT}.err")"
            fi
        fi
        # The last checkpoint the replay verified is the one the recorder
        # logged for that chain.
        if [ "$(field "$PROGRESS" checkpoints)" -gt 0 ]; then
            CKPT_CHECKED=$((CKPT_CHECKED + 1))
            WANT_SEQ="$(field "$PROGRESS" seq)"
            LOGGED="$(awk -v s="seq=${WANT_SEQ}" '$2 == s && $5 == "valid=1" { print; exit }' "$RUN_CKPT")"
            if [ -z "$LOGGED" ] ||
                [ "$(field "$LOGGED" events)" != "$(field "$PROGRESS" events)" ] ||
                [ "$(field "$LOGGED" digest)" != "$(field "$PROGRESS" digest)" ]; then
                if [ -z "$CKPT_BAD" ]; then
                    CKPT_BAD="kill ${i} at ${K}: replay verified seq ${WANT_SEQ} events=$(field "$PROGRESS" events) digest=$(field "$PROGRESS" digest); recorder logged '${LOGGED}'"
                fi
            fi
        else
            EMPTY_PREFIX=$((EMPTY_PREFIX + 1))
        fi
        PB="$(field "$PREFIX" bytes)"
        if [ "${PB:-0}" -gt "$BEST_BYTES" ]; then
            BEST_BYTES="$PB"
            BEST_TAG="$TAG"
        fi
    else
        # The run ended before its kill offset: a complete journal, replayed
        # exactly like the reference.
        COMPLETED=$((COMPLETED + 1))
        if [ "$RC" -ne 0 ] || [ "$(field "$PREFIX" complete)" != "1" ] ||
            ! grep -q '^REPLAY_OK' "$OUT" ||
            [ "$(value_of "$OUT" REPLAY_DIGEST)" != "$(value_of "$RUN_OUT" DIGEST)" ]; then
            if [ -z "$CUT_BAD" ]; then
                CUT_BAD="run ${i} (completed before ${K}): rc=${RC} $(tr '\n' ' ' <"$OUT") $(cat "${OUT}.err")"
            fi
        fi
    fi
done

if [ "$NOT_STARTED" -eq 0 ] && [ "$KILLED" -ge $((KILL_POINTS / 2)) ]; then
    e2e_scenario "cc.runs_were_killed" "seed=${E2E_SEED} points=${KILL_POINTS} killed=${KILLED} completed=${COMPLETED}" "pass"
else
    e2e_scenario "cc.runs_were_killed" "seed=${E2E_SEED} points=${KILL_POINTS} killed=${KILLED} completed=${COMPLETED} not_started=${NOT_STARTED}: too few runs reached their kill offset" "fail"
fi

if [ -z "$OFFSET_BAD" ]; then
    e2e_scenario "cc.kill_leaves_the_stream_cut_at_the_offset" "${KILLED} killed journals are exactly their kill offset long" "pass"
else
    e2e_scenario "cc.kill_leaves_the_stream_cut_at_the_offset" "$OFFSET_BAD" "fail"
fi

if [ -z "$CUT_BAD" ]; then
    e2e_scenario "cc.replay_ends_at_the_cut_with_the_seq" "${KILLED} killed runs ended at their cut, every committed record reproduced, no native network call; ${COMPLETED} complete runs replayed exactly" "pass"
else
    e2e_scenario "cc.replay_ends_at_the_cut_with_the_seq" "$CUT_BAD" "fail"
fi

if [ -z "$CKPT_BAD" ] && [ "$CKPT_CHECKED" -gt 0 ]; then
    e2e_scenario "cc.checkpoints_match_the_recorder" "${CKPT_CHECKED} replays verified a checkpoint equal to the recorder's log; ${EMPTY_PREFIX} cut before their first sampled chain" "pass"
else
    e2e_scenario "cc.checkpoints_match_the_recorder" "${CKPT_BAD:-no replay verified a checkpoint (${EMPTY_PREFIX} empty prefixes)}" "fail"
fi

# ----------------------------------------------- a damaged committed byte
# One byte changed inside the longest survivor's committed prefix: the
# prefix must end before the window that holds it, and the replay must
# still end at a cut.
if [ -n "$BEST_TAG" ] && [ "$BEST_BYTES" -gt 200 ]; then
    DAMAGED="${LOG}.damaged.journal"
    cp "${LOG}.${BEST_TAG}.journal" "$DAMAGED"
    MID=$((BEST_BYTES / 2))
    BYTE="$(dd if="$DAMAGED" bs=1 skip="$MID" count=1 2>/dev/null | od -An -tu1 | tr -d ' ')"
    printf "$(printf '\\%03o' $(((BYTE + 1) % 256)))" |
        dd of="$DAMAGED" bs=1 seek="$MID" count=1 conv=notrunc 2>/dev/null
    OUT="${LOG}.damaged.replay.out"
    RC=0
    $BOUND "$BIN" replay-prefix "$DAMAGED" >"$OUT" 2>"${OUT}.err" || RC=$?
    PREFIX="$(status_line "$OUT" PREFIX)"
    # CORRUPT for a changed record or chain; INCOMPLETE when the changed byte
    # was a length that now runs past the end of the file.
    REASON="$(field "$PREFIX" reason)"
    if [ "$RC" -eq 0 ] && { [ "$REASON" = "CORRUPT" ] || [ "$REASON" = "INCOMPLETE" ]; } &&
        [ "$(field "$PREFIX" complete)" = "0" ] &&
        [ "$(field "$PREFIX" bytes)" -le "$MID" ] && grep -q '^REPLAY_CUT' "$OUT"; then
        e2e_scenario "cc.damaged_window_ends_the_prefix" "byte ${MID} of ${BEST_BYTES} changed: prefix ends at $(field "$PREFIX" bytes) bytes, cut seq $(field "$PREFIX" cut_seq)" "pass"
    else
        e2e_scenario "cc.damaged_window_ends_the_prefix" "rc=${RC}: $(tr '\n' ' ' <"$OUT") $(cat "${OUT}.err")" "fail"
    fi
else
    e2e_scenario "cc.damaged_window_ends_the_prefix" "no survivor had a prefix of more than 200 bytes" "fail"
fi

finish
