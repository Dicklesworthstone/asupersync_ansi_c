/*
 * browser_diagnostic.c — browser-specific diagnostics and DX error taxonomy
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/runtime/browser_diagnostic.h>
#include <asx/core/ghost.h>
#include <asx/runtime/trace.h>
#include <stddef.h>

/* -------------------------------------------------------------------
 * DX error taxonomy catalog
 * ------------------------------------------------------------------- */

static const asx_browser_dx_entry g_dx_catalog[ASX_BROWSER_DX_COUNT] = {
    /* NONE */
    {ASX_BROWSER_DX_NONE, NULL, NULL, NULL},
    /* NATIVE_FS */
    {ASX_BROWSER_DX_NATIVE_FS, "filesystem unavailable",
     "The browser profile runs in a WASM sandbox without filesystem access. "
     "Native fs operations (read, write, stat) are blocked.",
     "Use IndexedDB via the host JS bridge, or pass data through channels."},
    /* NATIVE_PROCESS */
    {ASX_BROWSER_DX_NATIVE_PROCESS, "process spawn unavailable",
     "The browser sandbox cannot spawn child processes. "
     "Process operations return ASX_E_PERMISSION_DENIED.",
     "Use Web Workers via the host JS bridge for background computation."},
    /* NATIVE_SIGNAL */
    {ASX_BROWSER_DX_NATIVE_SIGNAL, "signal handling unavailable",
     "OS signal handling (SIGTERM, SIGINT, etc.) is not available in the browser. "
     "The WASM sandbox has no signal delivery mechanism.",
     "Use the cancellation protocol (asx_cancel) for cooperative shutdown."},
    /* NATIVE_IO */
    {ASX_BROWSER_DX_NATIVE_IO, "native I/O driver unavailable",
     "The native reactor/epoll/kqueue I/O driver is not available in the browser. "
     "WASM has no direct access to OS I/O multiplexing.",
     "Use the timer wheel for scheduling and channels for data flow."},
    /* NATIVE_BLOCKING */
    {ASX_BROWSER_DX_NATIVE_BLOCKING, "blocking thread pool unavailable",
     "The browser WASM environment runs on the main thread or in a Web Worker. "
     "The blocking thread pool is not available.",
     "Use cooperative scheduling with bounded poll budgets."},
    /* NATIVE_SERVER */
    {ASX_BROWSER_DX_NATIVE_SERVER, "server substrate unavailable",
     "The browser profile cannot host the native listener/accept loop surface used by "
     "the deterministic server family.",
     "Terminate server functionality at the JS host boundary and keep browser code on the "
     "client/channel side."},
    /* NATIVE_GRPC */
    {ASX_BROWSER_DX_NATIVE_GRPC, "gRPC family unavailable",
     "The browser build excludes the native-facing gRPC transport/server family even though "
     "the core profile can exercise it in-memory.",
     "Move RPC dispatch to a host bridge or compile a native profile for direct gRPC use."},
    /* NATIVE_MESSAGING */
    {ASX_BROWSER_DX_NATIVE_MESSAGING, "messaging broker unavailable",
     "The browser build excludes the broker-style messaging family from the fail-closed "
     "public surface.",
     "Route pub/sub through browser-safe channels or a host-managed messaging bridge."},
    /* NATIVE_TLS */
    {ASX_BROWSER_DX_NATIVE_TLS, "TLS family unavailable",
     "The browser build excludes the native-facing TLS connector/acceptor surface even though "
     "the core profile models it deterministically.",
     "Terminate TLS at the browser host boundary or compile a native profile for direct TLS use."},
    /* NATIVE_DATABASE */
    {ASX_BROWSER_DX_NATIVE_DATABASE, "database family unavailable",
     "The browser build excludes the database connection/query family from the fail-closed "
     "public surface.",
     "Move database access behind a host API or service boundary instead of using the browser build directly."},
    /* WRONG_PROFILE */
    {ASX_BROWSER_DX_WRONG_PROFILE, "profile mismatch",
     "Code compiled for a native profile is running in a browser context. "
     "The active profile does not match the expected browser profile.",
     "Recompile with -DASX_PROFILE_BROWSER to activate browser boundary enforcement."},
    /* ALLOCATOR_UNSEAL */
    {ASX_BROWSER_DX_ALLOCATOR_UNSEAL, "allocator not sealed",
     "The browser profile supports allocator sealing for steady-state "
     "zero-allocation operation, but the allocator has not been sealed.",
     "Call asx_runtime_seal_allocator() after initialization to prevent "
     "unexpected allocations during steady-state operation."}};

const asx_browser_dx_entry *asx_browser_dx_lookup(asx_browser_dx_class dx_class) {
    if ((int)dx_class <= 0 || (int)dx_class >= ASX_BROWSER_DX_COUNT) { return NULL; }
    return &g_dx_catalog[(int)dx_class];
}

asx_browser_dx_class asx_browser_dx_for_surface(asx_host_surface surface) {
    switch (surface) {
    case ASX_SURFACE_FILESYSTEM: return ASX_BROWSER_DX_NATIVE_FS;
    case ASX_SURFACE_PROCESS: return ASX_BROWSER_DX_NATIVE_PROCESS;
    case ASX_SURFACE_SIGNAL: return ASX_BROWSER_DX_NATIVE_SIGNAL;
    case ASX_SURFACE_IO_DRIVER: return ASX_BROWSER_DX_NATIVE_IO;
    case ASX_SURFACE_BLOCKING: return ASX_BROWSER_DX_NATIVE_BLOCKING;
    case ASX_SURFACE_SERVER: return ASX_BROWSER_DX_NATIVE_SERVER;
    case ASX_SURFACE_GRPC: return ASX_BROWSER_DX_NATIVE_GRPC;
    case ASX_SURFACE_MESSAGING: return ASX_BROWSER_DX_NATIVE_MESSAGING;
    case ASX_SURFACE_TLS: return ASX_BROWSER_DX_NATIVE_TLS;
    case ASX_SURFACE_DATABASE: return ASX_BROWSER_DX_NATIVE_DATABASE;
    case ASX_SURFACE_REGION:
    case ASX_SURFACE_TASK:
    case ASX_SURFACE_CHANNEL:
    case ASX_SURFACE_TIMER:
    case ASX_SURFACE_OBLIGATION:
    case ASX_SURFACE_TRACE:
    case ASX_SURFACE_GHOST:
    case ASX_SURFACE_COUNT: return ASX_BROWSER_DX_NONE;
    }
    return ASX_BROWSER_DX_NONE;
}

