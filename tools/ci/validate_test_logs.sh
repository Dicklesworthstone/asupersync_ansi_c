#!/usr/bin/env bash
# validate_test_logs.sh — CI validator for structured test log artifacts (bd-1md.11)
#
# Validates that all test log JSONL files in the given directory conform
# to schemas/test_log.schema.json by checking required fields, status
# values, and completeness. Fails CI if mandatory fields are missing
# or artifacts are malformed.
#
# Usage: tools/ci/validate_test_logs.sh [--log-dir <dir>] [--strict]
#
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
LOG_DIR="${PROJECT_ROOT}/build/test-logs"
STRICT=0
FAIL_COUNT=0
PASS_COUNT=0
SKIP_COUNT=0

# Parse arguments
while [ $# -gt 0 ]; do
    case "$1" in
        --log-dir)  LOG_DIR="$2"; shift 2 ;;
        --strict)   STRICT=1; shift ;;
        *)          echo "Unknown argument: $1" >&2; exit 1 ;;
    esac
done

echo "[asx] validate-test-logs: checking ${LOG_DIR}"

# Check if directory exists
if [ ! -d "$LOG_DIR" ]; then
    if [ "$STRICT" = "1" ]; then
        echo "[asx] validate-test-logs: FAIL — log directory does not exist: ${LOG_DIR}"
        exit 1
    else
        echo "[asx] validate-test-logs: SKIP — log directory does not exist"
        exit 0
    fi
fi

# Find all JSONL log files
LOG_FILES=$(find "$LOG_DIR" -name '*.jsonl' -type f 2>/dev/null || true)

if [ -z "$LOG_FILES" ]; then
    if [ "$STRICT" = "1" ]; then
        echo "[asx] validate-test-logs: FAIL — no log files found in ${LOG_DIR}"
        exit 1
    else
        echo "[asx] validate-test-logs: SKIP — no log files found"
        exit 0
    fi
fi

# Required fields per schema
REQUIRED_FIELDS='ts run_id layer subsystem status'

validate_record() {
    local file="$1"
    local line_no="$2"
    local record="$3"
    local errors=""

    for field in $REQUIRED_FIELDS; do
        # Check that the field exists and is non-null
        if ! echo "$record" | grep -q "\"${field}\""; then
            errors="${errors}  missing required field: ${field}\n"
        fi
    done

    # Validate status enum
    local status
    status=$(echo "$record" | sed -n 's/.*"status"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')
    if [ -n "$status" ]; then
        case "$status" in
            pass|fail|skip|error) ;;
            *) errors="${errors}  invalid status value: ${status}\n" ;;
        esac
    fi

    # Validate layer enum
    local layer
    layer=$(echo "$record" | sed -n 's/.*"layer"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')
    if [ -n "$layer" ]; then
        case "$layer" in
            unit|invariant|conformance|e2e|bench|fuzz) ;;
            *) errors="${errors}  invalid layer value: ${layer}\n" ;;
        esac
    fi

    if [ -n "$errors" ]; then
        printf "[asx] validate-test-logs: FAIL in %s line %d:\n%b" \
               "$(basename "$file")" "$line_no" "$errors" >&2
        FAIL_COUNT=$((FAIL_COUNT + 1))
    else
        PASS_COUNT=$((PASS_COUNT + 1))
    fi
}

# Process each file
FILE_COUNT=0
for log_file in $LOG_FILES; do
    FILE_COUNT=$((FILE_COUNT + 1))
    LINE_NO=0
    # A suite that opened its log and recorded nothing is not evidence: the
    # harness hooks were compiled out of every suite that way, and this
    # validator passed the empty files.
    if ! grep -q '^{' "$log_file"; then
        if [ "$STRICT" = "1" ]; then
            echo "[asx] validate-test-logs: FAIL — $(basename "$log_file") has no records" >&2
            FAIL_COUNT=$((FAIL_COUNT + 1))
        else
            echo "[asx] validate-test-logs: WARN — $(basename "$log_file") has no records" >&2
        fi
    fi
    while IFS= read -r line || [ -n "$line" ]; do
        LINE_NO=$((LINE_NO + 1))
        # Skip empty lines
        [ -z "$line" ] && continue
        # Check it looks like JSON
        case "$line" in
            \{*) validate_record "$log_file" "$LINE_NO" "$line" ;;
            *)
                echo "[asx] validate-test-logs: WARN — non-JSON line in $(basename "$log_file"):${LINE_NO}" >&2
                SKIP_COUNT=$((SKIP_COUNT + 1))
                ;;
        esac
    done < "$log_file"
done

echo "[asx] validate-test-logs: ${FILE_COUNT} files, ${PASS_COUNT} records valid, ${FAIL_COUNT} invalid, ${SKIP_COUNT} skipped"

if [ "$FAIL_COUNT" -gt 0 ]; then
    echo "[asx] validate-test-logs: FAIL"
    exit 1
fi

echo "[asx] validate-test-logs: PASS"
exit 0
