#!/bin/bash
# cleanup_temp.sh - Clean up temporary test data for GDB Index Creator
#
# This script removes:
#   - test_data/temp/ directory and all contents
#   - Temporary test GDBs in build directory
#   - Coverage build artifacts (optional)
#
# Usage: bash tools/ci/cleanup_temp.sh [--all]
#   --all    Also remove coverage build directory

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

echo "=== GDB Index Creator Cleanup ==="
echo "Project root: $PROJECT_ROOT"
echo ""

# Default: only clean temp test data
CLEAN_ALL=false
if [[ "${1:-}" == "--all" ]]; then
    CLEAN_ALL=true
fi

# 1. Clean test_data/temp/ directory
TEMP_DIR="$PROJECT_ROOT/test_data/temp"
if [[ -d "$TEMP_DIR" ]]; then
    echo "Removing $TEMP_DIR ..."
    rm -rf "$TEMP_DIR"
    echo "  Done."
else
    echo "No temp directory found at $TEMP_DIR"
fi

# 2. Clean temporary test GDBs in build directory
BUILD_DIR="$PROJECT_ROOT/build"
if [[ -d "$BUILD_DIR" ]]; then
    echo ""
    echo "Cleaning temporary test GDBs in build directory..."

    # Remove test GDB directories
    find "$BUILD_DIR" -type d -name "*test*.gdb" -exec rm -rf {} + 2>/dev/null || true
    find "$BUILD_DIR" -type d -name "*temp*.gdb" -exec rm -rf {} + 2>/dev/null || true

    # Remove index_test_* directories (from test_index_creator.cpp)
    find /tmp -maxdepth 1 -type d -name "index_test_*" -exec rm -rf {} + 2>/dev/null || true

    echo "  Done."
fi

# 3. Optional: Clean coverage build directory
if [[ "$CLEAN_ALL" == true ]]; then
    COVERAGE_DIR="$PROJECT_ROOT/build_coverage"
    if [[ -d "$COVERAGE_DIR" ]]; then
        echo ""
        echo "Removing coverage build directory $COVERAGE_DIR ..."
        rm -rf "$COVERAGE_DIR"
        echo "  Done."
    fi
fi

echo ""
echo "=== Cleanup Complete ==="