uint32_t asx_browser_dx_count(void) { return ASX_BROWSER_DX_COUNT; }

/* -------------------------------------------------------------------
 * Browser evidence collector
 * ------------------------------------------------------------------- */

void asx_browser_evidence_capture(asx_browser_evidence_report *report) {
    asx_profile_id id;

    if (!report) return;

    id = asx_profile_active();
    report->active_profile = id;
    report->is_browser = (id == ASX_PROFILE_ID_BROWSER) ? 1 : 0;
    report->surfaces_available = asx_surface_available_count(id);
    report->surfaces_blocked = asx_surface_blocked_count(id);

    /* Check allocator sealability from descriptor */
    {
        asx_profile_descriptor desc;
        if (asx_profile_get_descriptor(id, &desc) == ASX_OK) {
            report->allocator_sealable = desc.allocator_sealable;
        } else {
            report->allocator_sealable = 0;
        }
    }

    /* All semantic rules must hold */
    {
        uint32_t i;
        report->all_semantic_rules_hold = 1;
        for (i = 0; i < asx_profile_semantic_rule_count(); i++) {
            if (!asx_profile_semantic_rule_enforced((asx_semantic_rule)i)) {
                report->all_semantic_rules_hold = 0;
                break;
            }
        }
    }
}

asx_status asx_browser_evidence_to_sink(const asx_browser_evidence_report *report,
                                         asx_evidence_sink *sink) {
    asx_status rc;

    if (!report || !sink) return ASX_E_INVALID_ARGUMENT;

    /* Profile identity check */
    if (report->is_browser) {
        rc = asx_evidence_record(sink, "browser:profile", ASX_EVIDENCE_PASS,
                                 "active profile is BROWSER", 0);
    } else {
        rc = asx_evidence_record(sink, "browser:profile", ASX_EVIDENCE_INFO,
                                 "active profile is not BROWSER (boundary not enforced)", 0);
    }
    if (rc != ASX_OK) return rc;

    /* Boundary enforcement */
    if (report->is_browser && report->surfaces_blocked > 0) {
        rc = asx_evidence_record(sink, "browser:boundary", ASX_EVIDENCE_PASS,
                                 "native surfaces blocked by fail-closed gate", 0);
    } else if (report->is_browser) {
        rc = asx_evidence_record(sink, "browser:boundary", ASX_EVIDENCE_WARN,
                                 "browser profile active but no surfaces blocked", 0);
    } else {
        rc = asx_evidence_record(sink, "browser:boundary", ASX_EVIDENCE_INFO,
                                 "boundary not active (non-browser profile)", 0);
    }
    if (rc != ASX_OK) return rc;

    /* Semantic parity */
    if (report->all_semantic_rules_hold) {
        rc = asx_evidence_record(sink, "browser:parity", ASX_EVIDENCE_PASS,
                                 "all semantic rules enforced", 0);
    } else {
        rc = asx_evidence_record(sink, "browser:parity", ASX_EVIDENCE_FAIL,
                                 "semantic rule violation detected", 0);
    }
    if (rc != ASX_OK) return rc;

    /* Allocator seal status */
    if (report->allocator_sealable) {
        rc = asx_evidence_record(sink, "browser:allocator", ASX_EVIDENCE_PASS,
                                 "allocator sealable for steady-state", 0);
    } else {
        rc = asx_evidence_record(sink, "browser:allocator", ASX_EVIDENCE_INFO,
                                 "allocator not sealable in this profile", 0);
    }

    return rc;
}

/* -------------------------------------------------------------------
 * Flake governance
 * ------------------------------------------------------------------- */

void asx_browser_flake_capture(asx_browser_flake_snapshot *snapshot,
                                const asx_evidence_sink *sink) {
    if (!snapshot) return;

    snapshot->trace_digest = asx_trace_digest();
    snapshot->profile = asx_profile_active();
    snapshot->ghost_violations = asx_ghost_violation_count();
    snapshot->surfaces_blocked = asx_surface_blocked_count(snapshot->profile);
    snapshot->is_deterministic = 1; /* always deterministic in this runtime */

    if (sink) {
        snapshot->evidence_fail_count = sink->fail_count;
        snapshot->evidence_warn_count = sink->warn_count;
    } else {
        snapshot->evidence_fail_count = 0;
        snapshot->evidence_warn_count = 0;
    }
}

int asx_browser_flake_match(const asx_browser_flake_snapshot *a,
                             const asx_browser_flake_snapshot *b) {
    if (!a || !b) return 0;

    return a->trace_digest == b->trace_digest && a->profile == b->profile &&
           a->ghost_violations == b->ghost_violations &&
           a->surfaces_blocked == b->surfaces_blocked &&
           a->evidence_fail_count == b->evidence_fail_count &&
           a->evidence_warn_count == b->evidence_warn_count &&
           a->is_deterministic == b->is_deterministic;
}
