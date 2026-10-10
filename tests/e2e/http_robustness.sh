#!/usr/bin/env bash
# http_robustness.sh — the HTTP server on real sockets (bd-9kll.10.3)
#
# Runs e2e_http_robustness (a live native server) and checks, with curl and
# raw /dev/tcp clients:
#   - slow clients (partial request heads) do not block a fresh client, and
#     the idle timeout closes them;
#   - 64 concurrent keep-alive clients are all served;
#   - a drain with stuck clients ends within drain_timeout + slack;
#   - binding 0.0.0.0 and :: serves loopback clients.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
export ASX_E2E_POLICY_ID="${ASX_E2E_POLICY_ID:-HTTP-ROBUSTNESS}"
source "$SCRIPT_DIR/harness.sh"

e2e_init "http-robustness" "E2E-HTTP-ROBUSTNESS"

# A live POSIX library in its own BUILD_DIR (run_all.sh fails a family that
# rewrites build/lib/libasx.a), sized for 64+ concurrent connections.
LIVE_DIR="build/e2e-http-live"
CAPS="-DASX_HTTP_SERVER_MAX_CONNS=80u -DASX_SERVER_MAX_CONNECTIONS=80u -DASX_MAX_TCP_STREAMS=96u"
CAPS="$CAPS -DASX_NATIVE_MAX_SOCKETS=128u -DASX_MAX_IO_TOKENS=128u -DASX_MAX_WAKERS=128u"
BIN="${E2E_ARTIFACT_DIR}/e2e_http_robustness"
LOG="${E2E_ARTIFACT_DIR}/http_robustness"
SLACK_MS="${ASX_E2E_HTTP_SLACK_MS:-1500}"

finish() {
    set +e
    e2e_finish
    exit $?
}

if ! command -v curl >/dev/null 2>&1; then
    e2e_scenario "http.curl" "curl is not installed" "fail"
    finish
fi

if ! "${MAKE:-make}" -C "$E2E_PROJECT_ROOT" build PROFILE=POSIX DETERMINISTIC=0 \
    BUILD_DIR="$LIVE_DIR" CFLAGS="$CAPS" LDFLAGS="-lpthread" >"${LOG}.lib.log" 2>&1; then
    e2e_scenario "http.lib_build" "live POSIX library build failed: ${LOG}.lib.log" "fail"
    finish
fi
e2e_scenario "http.lib_build" "" "pass"

# The driver gets exactly the library's flags (struct layouts must match).
LIB_CFLAGS="$(sed -n 's/^ALL_CFLAGS=//p' "${E2E_PROJECT_ROOT}/${LIVE_DIR}/.build-config")"
mkdir -p "$(dirname "$BIN")"
# shellcheck disable=SC2086
if ! ${CC:-gcc} $LIB_CFLAGS -I"${E2E_PROJECT_ROOT}/include" "${SCRIPT_DIR}/e2e_http_robustness.c" \
    "${E2E_PROJECT_ROOT}/${LIVE_DIR}/lib/libasx.a" -lpthread -o "$BIN" 2>"${LOG}.build.log"; then
    e2e_scenario "http.build" "driver build failed: ${LOG}.build.log" "fail"
    finish
fi
e2e_scenario "http.build" "" "pass"

SERVER_PID=""
PORT=""

# start_server ADDR IDLE_MS DRAIN_MS OUT: run the driver, wait for its port.
start_server() {
    local i
    PORT=""
    "$BIN" "$1" "$2" "$3" >"$4" 2>"$4.err" &
    SERVER_PID=$!
    for i in $(seq 1 200); do
        PORT="$(sed -n 's/^LISTENING //p' "$4")"
        [ -n "$PORT" ] && return 0
        kill -0 "$SERVER_PID" 2>/dev/null || return 1
        sleep 0.05
    done
    return 1
}

# stop_server: SIGTERM, then wait (in this shell: the server is its child);
# sets ELAPSED_MS.
# A server still running 10 s after SIGTERM is killed (a drain that never
# ends fails its scenario instead of hanging the suite).
ELAPSED_MS=0
stop_server() {
    local t0 t1 i
    t0=$(date +%s%N)
    kill -TERM "$SERVER_PID" 2>/dev/null || true
    for i in $(seq 1 200); do
        kill -0 "$SERVER_PID" 2>/dev/null || break
        sleep 0.05
    done
    kill -KILL "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    t1=$(date +%s%N)
    ELAPSED_MS=$(((t1 - t0) / 1000000))
}

now_ms() { echo $(($(date +%s%N) / 1000000)); }

stat_of() { sed -n "s/.*[ ]$2=\\([0-9]*\\).*/\\1/p" "$1" | tail -1; }

# --- 0.0.0.0, 1 s idle timeout --------------------------------------------
OUT="${LOG}.any4.out"
if ! start_server 0.0.0.0 1000 3000 "$OUT"; then
    e2e_scenario "http.bind_any4" "server did not start: $(cat "$OUT.err")" "fail"
    finish
