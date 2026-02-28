#!/usr/bin/env bash
# =============================================================================
# test_checkpoint_lint.sh — Tests for the checkpoint-coverage lint gate
#
# Verifies true positives, false positives, bounded detection, and waiver handling.
#
# SPDX-License-Identifier: MIT
# =============================================================================

set -euo pipefail

SCRIPT="./tools/ci/check_checkpoint_coverage.sh"
TMPDIR=$(mktemp -d)
PASS=0
FAIL=0

cleanup() {
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

assert_pass() {
    local name="$1"
    shift
    if "$@" > /dev/null 2>&1; then
        echo "  PASS: $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $name (expected pass, got violation)"
        FAIL=$((FAIL + 1))
    fi
}

assert_fail() {
    local name="$1"
    shift
    if "$@" > /dev/null 2>&1; then
        echo "  FAIL: $name (expected violation, got pass)"
        FAIL=$((FAIL + 1))
    else
        echo "  PASS: $name"
        PASS=$((PASS + 1))
    fi
}

echo "=== test_checkpoint_lint ==="

# ---------------------------------------------------------------------------
# Test 1: True positive — long-running loop without checkpoint or waiver
# ---------------------------------------------------------------------------
mkdir -p "$TMPDIR/src1"
cat > "$TMPDIR/src1/unchecked.c" <<'EOF'
#include <stdint.h>
void scan_all(uint32_t count) {
    uint32_t i;
    for (i = 0; i < count; i++) {
        /* process */
    }
}
EOF
assert_fail "true_positive_unbounded_loop" bash "$SCRIPT" "$TMPDIR/src1"

# ---------------------------------------------------------------------------
# Test 2: True negative — loop with asx_checkpoint
# ---------------------------------------------------------------------------
mkdir -p "$TMPDIR/src2"
cat > "$TMPDIR/src2/checked.c" <<'EOF'
#include <stdint.h>
void poll_all(uint32_t count) {
    uint32_t i;
    for (i = 0; i < count; i++) {
        asx_checkpoint(self, &cr);
    }
}
EOF
assert_pass "checkpoint_present_passes" bash "$SCRIPT" "$TMPDIR/src2"

# ---------------------------------------------------------------------------
# Test 3: Per-loop waiver via ASX_CHECKPOINT_WAIVER
# ---------------------------------------------------------------------------
mkdir -p "$TMPDIR/src3"
cat > "$TMPDIR/src3/waived.c" <<'EOF'
#include <stdint.h>
void kernel_loop(uint32_t count) {
    uint32_t i;
    for (i = 0; i < count; i++) {
        ASX_CHECKPOINT_WAIVER("bounded by external contract");
    }
}
EOF
assert_pass "per_loop_waiver_passes" bash "$SCRIPT" "$TMPDIR/src3"

# ---------------------------------------------------------------------------
# Test 4: File-level waiver via ASX_CHECKPOINT_WAIVER_FILE
# ---------------------------------------------------------------------------
mkdir -p "$TMPDIR/src4"
cat > "$TMPDIR/src4/file_waived.c" <<'EOF'
/*
 * ASX_CHECKPOINT_WAIVER_FILE("all loops in this file are codec utilities")
 */
#include <stdint.h>
void parse(uint32_t len) {
    uint32_t i;
    for (i = 0; i < len; i++) {
        /* parse byte */
    }
}
EOF
assert_pass "file_level_waiver_passes" bash "$SCRIPT" "$TMPDIR/src4"

# ---------------------------------------------------------------------------
# Test 5: Bounded loop (ASX_MAX_*) is not flagged
# ---------------------------------------------------------------------------
mkdir -p "$TMPDIR/src5"
cat > "$TMPDIR/src5/bounded.c" <<'EOF'
#include <stdint.h>
#define ASX_MAX_TASKS 64
void init(void) {
    uint32_t i;
    for (i = 0; i < ASX_MAX_TASKS; i++) {
        /* init slot */
    }
}
EOF
assert_pass "bounded_ASX_MAX_not_flagged" bash "$SCRIPT" "$TMPDIR/src5"

# ---------------------------------------------------------------------------
# Test 6: Bounded loop (sizeof) is not flagged
# ---------------------------------------------------------------------------
mkdir -p "$TMPDIR/src6"
cat > "$TMPDIR/src6/sizeof_bounded.c" <<'EOF'
#include <stdint.h>
void hash(const void *data) {
    uint32_t i;
    for (i = 0; i < sizeof(uint64_t); i++) {
        /* mix byte */
    }
}
EOF
assert_pass "bounded_sizeof_not_flagged" bash "$SCRIPT" "$TMPDIR/src6"

# ---------------------------------------------------------------------------
# Test 7: for(;;) infinite loop without waiver is flagged
# ---------------------------------------------------------------------------
mkdir -p "$TMPDIR/src7"
cat > "$TMPDIR/src7/infinite.c" <<'EOF'
void event_loop(void) {
    for (;;) {
        /* spin */
    }
}
EOF
assert_fail "infinite_loop_without_waiver_flagged" bash "$SCRIPT" "$TMPDIR/src7"

# ---------------------------------------------------------------------------
# Test 8: for(;;) infinite loop with waiver passes
# ---------------------------------------------------------------------------
mkdir -p "$TMPDIR/src8"
cat > "$TMPDIR/src8/infinite_waived.c" <<'EOF'
void event_loop(void) {
    for (;;) {
        ASX_CHECKPOINT_WAIVER("kernel event loop with budget exit");
        break;
    }
}
EOF
assert_pass "infinite_loop_with_waiver_passes" bash "$SCRIPT" "$TMPDIR/src8"

# ---------------------------------------------------------------------------
# Test 9: asx_is_cancelled counts as checkpoint coverage
# ---------------------------------------------------------------------------
mkdir -p "$TMPDIR/src9"
cat > "$TMPDIR/src9/is_cancelled.c" <<'EOF'
#include <stdint.h>
void work(uint32_t count) {
    uint32_t i;
    for (i = 0; i < count; i++) {
        if (asx_is_cancelled(self)) break;
    }
}
EOF
assert_pass "is_cancelled_counts_as_checkpoint" bash "$SCRIPT" "$TMPDIR/src9"

# ---------------------------------------------------------------------------
# Test 10: JSON output mode works
# ---------------------------------------------------------------------------
mkdir -p "$TMPDIR/src10"
cat > "$TMPDIR/src10/clean.c" <<'EOF'
#define ASX_MAX_X 8
void f(void) {
    int i;
    for (i = 0; i < ASX_MAX_X; i++) {}
}
EOF
json_output=$(bash "$SCRIPT" --json "$TMPDIR/src10" 2>&1)
if echo "$json_output" | grep -q '"pass": true'; then
    echo "  PASS: json_output_mode"
    PASS=$((PASS + 1))
else
    echo "  FAIL: json_output_mode"
    FAIL=$((FAIL + 1))
fi

# ---------------------------------------------------------------------------
# Test 11: Empty directory passes (no loops to check)
# ---------------------------------------------------------------------------
mkdir -p "$TMPDIR/src11"
cat > "$TMPDIR/src11/empty.c" <<'EOF'
void no_loops(void) {
    return;
}
EOF
assert_pass "no_loops_passes" bash "$SCRIPT" "$TMPDIR/src11"

# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------
echo ""
echo "$((PASS + FAIL)) tests: $PASS passed, $FAIL failed"
if [ $FAIL -gt 0 ]; then
    exit 1
fi
exit 0
