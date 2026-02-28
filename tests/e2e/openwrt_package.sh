#!/usr/bin/env bash
# openwrt_package.sh — package/install/upgrade/rollback sanity lane (bd-j4m.7)
#
# Validates OpenWrt packaging artifacts are buildable, deterministic, and
# include operator-facing install/upgrade/rollback metadata.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/harness.sh"

e2e_init "openwrt-package" "E2E-OPENWRT-PACKAGE"

RECIPE_FILE="${E2E_PROJECT_ROOT}/packaging/openwrt/asx/Makefile"
INIT_FILE="${E2E_PROJECT_ROOT}/packaging/openwrt/asx/files/asx.init"
BUILD_SCRIPT="${E2E_PROJECT_ROOT}/tools/packaging/openwrt/build_package.sh"

if [ ! -f "${RECIPE_FILE}" ]; then
    e2e_scenario "openwrt.recipe_exists" "missing packaging/openwrt/asx/Makefile" "fail"
    e2e_finish
    exit $?
fi
e2e_scenario "openwrt.recipe_exists" "" "pass"

if [ ! -f "${INIT_FILE}" ]; then
    e2e_scenario "openwrt.init_exists" "missing packaging/openwrt/asx/files/asx.init" "fail"
    e2e_finish
    exit $?
fi
e2e_scenario "openwrt.init_exists" "" "pass"

if [ ! -x "${BUILD_SCRIPT}" ]; then
    e2e_scenario "openwrt.builder_executable" "builder script not executable" "fail"
    e2e_finish
    exit $?
fi
e2e_scenario "openwrt.builder_executable" "" "pass"

OUT_A="${E2E_ARTIFACT_DIR}/pkg-a"
OUT_B="${E2E_ARTIFACT_DIR}/pkg-b"
EPOCH=1700000000

if ! "${BUILD_SCRIPT}" --output-dir "${OUT_A}" --source-date-epoch "${EPOCH}" > "${OUT_A}.log" 2>&1; then
    e2e_scenario "openwrt.package_build" "package build failed (see ${OUT_A}.log)" "fail"
    e2e_finish
    exit $?
fi
e2e_scenario "openwrt.package_build" "" "pass"

if ! "${BUILD_SCRIPT}" --skip-build --output-dir "${OUT_B}" --source-date-epoch "${EPOCH}" > "${OUT_B}.log" 2>&1; then
    e2e_scenario "openwrt.package_rebuild" "package rebuild failed (see ${OUT_B}.log)" "fail"
    e2e_finish
    exit $?
fi
e2e_scenario "openwrt.package_rebuild" "" "pass"

SHA_A_FILE="$(ls "${OUT_A}"/artifacts/*.ipk.sha256 | head -1)"
SHA_B_FILE="$(ls "${OUT_B}"/artifacts/*.ipk.sha256 | head -1)"
SHA_A="$(cut -d' ' -f1 "${SHA_A_FILE}")"
SHA_B="$(cut -d' ' -f1 "${SHA_B_FILE}")"

if [ -z "${SHA_A}" ] || [ -z "${SHA_B}" ]; then
    e2e_scenario "openwrt.sha_present" "missing sha256 output from package build" "fail"
    e2e_finish
    exit $?
fi
e2e_scenario "openwrt.sha_present" "" "pass"

if [ "${SHA_A}" != "${SHA_B}" ]; then
    e2e_scenario "openwrt.reproducible_ipk" "ipk checksum drift between deterministic builds" "fail"
    e2e_finish
    exit $?
fi
e2e_scenario "openwrt.reproducible_ipk" "" "pass" "sha256:${SHA_A}"

MANIFEST="${OUT_A}/artifacts/openwrt-package-manifest.json"
if ! grep -q '"install_command"' "${MANIFEST}" \
    || ! grep -q '"upgrade_command"' "${MANIFEST}" \
    || ! grep -q '"rollback_command"' "${MANIFEST}"; then
    e2e_scenario "openwrt.manifest_commands" "manifest missing install/upgrade/rollback commands" "fail"
    e2e_finish
    exit $?
fi
e2e_scenario "openwrt.manifest_commands" "" "pass"

set +e
e2e_finish
FINISH_RC=$?
set -e

exit "${FINISH_RC}"
