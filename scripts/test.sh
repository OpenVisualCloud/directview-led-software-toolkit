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

# ── Detect a live X display for opt-in screen-capture (x11grab) tests ───────
# The screen-capture success tests run only when DVLED_TEST_DISPLAY is set to
# an x11grab source string ("<display>+<x>,<y>"); otherwise they self-skip.
if [[ -z "${DVLED_TEST_DISPLAY:-}" ]] && command -v xdpyinfo &>/dev/null; then
    for disp in "${DISPLAY:-}" ":99" ":0"; do
        [[ -z "$disp" ]] && continue
        if DISPLAY="$disp" xdpyinfo &>/dev/null; then
            # Append screen ".0" only when the display has no screen already
            # (e.g. ":0" → ":0.0", but leave ":0.0" untouched to avoid ":0.0.0").
            disp_tail="${disp##*:}"
            if [[ "$disp_tail" == *.* ]]; then
                export DVLED_TEST_DISPLAY="${disp}+0,0"
            else
                export DVLED_TEST_DISPLAY="${disp}.0+0,0"
            fi
            echo "Live X display detected on $disp — enabling screen-capture tests."
            break
        fi
    done
fi

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
