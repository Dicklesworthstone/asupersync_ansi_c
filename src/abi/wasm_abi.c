/*
 * wasm_abi.c — wasm ABI type contracts and transition helpers
 *
 * Implements ABI version introspection, compatibility checking,
 * bootstrap/provider/hook transition validation, and string helpers.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/abi/wasm_abi.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* ABI version and signature                                           */
/* ------------------------------------------------------------------ */

asx_abi_version asx_abi_version_current(void) {
    asx_abi_version v;
    v.major = ASX_WASM_ABI_VERSION_MAJOR;
    v.minor = ASX_WASM_ABI_VERSION_MINOR;
    v.patch = ASX_WASM_ABI_VERSION_PATCH;
    v.feature_flags = 0u;

    /* Auto-detect features from compile-time config */
#if defined(ASX_DETERMINISTIC) && ASX_DETERMINISTIC
    v.feature_flags |= ASX_ABI_FEATURE_DETERMINISTIC;
#endif
    /* Channels, timers, obligations are always available in core */
    v.feature_flags |= ASX_ABI_FEATURE_CHANNELS;
    v.feature_flags |= ASX_ABI_FEATURE_TIMERS;
    v.feature_flags |= ASX_ABI_FEATURE_OBLIGATIONS;
    v.feature_flags |= ASX_ABI_FEATURE_SYMBOLS;
#if defined(ASX_CODEC_JSON)
    v.feature_flags |= ASX_ABI_FEATURE_CODEC_JSON;
#endif
#if defined(ASX_CODEC_BIN)
    v.feature_flags |= ASX_ABI_FEATURE_CODEC_BIN;
#endif
#if defined(ASX_DEBUG_GHOST) && ASX_DEBUG_GHOST
    v.feature_flags |= ASX_ABI_FEATURE_GHOST_MONITORS;
#endif

    return v;
}

asx_abi_signature asx_abi_signature_compute(void) {
    asx_abi_version v = asx_abi_version_current();
    /* Pack: major[63:56] minor[55:48] patch[47:32] features[31:0] */
    return ((uint64_t)v.major << 56) | ((uint64_t)v.minor << 48) | ((uint64_t)v.patch << 32) |
           (uint64_t)v.feature_flags;
}

/* ------------------------------------------------------------------ */
/* Compatibility checking                                              */
/* ------------------------------------------------------------------ */

