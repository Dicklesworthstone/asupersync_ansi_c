#!/usr/bin/env bash
# build_package.sh — reproducible OpenWrt package artifact builder (bd-j4m.7)
#
# Produces:
#   - asx_<version>-<release>_all.ipk
#   - matching sha256 file
#   - machine-readable manifest with install/upgrade/rollback commands
#
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../../" && pwd)"

OUT_DIR="${REPO_ROOT}/build/openwrt-package"
# The router build gets its own build dir so it never overwrites the
# caller's build/lib/libasx.a (which may be a different profile).
PKG_BUILD_DIR="build/pkg-embedded-router"
VERSION="0.1.0"
RELEASE="1"
SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-1700000000}"
DRY_RUN=0
SKIP_BUILD=0
ASX_USE_RCH="${ASX_USE_RCH:-auto}"

usage() {
    cat <<'USAGE'
Usage: build_package.sh [options]

Options:
  --output-dir <dir>          Output directory (default: build/openwrt-package)
  --version <ver>             Package version (default: 0.1.0)
  --release <n>               Package release number (default: 1)
  --source-date-epoch <secs>  Reproducible timestamp seed
  --skip-build                Reuse existing build/pkg-embedded-router/lib/libasx.a
  --dry-run                   Print plan and exit
  -h, --help                  Show this help

Environment:
  ASX_USE_RCH=1|0|auto        Offload build step via rch (default: auto; auto
                              builds locally when already running inside an
                              rch job, to avoid a nested offload)
USAGE
}

while [ $# -gt 0 ]; do
    case "$1" in
        --output-dir) OUT_DIR="$2"; shift 2 ;;
        --version) VERSION="$2"; shift 2 ;;
        --release) RELEASE="$2"; shift 2 ;;
        --source-date-epoch) SOURCE_DATE_EPOCH="$2"; shift 2 ;;
        --skip-build) SKIP_BUILD=1; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage; exit 2 ;;
    esac
done

run_build_cmd() {
    if [ "${ASX_USE_RCH}" = "1" ]; then
        if ! command -v rch >/dev/null 2>&1; then
            echo "ASX_USE_RCH=1 but rch is not available on PATH" >&2
            return 1
        fi
        rch exec -- "$@"
        return $?
    fi

    if [ "${ASX_USE_RCH}" = "0" ]; then
        "$@"
        return $?
    fi

    # Already inside an rch job (the worker environment sets
    # RCH_CARGO_WRAPPER_BYPASS; callers may set ASX_IN_RCH): offloading again
    # from the worker is slow (~12 min observed for this step) and pointless.
    if [ -n "${RCH_CARGO_WRAPPER_BYPASS:-}" ] || [ -n "${ASX_IN_RCH:-}" ]; then
        "$@"
        return $?
    fi

    if command -v rch >/dev/null 2>&1; then
        rch exec -- "$@"
    else
        "$@"
    fi
}

PKG_BASENAME="asx_${VERSION}-${RELEASE}_all"
WORK_DIR="${OUT_DIR}/work"
STAGE_DIR="${WORK_DIR}/data"
CONTROL_DIR="${WORK_DIR}/control"
ARTIFACTS_DIR="${OUT_DIR}/artifacts"
IPK_PATH="${ARTIFACTS_DIR}/${PKG_BASENAME}.ipk"
SHA_PATH="${IPK_PATH}.sha256"
MANIFEST_PATH="${ARTIFACTS_DIR}/openwrt-package-manifest.json"

if [ "$DRY_RUN" = "1" ]; then
    cat <<EOF
[asx-openwrt-package] dry-run
repo_root=${REPO_ROOT}
output_dir=${OUT_DIR}
version=${VERSION}
release=${RELEASE}
source_date_epoch=${SOURCE_DATE_EPOCH}
skip_build=${SKIP_BUILD}
asx_use_rch=${ASX_USE_RCH}
ipk=${IPK_PATH}
manifest=${MANIFEST_PATH}
EOF
    exit 0
fi

mkdir -p "${WORK_DIR}" "${ARTIFACTS_DIR}"
rm -rf "${STAGE_DIR}" "${CONTROL_DIR}"
mkdir -p "${STAGE_DIR}" "${CONTROL_DIR}"

if [ "$SKIP_BUILD" != "1" ]; then
    run_build_cmd make -C "${REPO_ROOT}" build PROFILE=EMBEDDED_ROUTER CODEC=BIN DETERMINISTIC=1 \
        BUILD_DIR="${PKG_BUILD_DIR}"
fi

LIB_PATH="${REPO_ROOT}/${PKG_BUILD_DIR}/lib/libasx.a"
if [ ! -f "${LIB_PATH}" ]; then
    echo "missing ${LIB_PATH}; run build first or remove --skip-build" >&2
    exit 1
fi

INIT_SRC="${REPO_ROOT}/packaging/openwrt/asx/files/asx.init"
if [ ! -f "${INIT_SRC}" ]; then
    echo "missing ${INIT_SRC}" >&2
    exit 1
fi

mkdir -p "${STAGE_DIR}/usr/lib" "${STAGE_DIR}/usr/include" "${STAGE_DIR}/etc/init.d"
cp "${LIB_PATH}" "${STAGE_DIR}/usr/lib/libasx.a"
cp -R "${REPO_ROOT}/include/asx" "${STAGE_DIR}/usr/include/"
install -m 0755 "${INIT_SRC}" "${STAGE_DIR}/etc/init.d/asx"

cat > "${CONTROL_DIR}/control" <<EOF
Package: asx
Version: ${VERSION}-${RELEASE}
Architecture: all
Maintainer: asx maintainers
Section: libs
Priority: optional
Description: asupersync ANSI C runtime (deterministic embedded profile package)
EOF

echo "2.0" > "${WORK_DIR}/debian-binary"

TAR_OPTS=(
    --sort=name
    --owner=0
    --group=0
    --numeric-owner
    --mtime="@${SOURCE_DATE_EPOCH}"
)

tar "${TAR_OPTS[@]}" -czf "${WORK_DIR}/control.tar.gz" -C "${CONTROL_DIR}" .
tar "${TAR_OPTS[@]}" -czf "${WORK_DIR}/data.tar.gz" -C "${STAGE_DIR}" .

ar rcsD "${IPK_PATH}" \
    "${WORK_DIR}/debian-binary" \
    "${WORK_DIR}/control.tar.gz" \
    "${WORK_DIR}/data.tar.gz"

sha256sum "${IPK_PATH}" > "${SHA_PATH}"
SHA256="$(cut -d' ' -f1 "${SHA_PATH}")"

cat > "${MANIFEST_PATH}" <<EOF
{
  "package": "asx",
  "version": "${VERSION}",
  "release": "${RELEASE}",
  "source_date_epoch": ${SOURCE_DATE_EPOCH},
  "ipk": "$(basename "${IPK_PATH}")",
  "sha256": "${SHA256}",
  "install_command": "opkg install $(basename "${IPK_PATH}")",
  "upgrade_command": "opkg upgrade $(basename "${IPK_PATH}")",
  "rollback_command": "opkg remove asx && opkg install <previous-asx.ipk>",
  "startup_sanity_command": "/etc/init.d/asx enable && /etc/init.d/asx restart"
}
EOF

cat <<EOF
[asx-openwrt-package] created
ipk=${IPK_PATH}
sha256=${SHA_PATH}
manifest=${MANIFEST_PATH}
EOF
