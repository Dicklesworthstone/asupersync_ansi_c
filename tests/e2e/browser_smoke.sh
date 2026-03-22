#!/usr/bin/env bash
# browser_smoke.sh — e2e family for browser edition smoke tests (bd-1eqo.16.2)
#
# Builds and runs browser edition canonical examples with ASX_PROFILE_BROWSER.
# Each example emits SCENARIO lines parsed by the harness.
#
# The browser profile is compiled in via -DASX_PROFILE_BROWSER, which
# activates the fail-closed capability boundary and browser-specific
# operational parameters.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Source shared harness — override profile to BROWSER
export ASX_E2E_PROFILE="BROWSER"
source "$SCRIPT_DIR/harness.sh"

# -------------------------------------------------------------------
# Init
# -------------------------------------------------------------------

e2e_init "browser-smoke" "E2E-BROWSER-SMOKE"

# -------------------------------------------------------------------
# Browser build helper
#
# Like e2e_build but accepts extra source files as a fourth argument.
# Needed because browser examples link browser_boundary.c which is
# not yet in the static library.
# -------------------------------------------------------------------

_browser_build() {
    local source="$1"
    local output="$2"
    local extra_src="${3:-}"
    local lib="${E2E_BUILD_DIR}/lib/libasx.a"
    local cc="${CC:-gcc}"

    local cflags="-std=c99 -Wall -Wextra -Wpedantic -Werror"
    cflags="$cflags -I${E2E_PROJECT_ROOT}/include"
    cflags="$cflags -I${E2E_PROJECT_ROOT}/tests"
    cflags="$cflags -I${E2E_PROJECT_ROOT}/src"
    cflags="$cflags -DASX_PROFILE_${E2E_PROFILE}"
    cflags="$cflags -DASX_CODEC_$(echo "$E2E_CODEC" | tr '[:lower:]' '[:upper:]')"
    cflags="$cflags -DASX_DETERMINISTIC=1"

    mkdir -p "$(dirname "$output")"
    # shellcheck disable=SC2086
    if ! $cc $cflags -o "$output" "$source" $extra_src "$lib" 2>"${output}.build.log"; then
        echo "[e2e] BUILD FAIL: $source" >&2
        cat "${output}.build.log" >&2
        return 1
    fi
    return 0
}

# -------------------------------------------------------------------
# Build and run each browser example
# -------------------------------------------------------------------

EXAMPLES_DIR="${E2E_PROJECT_ROOT}/examples"
# browser_boundary.c is not in LIB_A; profile_compat.c must be
# recompiled with -DASX_PROFILE_BROWSER to override the CORE version in LIB_A
EXTRA_SRC="${E2E_PROJECT_ROOT}/src/runtime/browser_boundary.c ${E2E_PROJECT_ROOT}/src/runtime/profile_compat.c"

BROWSER_EXAMPLES=(
    ex_browser_boundary.c
    ex_browser_replay.c
)

for src in "${BROWSER_EXAMPLES[@]}"; do
    name="${src%.c}"

    EX_BIN="${E2E_ARTIFACT_DIR}/${name}"

    if ! _browser_build "${EXAMPLES_DIR}/${src}" "$EX_BIN" "$EXTRA_SRC"; then
        e2e_scenario "${name}.build" "compilation failed" "fail"
        continue
    fi
    e2e_scenario "${name}.build" "" "pass"

    e2e_run_binary "$EX_BIN" "${E2E_ARTIFACT_DIR}/${name}.stderr" "$name"
done

# -------------------------------------------------------------------
# Finish
# -------------------------------------------------------------------

set +e
e2e_finish
FINISH_RC=$?
set -e

exit $FINISH_RC