asx_status asx_abi_check_compat(const asx_abi_version *local, const asx_abi_version *remote,
                                asx_abi_compat_result *out) {
    if (local == NULL || remote == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;

    out->local = *local;
    out->remote = *remote;
    out->missing_features = remote->feature_flags & ~local->feature_flags;
    out->extra_features = local->feature_flags & ~remote->feature_flags;

    /* Major version mismatch → incompatible */
    if (local->major != remote->major) {
        out->compat = ASX_ABI_INCOMPATIBLE;
        return ASX_OK;
    }

    /* Minor version: older local can serve newer remote in degraded mode */
    if (local->minor < remote->minor || out->missing_features != 0u) {
        out->compat = ASX_ABI_DEGRADED;
        return ASX_OK;
    }

    out->compat = ASX_ABI_COMPATIBLE;
    return ASX_OK;
}

const char *asx_abi_compat_str(asx_abi_compat compat) {
    switch (compat) {
    case ASX_ABI_COMPATIBLE: return "compatible";
    case ASX_ABI_DEGRADED: return "degraded";
    case ASX_ABI_INCOMPATIBLE: return "incompatible";
    default: return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/* Bootstrap transition table                                          */
/*                                                                     */
/* Legal transitions:                                                  */
/*   UNINITIALIZED → ABI_CHECK                                         */
/*   ABI_CHECK     → HOOKS_BIND | FAILED                               */
/*   HOOKS_BIND    → PROVIDER_INIT | FAILED                            */
/*   PROVIDER_INIT → RUNTIME_INIT | FAILED                             */
/*   RUNTIME_INIT  → READY | FAILED                                    */
/*   READY         → SHUTDOWN                                          */
/*   SHUTDOWN      → TERMINATED | FAILED                               */
/*   FAILED        → UNINITIALIZED (re-bootstrap)                      */
/*   TERMINATED    → UNINITIALIZED (re-bootstrap)                      */
/* ------------------------------------------------------------------ */

static const uint16_t g_boot_legal[ASX_BOOT_PHASE_COUNT] = {
    /* UNINITIALIZED */ (1u << ASX_BOOT_PHASE_ABI_CHECK),
    /* ABI_CHECK     */ (1u << ASX_BOOT_PHASE_HOOKS_BIND) | (1u << ASX_BOOT_PHASE_FAILED),
    /* HOOKS_BIND    */ (1u << ASX_BOOT_PHASE_PROVIDER_INIT) | (1u << ASX_BOOT_PHASE_FAILED),
    /* PROVIDER_INIT */ (1u << ASX_BOOT_PHASE_RUNTIME_INIT) | (1u << ASX_BOOT_PHASE_FAILED),
    /* RUNTIME_INIT  */ (1u << ASX_BOOT_PHASE_READY) | (1u << ASX_BOOT_PHASE_FAILED),
    /* READY         */ (1u << ASX_BOOT_PHASE_SHUTDOWN),
    /* SHUTDOWN      */ (1u << ASX_BOOT_PHASE_TERMINATED) | (1u << ASX_BOOT_PHASE_FAILED),
    /* TERMINATED    */ (1u << ASX_BOOT_PHASE_UNINITIALIZED),
    /* FAILED        */ (1u << ASX_BOOT_PHASE_UNINITIALIZED),
};

asx_status asx_boot_transition_check(asx_boot_phase from, asx_boot_phase to) {
    if ((int)from < 0 || (int)from >= ASX_BOOT_PHASE_COUNT || (int)to < 0 ||
        (int)to >= ASX_BOOT_PHASE_COUNT)
        return ASX_E_INVALID_ARGUMENT;

    if (g_boot_legal[(int)from] & (1u << (int)to)) return ASX_OK;
    return ASX_E_INVALID_TRANSITION;
}

const char *asx_boot_phase_str(asx_boot_phase phase) {
    static const char *names[] = {"uninitialized", "abi_check",    "hooks_bind",
                                  "provider_init", "runtime_init", "ready",
                                  "shutdown",      "terminated",   "failed"};
    if ((int)phase < 0 || (int)phase >= ASX_BOOT_PHASE_COUNT) return "unknown";
    return names[(int)phase];
}

/* ------------------------------------------------------------------ */
/* Provider transition table                                           */
/*                                                                     */
/* UNREGISTERED → REGISTERING                                          */
/* REGISTERING  → ACTIVE | FAILED                                      */
/* ACTIVE       → SUSPENDING | REMOVING                                */
/* SUSPENDING   → SUSPENDED | FAILED                                   */
/* SUSPENDED    → ACTIVE | REMOVING                                    */
/* REMOVING     → REMOVED | FAILED                                     */
/* REMOVED      → (terminal)                                           */
/* FAILED       → UNREGISTERED                                         */
/* ------------------------------------------------------------------ */

static const uint8_t g_provider_legal[ASX_PROVIDER_STATE_COUNT] = {
    /* UNREGISTERED */ (1u << ASX_PROVIDER_REGISTERING),
    /* REGISTERING  */ (1u << ASX_PROVIDER_ACTIVE) | (1u << ASX_PROVIDER_FAILED),
    /* ACTIVE       */ (1u << ASX_PROVIDER_SUSPENDING) | (1u << ASX_PROVIDER_REMOVING),
    /* SUSPENDING   */ (1u << ASX_PROVIDER_SUSPENDED) | (1u << ASX_PROVIDER_FAILED),
    /* SUSPENDED    */ (1u << ASX_PROVIDER_ACTIVE) | (1u << ASX_PROVIDER_REMOVING),
    /* REMOVING     */ (1u << ASX_PROVIDER_REMOVED) | (1u << ASX_PROVIDER_FAILED),
    /* REMOVED      */ 0u,
    /* FAILED       */ (1u << ASX_PROVIDER_UNREGISTERED),
};

asx_status asx_provider_transition_check(asx_provider_state from, asx_provider_state to) {
    if ((int)from < 0 || (int)from >= ASX_PROVIDER_STATE_COUNT || (int)to < 0 ||
        (int)to >= ASX_PROVIDER_STATE_COUNT)
        return ASX_E_INVALID_ARGUMENT;

    if (g_provider_legal[(int)from] & (1u << (int)to)) return ASX_OK;
    return ASX_E_INVALID_TRANSITION;
}

const char *asx_provider_state_str(asx_provider_state state) {
    static const char *names[] = {"unregistered", "registering", "active",  "suspending",
                                  "suspended",    "removing",    "removed", "failed"};
    if ((int)state < 0 || (int)state >= ASX_PROVIDER_STATE_COUNT) return "unknown";
    return names[(int)state];
}

/* ------------------------------------------------------------------ */
/* Hook transition table                                               */
/*                                                                     */
/* UNBOUND  → BINDING                                                  */
/* BINDING  → BOUND | UNBOUND_FAILED                                   */
/* BOUND    → REPLACING | UNBOUND                                      */
/* REPLACING → BOUND | UNBOUND_FAILED                                  */
/* UNBOUND_FAILED → BINDING                                            */
/* ------------------------------------------------------------------ */

static const uint8_t g_hook_legal[ASX_HOOK_STATE_COUNT] = {
    /* UNBOUND       */ (1u << ASX_HOOK_BINDING),
    /* BINDING       */ (1u << ASX_HOOK_BOUND) | (1u << ASX_HOOK_UNBOUND_FAILED),
    /* BOUND         */ (1u << ASX_HOOK_REPLACING) | (1u << ASX_HOOK_UNBOUND),
    /* REPLACING     */ (1u << ASX_HOOK_BOUND) | (1u << ASX_HOOK_UNBOUND_FAILED),
    /* UNBOUND_FAILED*/ (1u << ASX_HOOK_BINDING),
};

asx_status asx_hook_transition_check(asx_hook_state from, asx_hook_state to) {
    if ((int)from < 0 || (int)from >= ASX_HOOK_STATE_COUNT || (int)to < 0 ||
        (int)to >= ASX_HOOK_STATE_COUNT)
        return ASX_E_INVALID_ARGUMENT;

    if (g_hook_legal[(int)from] & (1u << (int)to)) return ASX_OK;
    return ASX_E_INVALID_TRANSITION;
}

const char *asx_hook_state_str(asx_hook_state state) {
    static const char *names[] = {"unbound", "binding", "bound", "replacing", "unbound_failed"};
    if ((int)state < 0 || (int)state >= ASX_HOOK_STATE_COUNT) return "unknown";
    return names[(int)state];
}

/* ------------------------------------------------------------------ */
/* Recoverability                                                      */
/* ------------------------------------------------------------------ */

const char *asx_recoverability_str(asx_recoverability r) {
    switch (r) {
    case ASX_RECOVER_NONE: return "none";
    case ASX_RECOVER_RETRY: return "retry";
    case ASX_RECOVER_DEGRADE: return "degrade";
    case ASX_RECOVER_RECONNECT: return "reconnect";
    default: return "unknown";
    }
}
