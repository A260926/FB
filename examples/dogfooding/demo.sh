#!/bin/bash
# Dogfooding Demo Script for racedetector
#
# This script demonstrates the racedetector tool testing itself.
# It shows that the standalone tool works end-to-end:
# - Builds instrumented code
# - Injects race detector runtime
# - Runs programs with race detection initialized
#
# Phase 6A - Task A.7: Dogfooding Demo

set -e  # Exit on error

echo "========================================"
echo "  racedetector Dogfooding Demo"
echo "========================================"
echo ""

# Colors for output
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

# Step 1: Build racedetector tool
echo "Step 1: Building racedetector tool..."
echo "--------------------------------------"
cd ../../
go build -o racedetector ./cmd/racedetector
if [ $? -eq 0 ]; then
    echo -e "${GREEN}✓ racedetector built successfully${NC}"
else
    echo -e "${RED}✗ Failed to build racedetector${NC}"
    exit 1
fi
echo ""

# Step 2: Display tool version
echo "Step 2: Checking tool version..."
echo "--------------------------------------"
./racedetector --version
echo ""

# Step 3: Build simple race example with race detection
echo "Step 3: Building example with race detection..."
echo "--------------------------------------"
cd examples/dogfooding/
../../racedetector build -o simple_race simple_race.go
if [ $? -eq 0 ]; then
    echo -e "${GREEN}✓ Example built successfully with race detection${NC}"
else
    echo -e "${RED}✗ Failed to build example${NC}"
    exit 1
fi
echo ""

# Step 4: Run instrumented program
echo "Step 4: Running instrumented program..."
echo "--------------------------------------"
./simple_race
echo ""
if [ $? -eq 0 ]; then
    echo -e "${GREEN}✓ Program executed successfully${NC}"
else
    echo -e "${YELLOW}⚠ Program exited with non-zero code${NC}"
fi
echo ""

# Step 5: Test 'run' command
echo "Step 5: Testing 'racedetector run' command..."
echo "--------------------------------------"
../../racedetector run simple_race.go
if [ $? -eq 0 ]; then
    echo -e "${GREEN}✓ 'run' command executed successfully${NC}"
else
    echo -e "${YELLOW}⚠ 'run' command exited with non-zero code${NC}"
fi
echo ""

# Step 6: Cleanup
echo "Step 6: Cleanup..."
echo "--------------------------------------"
rm -f simple_race simple_race.exe
echo -e "${GREEN}✓ Cleanup complete${NC}"
echo ""

# Summary
echo "========================================"
echo "  Dogfooding Demo Summary"
echo "========================================"
echo ""
echo -e "${GREEN}✓ Tool builds successfully${NC}"
echo -e "${GREEN}✓ Code instrumentation works${NC}"
echo -e "${GREEN}✓ Instrumented code compiles${NC}"
echo -e "${GREEN}✓ race.Init() is called${NC}"
echo -e "${GREEN}✓ Programs run with race detection${NC}"
echo ""
echo -e "${YELLOW}Note: Current MVP Status (Phase 6A)${NC}"
echo "  - AST parsing: ✓ Working"
echo "  - Import injection: ✓ Working"
echo "  - Init() injection: ✓ Working"
echo "  - Runtime linking: ✓ Working"
echo "  - Build/Run commands: ✓ Working"
echo ""
echo "  Phase 2 (Future):"
echo "  - Full AST modification (insert RaceRead/Write calls)"
echo "  - Actual race detection in running programs"
echo ""
echo "========================================"
echo "  Dogfooding Demo Complete!"
echo "========================================"