fi
if [ "$(curl -s -m 3 "http://127.0.0.1:${PORT}/hello")" = "hello" ]; then
    e2e_scenario "http.bind_any4" "" "pass"
else
    e2e_scenario "http.bind_any4" "GET /hello on 0.0.0.0:${PORT} via 127.0.0.1 failed" "fail"
fi

# Eight slow clients hold partial request heads.
SLOW_FDS=()
for i in $(seq 1 8); do
    exec {fd}<>"/dev/tcp/127.0.0.1/${PORT}"
    printf 'GET /hello HTTP/1.1\r\nHo' >&"$fd"
    SLOW_FDS+=("$fd")
done
sleep 0.2
t0=$(now_ms)
body="$(curl -s -m 3 "http://127.0.0.1:${PORT}/hello" || true)"
dt=$(($(now_ms) - t0))
if [ "$body" = "hello" ] && [ "$dt" -lt 1000 ]; then
    e2e_scenario "http.slow_clients_do_not_block" "fresh client served in ${dt} ms" "pass"
else
    e2e_scenario "http.slow_clients_do_not_block" "fresh client got '${body}' after ${dt} ms" "fail"
fi

# The idle timeout (1 s) closes them: each read ends at EOF, not by timing out.
sleep 1.2
closed=0
for fd in "${SLOW_FDS[@]}"; do
    rc=0
    read -r -t 3 -u "$fd" _line || rc=$?
    [ "$rc" -eq 1 ] && closed=$((closed + 1))
    exec {fd}>&-
done
if [ "$closed" -eq 8 ]; then
    e2e_scenario "http.idle_timeout_closes_slow_clients" "" "pass"
else
    e2e_scenario "http.idle_timeout_closes_slow_clients" "${closed}/8 closed" "fail"
fi

# 64 concurrent keep-alive clients, three requests each on one connection.
PIDS=()
for i in $(seq 1 64); do
    curl -s -m 10 "http://127.0.0.1:${PORT}/hello" "http://127.0.0.1:${PORT}/hello" \
        "http://127.0.0.1:${PORT}/hello" >"${LOG}.client${i}" &
    PIDS+=("$!")
done
ok=0
for i in $(seq 1 64); do
    wait "${PIDS[$((i - 1))]}" || true
    [ "$(cat "${LOG}.client${i}")" = "hellohellohello" ] && ok=$((ok + 1))
done
if [ "$ok" -eq 64 ]; then
    e2e_scenario "http.concurrent_keepalive_64" "" "pass"
else
    e2e_scenario "http.concurrent_keepalive_64" "${ok}/64 clients served" "fail"
fi

stop_server
timed_out="$(stat_of "$OUT" timed_out)"
served="$(stat_of "$OUT" served)"
if [ "${timed_out:-0}" -ge 8 ] && [ "${served:-0}" -ge 194 ]; then
    e2e_scenario "http.stats_any4" "served=${served} timed_out=${timed_out}" "pass"
else
    e2e_scenario "http.stats_any4" "served=${served:-?} timed_out=${timed_out:-?}" "fail"
fi

# --- drain: stuck clients, no idle timeout, 1 s drain timeout -------------
OUT="${LOG}.drain.out"
if ! start_server 127.0.0.1 0 1000 "$OUT"; then
    e2e_scenario "http.drain_bounded" "server did not start: $(cat "$OUT.err")" "fail"
    finish
fi
STUCK_FDS=()
for i in 1 2 3; do
    exec {fd}<>"/dev/tcp/127.0.0.1/${PORT}"
    printf 'POST /hello HTTP/1.1\r\nHost: a\r\nContent-Length: 100\r\n\r\npartial' >&"$fd"
    STUCK_FDS+=("$fd")
done
sleep 0.3
stop_server
elapsed="$ELAPSED_MS"
for fd in "${STUCK_FDS[@]}"; do exec {fd}>&-; done
drain_closed="$(stat_of "$OUT" drain_closed)"
if [ "$elapsed" -ge 900 ] && [ "$elapsed" -le $((1000 + SLACK_MS)) ] &&
    [ "${drain_closed:-0}" -eq 3 ]; then
    e2e_scenario "http.drain_bounded" "exited ${elapsed} ms after SIGTERM, drain_closed=3" "pass"
else
    e2e_scenario "http.drain_bounded" \
        "exited ${elapsed} ms after SIGTERM (want 900..$((1000 + SLACK_MS))), drain_closed=${drain_closed:-?}" "fail"
fi

# --- :: (IPv6 unspecified) -------------------------------------------------
OUT="${LOG}.any6.out"
if ! start_server :: 1000 1000 "$OUT"; then
    e2e_scenario "http.bind_any6" "no IPv6 here: $(cat "$OUT.err")" "skip"
else
    if [ "$(curl -g -s -m 3 "http://[::1]:${PORT}/hello")" = "hello" ]; then
        e2e_scenario "http.bind_any6" "" "pass"
    else
        e2e_scenario "http.bind_any6" "GET /hello on [::]:${PORT} via [::1] failed" "fail"
    fi
    stop_server
fi

finish
