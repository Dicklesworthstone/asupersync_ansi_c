#!/usr/bin/env bash
# test_crate_acceptance_gate.sh — unit test for crate acceptance gate script
#
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
GATE_SCRIPT="$PROJECT_ROOT/tools/ci/run_crate_acceptance_gate.py"

PASS=0
FAIL=0
TMPDIR_BASE=""

setup() {
    TMPDIR_BASE=$(mktemp -d)
    trap 'rm -rf "$TMPDIR_BASE"' EXIT
}

assert_eq() {
    local label="$1" expected="$2" actual="$3"
    if [ "$expected" = "$actual" ]; then
        echo "  PASS: $label"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $label (expected=$expected actual=$actual)"
        FAIL=$((FAIL + 1))
    fi
}

assert_contains() {
    local label="$1" haystack="$2" needle="$3"
    if echo "$haystack" | command grep -qF "$needle"; then
        echo "  PASS: $label"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $label (missing '$needle')"
        FAIL=$((FAIL + 1))
    fi
}

create_doc() {
    local path="$1"
    shift
    mkdir -p "$(dirname "$path")"
    {
        for line in "$@"; do
            printf '%s\n' "$line"
        done
    } >"$path"
}

create_mock_repo() {
    local root="$1"
    mkdir -p "$root/build/e2e-artifacts/e2e-pass" \
             "$root/tools/ci/artifacts/conformance" \
             "$root/tests/unit/cx" "$root/tests/unit/runtime" "$root/tests/unit/security" \
             "$root/tests/unit/app" "$root/tests/unit/evidence" "$root/tests/unit/service" \
             "$root/tests/unit/transport" "$root/tests/unit/remote" "$root/tests/unit/spork" \
             "$root/tests/unit/stream" "$root/tests/unit/net" "$root/tests/unit/migration" \
             "$root/tests/unit/raptorq" "$root/tests/unit/console" "$root/tests/unit/tracing_compat" \
             "$root/tests/unit/actor" "$root/tests/e2e"

    create_doc "$root/docs/FEATURE_PARITY.md" "# feature parity"
    create_doc "$root/docs/SUBSYSTEM_EVIDENCE_MATRIX.md" \
        "bd-yx9r.4*" "bd-yx9r.5*" "bd-yx9r.6*" "bd-yx9r.7*" "bd-yx9r.8*" "bd-yx9r.9*" \
        "bd-yx9r.10*" "bd-yx9r.11*" "bd-yx9r.12*" "actor / gen_server / supervision"
    create_doc "$root/docs/USER_WORKFLOW_ACCEPTANCE_INVENTORY.md" \
        "bd-yx9r.[4-12].*" "HTTP/gRPC/server/web/process/signal/CLI surfaces" \
        "Messaging/remote/distributed" "docs-safe artifact bundle"
    create_doc "$root/docs/E2E_SCENARIO_PACKS_AND_REPORT_CONTRACT.md" \
        "bd-yx9r.4" "bd-yx9r.5" "bd-yx9r.6" "bd-yx9r.7" "bd-yx9r.8" "bd-yx9r.9" \
        "bd-yx9r.10" "bd-yx9r.11" "bd-yx9r.12" "bd-yx9r.19"
    create_doc "$root/docs/DEFERRED_SURFACE_REGISTER.md" \
        "Runtime object/config/bootstrap beyond kernel loop" \
        "Capability / structured-concurrency surface" \
        "Trace / replay / lab / diagnostics" \
        "Networking core (\`net\`)" \
        "HTTP / web / gRPC / transport / service / remote / distributed / messaging / server / TLS / database" \
        "Actor / supervision / gen_server"
    create_doc "$root/docs/WAVE_GATING_PROTOCOL.md" "# wave gating"

    : >"$root/tests/unit/cx/test_cx.c"
    : >"$root/tests/unit/runtime/test_runtime.c"
    : >"$root/tests/unit/security/test_security.c"
    : >"$root/tests/unit/app/test_app.c"
    : >"$root/tests/unit/evidence/test_evidence.c"
    : >"$root/tests/unit/service/test_service.c"
    : >"$root/tests/unit/transport/test_transport.c"
    : >"$root/tests/unit/remote/test_remote.c"
    : >"$root/tests/unit/spork/test_spork.c"
    : >"$root/tests/unit/stream/test_stream.c"
    : >"$root/tests/unit/net/test_net.c"
    : >"$root/tests/unit/net/test_tls.c"
    : >"$root/tests/unit/net/test_server.c"
    : >"$root/tests/unit/net/test_quic.c"
    : >"$root/tests/unit/net/test_websocket.c"
    : >"$root/tests/unit/net/test_http.c"
    : >"$root/tests/unit/net/test_web.c"
    : >"$root/tests/unit/net/test_grpc.c"
    : >"$root/tests/unit/net/test_db.c"
    : >"$root/tests/unit/net/test_messaging.c"
    : >"$root/tests/unit/net/test_distributed.c"
    : >"$root/tests/unit/migration/test_migration.c"
    : >"$root/tests/unit/raptorq/test_raptorq.c"
    : >"$root/tests/unit/console/test_console.c"
    : >"$root/tests/unit/tracing_compat/test_tracing_compat.c"
    : >"$root/tests/unit/actor/test_actor.c"

    : >"$root/tests/e2e/foundational_contracts.sh"
    : >"$root/tests/e2e/core_lifecycle.sh"
    : >"$root/tests/e2e/native_host.sh"
    : >"$root/tests/e2e/server_shutdown.sh"
    : >"$root/tests/e2e/network_surface.sh"
    : >"$root/tests/e2e/continuity.sh"
    : >"$root/tests/e2e/router_storm.sh"
    : >"$root/tests/e2e/browser_smoke.sh"
    : >"$root/tests/e2e/continuity_restart.sh"
    : >"$root/tests/e2e/examples_smoke.sh"

    cat >"$root/tools/ci/artifacts/conformance/conformance-good.summary.json" <<'EOF_CONFORMANCE'
{"mode":"conformance","fail":0,"semantic_delta_pass":true}
EOF_CONFORMANCE
    cat >"$root/tools/ci/artifacts/conformance/codec-good.summary.json" <<'EOF_CODEC'
{"mode":"codec-equivalence","fail":0}
EOF_CODEC
    cat >"$root/tools/ci/artifacts/conformance/profile-good.summary.json" <<'EOF_PROFILE'
{"mode":"profile-parity","fail":0,"semantic_delta_pass":true}
EOF_PROFILE
    cat >"$root/build/e2e-artifacts/e2e-pass/run_manifest.json" <<'EOF_E2E'
{"status":"pass","failed_families":0}
EOF_E2E
}

