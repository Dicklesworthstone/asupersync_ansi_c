#!/usr/bin/env bash
# install_smoke.sh — E2E smoke for install script and make install (bd-0toq, bd-jxpg)
#
# Verifies:
#   1. scripts/install.sh --help works
#   2. scripts/install.sh builds and installs to a temp prefix
#   3. make install installs lib + all header subdirectories
#   4. Installed headers compile against ex_quickstart.c
#   5. Compiled quickstart runs successfully
#   6. Idempotent: running install twice doesn't fail
#
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Source shared harness
source "$SCRIPT_DIR/harness.sh"

e2e_init "install-smoke" "E2E-INSTALL"

INSTALL_DIR="$(mktemp -d "${TMPDIR:-/tmp}/asx-install-smoke.XXXXXX")"
cleanup_install() {
    rm -rf "$INSTALL_DIR" 2>/dev/null || true
}
trap cleanup_install EXIT

# -------------------------------------------------------------------
# Scenario 1: install.sh --help
# -------------------------------------------------------------------
if "$PROJECT_ROOT/scripts/install.sh" --help >/dev/null 2>&1; then
    e2e_scenario "install_smoke.help_flag" "" "pass"
else
    e2e_scenario "install_smoke.help_flag" "--help returned non-zero" "fail"
fi

# -------------------------------------------------------------------
# Scenario 2: install.sh --prefix=... succeeds
# -------------------------------------------------------------------
if "$PROJECT_ROOT/scripts/install.sh" --prefix="$INSTALL_DIR" >/dev/null 2>&1; then
    e2e_scenario "install_smoke.script_install" "" "pass"
else
    e2e_scenario "install_smoke.script_install" "install.sh failed" "fail"
    e2e_finish
    exit $?
fi

# -------------------------------------------------------------------
# Scenario 3: libasx.a exists and is non-empty
# -------------------------------------------------------------------
if [ -s "$INSTALL_DIR/lib/libasx.a" ]; then
    e2e_scenario "install_smoke.library_present" "" "pass"
else
    e2e_scenario "install_smoke.library_present" "libasx.a missing or empty" "fail"
fi

# -------------------------------------------------------------------
# Scenario 4: umbrella header exists
# -------------------------------------------------------------------
if [ -f "$INSTALL_DIR/include/asx/asx.h" ]; then
    e2e_scenario "install_smoke.umbrella_header" "" "pass"
else
    e2e_scenario "install_smoke.umbrella_header" "asx.h missing" "fail"
fi

# -------------------------------------------------------------------
# Scenario 5: core subdirectory headers installed
# -------------------------------------------------------------------
if [ -d "$INSTALL_DIR/include/asx/core" ] && [ -d "$INSTALL_DIR/include/asx/runtime" ] && [ -d "$INSTALL_DIR/include/asx/platform" ]; then
    e2e_scenario "install_smoke.header_subdirs" "" "pass"
else
    e2e_scenario "install_smoke.header_subdirs" "header subdirectories missing" "fail"
fi

# -------------------------------------------------------------------
# Scenario 6: compile ex_quickstart.c against installed headers
# -------------------------------------------------------------------
QS_BIN="$INSTALL_DIR/test_quickstart"
if gcc -std=c99 -Wall -Werror \
    -I"$INSTALL_DIR/include" \
    -L"$INSTALL_DIR/lib" \
    -o "$QS_BIN" \
    "$PROJECT_ROOT/examples/ex_quickstart.c" \
    -lasx 2>/dev/null; then
    e2e_scenario "install_smoke.quickstart_compile" "" "pass"
else
    e2e_scenario "install_smoke.quickstart_compile" "compilation failed" "fail"
fi

# -------------------------------------------------------------------
# Scenario 7: run compiled quickstart
# -------------------------------------------------------------------
if [ -x "$QS_BIN" ] && "$QS_BIN" >/dev/null 2>&1; then
    e2e_scenario "install_smoke.quickstart_run" "" "pass"
else
    e2e_scenario "install_smoke.quickstart_run" "quickstart execution failed" "fail"
fi

# -------------------------------------------------------------------
# Scenario 8: idempotent install (run again, should succeed)
# -------------------------------------------------------------------
if "$PROJECT_ROOT/scripts/install.sh" --prefix="$INSTALL_DIR" >/dev/null 2>&1; then
    e2e_scenario "install_smoke.idempotent" "" "pass"
else
    e2e_scenario "install_smoke.idempotent" "second install failed" "fail"
fi

# -------------------------------------------------------------------
# Finish
# -------------------------------------------------------------------

e2e_finish
