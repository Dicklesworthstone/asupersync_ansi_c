#!/bin/sh
# install.sh — Install libasx.a and public headers from source
#
# Usage:
#   # From inside the repository:
#   ./scripts/install.sh [--prefix=/usr/local]
#
#   # Via curl (clones, builds, installs):
#   curl -fsSL https://raw.githubusercontent.com/Dicklesworthstone/asupersync_ansi_c/main/scripts/install.sh | sh
#
# Options:
#   --prefix=PATH   Installation prefix (default: /usr/local)
#   --help          Print this help and exit
#
# Installs:
#   PREFIX/lib/libasx.a          Static library
#   PREFIX/include/asx/          Public headers (125 files, 38 families)
#
# Requirements:
#   - C compiler (cc, gcc, or clang)
#   - make
#   - git (only for curl|sh mode, to clone the repository)
#
# SPDX-License-Identifier: MIT

set -e

# -------------------------------------------------------------------
# Defaults
# -------------------------------------------------------------------

PREFIX="/usr/local"
REPO_URL="https://github.com/Dicklesworthstone/asupersync_ansi_c.git"
CLONE_DIR=""
CLEANUP_CLONE=0

# -------------------------------------------------------------------
# Helpers
# -------------------------------------------------------------------

info()  { printf "[asx] %s\n" "$*"; }
warn()  { printf "[asx] WARNING: %s\n" "$*" >&2; }
die()   { printf "[asx] ERROR: %s\n" "$*" >&2; exit 1; }

usage() {
    cat <<'USAGE'
Usage: install.sh [OPTIONS]

Install the asx C library (libasx.a + public headers) from source.

Options:
  --prefix=PATH   Installation prefix (default: /usr/local)
  --help          Print this help and exit

Examples:
  ./scripts/install.sh                        # Install to /usr/local
  ./scripts/install.sh --prefix=$HOME/.local  # Install to ~/.local
  sudo ./scripts/install.sh                   # Install system-wide
USAGE
    exit 0
}

cleanup() {
    if [ "$CLEANUP_CLONE" = "1" ] && [ -n "$CLONE_DIR" ] && [ -d "$CLONE_DIR" ]; then
        info "Cleaning up temporary clone: $CLONE_DIR"
        rm -rf "$CLONE_DIR"
    fi
}

trap cleanup EXIT INT TERM

check_tool() {
    if ! command -v "$1" >/dev/null 2>&1; then
        die "Required tool '$1' not found. Please install it and try again."
    fi
}

find_cc() {
    for cc_candidate in cc gcc clang; do
        if command -v "$cc_candidate" >/dev/null 2>&1; then
            printf "%s" "$cc_candidate"
            return 0
        fi
    done
    die "No C compiler found. Install gcc or clang and try again."
}

# -------------------------------------------------------------------
# Parse arguments
# -------------------------------------------------------------------

for arg in "$@"; do
    case "$arg" in
        --prefix=*)
            PREFIX="${arg#--prefix=}"
            ;;
        --help|-h)
            usage
            ;;
        *)
            die "Unknown option: $arg (try --help)"
            ;;
    esac
done

# -------------------------------------------------------------------
# Detect environment
# -------------------------------------------------------------------

OS="$(uname -s)"
ARCH="$(uname -m)"
info "Detected: OS=$OS ARCH=$ARCH"
info "Install prefix: $PREFIX"

# Check for required tools
check_tool make
CC="$(find_cc)"
info "Using C compiler: $CC"

# -------------------------------------------------------------------
# Find or clone the repository
# -------------------------------------------------------------------

if [ -f "Makefile" ] && [ -d "include/asx" ] && [ -d "src" ]; then
    # We're inside the repository already
    REPO_DIR="$(pwd)"
    info "Building from current directory: $REPO_DIR"
else
    # Need to clone
    check_tool git
    CLONE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/asx-install.XXXXXX")"
    CLEANUP_CLONE=1
    info "Cloning repository to $CLONE_DIR ..."
    git clone --depth 1 "$REPO_URL" "$CLONE_DIR"
    REPO_DIR="$CLONE_DIR"
    info "Clone complete."
fi

# -------------------------------------------------------------------
# Build
# -------------------------------------------------------------------

info "Building libasx.a (this may take a moment) ..."
(cd "$REPO_DIR" && CC="$CC" make release 2>&1) || die "Build failed. Check compiler output above."
info "Build complete."

# -------------------------------------------------------------------
# Install
# -------------------------------------------------------------------

info "Installing to $PREFIX ..."

# Create directories
install -d "$PREFIX/lib"
install -d "$PREFIX/include"

# Install library
install -m 644 "$REPO_DIR/build/lib/libasx.a" "$PREFIX/lib/"

# Install headers (recursive copy preserving directory structure)
cp -R "$REPO_DIR/include/asx" "$PREFIX/include/"

info "Installation complete!"
info ""
info "  Library: $PREFIX/lib/libasx.a"
info "  Headers: $PREFIX/include/asx/"
info ""
info "To use in your project:"
info "  gcc -I$PREFIX/include -L$PREFIX/lib -o myapp myapp.c -lasx"