test_gate_passes_with_complete_inputs() {
    echo "Test: gate passes with complete inputs"
    local repo="$TMPDIR_BASE/repo-pass"
    create_mock_repo "$repo"

    local output rc=0
    output=$(python3 "$GATE_SCRIPT" --repo-root "$repo" --run-id test-pass 2>&1) || rc=$?

    assert_eq "exit code" "0" "$rc"
    assert_contains "summary says pass" "$output" '"status": "pass"'
    assert_contains "report file emitted" "$(cat "$repo/tools/ci/artifacts/acceptance/crate_acceptance_test-pass.summary.json")" '"status": "pass"'
}

test_gate_fails_when_family_docs_missing() {
    echo "Test: gate fails when family documentation anchor is missing"
    local repo="$TMPDIR_BASE/repo-fail-doc"
    create_mock_repo "$repo"
    create_doc "$repo/docs/E2E_SCENARIO_PACKS_AND_REPORT_CONTRACT.md" \
        "bd-yx9r.4" "bd-yx9r.5" "bd-yx9r.6" "bd-yx9r.7" "bd-yx9r.8" "bd-yx9r.9" \
        "bd-yx9r.10" "bd-yx9r.11" "bd-yx9r.12"

    local output rc=0
    output=$(python3 "$GATE_SCRIPT" --repo-root "$repo" --run-id test-fail-doc 2>&1) || rc=$?

    assert_eq "exit code" "1" "$rc"
    assert_contains "family listed in failures" "$output" 'bd-yx9r.19'
}

main() {
    echo "=========================================="
    echo "Crate Acceptance Gate — Unit Tests"
    echo "=========================================="
    echo ""

    setup
    test_gate_passes_with_complete_inputs
    echo ""
    test_gate_fails_when_family_docs_missing
    echo ""

    echo "=========================================="
    echo "Summary: PASS=$PASS  FAIL=$FAIL"
    echo "=========================================="

    if [ "$FAIL" -gt 0 ]; then
        exit 1
    fi
    exit 0
}

main
