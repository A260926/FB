#!/usr/bin/env bash
# smoke_test.sh — Build + render all TestSuite scenes
# Exit 0 if all pass, exit 1 if any fail

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"
OUTPUT_DIR="$PROJECT_ROOT/output"
TEST_SUITE="$PROJECT_ROOT/TestSuite"
TIMEOUT_SECS=120

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

passed=0
failed=0
failed_list=()

# ── Step 1: Build ──────────────────────────────────────────────
echo "========================================="
echo "  Smoke Test — Build"
echo "========================================="
echo ""

mkdir -p "$BUILD_DIR"

build_start=$(date +%s%N)
if ! (cd "$BUILD_DIR" && cmake .. > /dev/null 2>&1 && make -j"$(nproc)" 2>&1); then
    echo -e "${RED}BUILD FAILED${NC}"
    exit 1
fi
build_end=$(date +%s%N)
build_secs=$(( (build_end - build_start) / 1000000 ))
echo -e "${GREEN}BUILD OK${NC} (${build_secs}ms)"
echo ""

# ── Step 2: Create output dir ─────────────────────────────────
mkdir -p "$OUTPUT_DIR"

# ── Step 3: Render each scene ─────────────────────────────────
echo "========================================="
echo "  Smoke Test — Render Scenes"
echo "========================================="
echo ""

shopt -s nullglob
scenes=("$TEST_SUITE"/*.json)
shopt -u nullglob

if [ ${#scenes[@]} -eq 0 ]; then
    echo -e "${RED}No JSON scenes found in $TEST_SUITE${NC}"
    exit 1
fi

for scene_path in "${scenes[@]}"; do
    scene_file="$(basename "$scene_path")"
    scene_name="${scene_file%.json}"
    output_name="smoke_${scene_name}"
    output_file="$OUTPUT_DIR/${output_name}.ppm"

    # Capture stderr separately for texture error checks
    stderr_file=$(mktemp)

    scene_start=$(date +%s%N)

    # Run from build/ so relative texture paths resolve correctly
    set +e
    (cd "$BUILD_DIR" && timeout "${TIMEOUT_SECS}s" ./ray_tracer "$scene_path" "$OUTPUT_DIR/" "$output_name" > /dev/null 2>"$stderr_file")
    exit_code=$?
    set -e

    scene_end=$(date +%s%N)
    scene_secs=$(( (scene_end - scene_start) / 1000000000 ))
    scene_ms=$(( (scene_end - scene_start) / 1000000 ))

    # Check pass/fail criteria
    fail_reason=""

    if [ $exit_code -ne 0 ]; then
        if [ $exit_code -eq 124 ]; then
            fail_reason="TIMEOUT after ${TIMEOUT_SECS}s"
        else
            fail_reason="EXIT CODE $exit_code"
        fi
    elif [ ! -f "$output_file" ]; then
        fail_reason="OUTPUT FILE MISSING"
    elif [ ! -s "$output_file" ]; then
        fail_reason="OUTPUT FILE EMPTY"
    elif grep -q "Failed to open texture file" "$stderr_file" 2>/dev/null; then
        fail_reason="TEXTURE LOAD FAILURE"
    fi

    rm -f "$stderr_file"

    if [ -z "$fail_reason" ]; then
        echo -e "  ${GREEN}PASS${NC}  $scene_file (${scene_ms}ms)"
        ((++passed)) || true
    else
        echo -e "  ${RED}FAIL${NC}  $scene_file — $fail_reason"
        ((++failed)) || true
        failed_list+=("$scene_file")
    fi
done

# ── Step 4: Summary ───────────────────────────────────────────
echo ""
echo "========================================="
echo "  Summary: $passed passed, $failed failed"
echo "========================================="

if [ $failed -gt 0 ]; then
    echo ""
    echo "Failed scenes:"
    for f in "${failed_list[@]}"; do
        echo -e "  ${RED}$f${NC}"
    done
    exit 1
fi

exit 0
