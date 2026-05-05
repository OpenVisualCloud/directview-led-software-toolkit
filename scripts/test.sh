#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

# Unit test runner for dvledtx
# Usage: bash scripts/test.sh [--no-coverage]
#
# Runs all cmocka unit tests under tests/ and prints a pass/fail summary.
# When gcovr is available (and --no-coverage is not passed), a per-file
# line coverage report is printed after the test results.

set -e

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TESTS_DIR="$REPO_ROOT/tests"
BUILD_DIR="$TESTS_DIR/build"

COVERAGE=true
if [[ "${1:-}" == "--no-coverage" ]]; then
    COVERAGE=false
fi

# Locate gcovr
GCOVR=""
for candidate in gcovr /home/intel/.local/bin/gcovr; do
    if command -v "$candidate" &>/dev/null 2>&1 || [[ -x "$candidate" ]]; then
        GCOVR="$candidate"
        break
    fi
done

if $COVERAGE && [[ -z "$GCOVR" ]]; then
    echo "Warning: gcovr not found — coverage report will be skipped."
    echo "  Install with: pip install gcovr"
    COVERAGE=false
fi

# ── Build ──────────────────────────────────────────────────────────────────
echo "Setting up test build..."
cd "$TESTS_DIR"
rm -rf "$BUILD_DIR"

MESON_ARGS=()
$COVERAGE && MESON_ARGS+=("-Db_coverage=true")

meson setup build "${MESON_ARGS[@]}" >/dev/null
ninja -C build >/dev/null

# ── Run tests ──────────────────────────────────────────────────────────────
echo ""
echo "Running unit tests..."
echo "──────────────────────────────────────────────────────────────────────"

set +e
meson test -C build --print-errorlogs 2>&1
TEST_EXIT=$?
set -e

# ── Coverage ───────────────────────────────────────────────────────────────
if $COVERAGE; then
    echo ""
    echo "Coverage report (src/ — main.c excluded)"
    echo "──────────────────────────────────────────────────────────────────────"
    "$GCOVR" \
        --root "$REPO_ROOT/src" \
        --filter "$REPO_ROOT/src/" \
        --exclude "$REPO_ROOT/src/main.c" \
        --gcov-ignore-parse-errors=negative_hits.warn_once_per_file \
        "$BUILD_DIR" \
        --txt
fi

exit $TEST_EXIT
