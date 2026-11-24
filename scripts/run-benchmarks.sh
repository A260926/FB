#!/bin/bash
# Run comprehensive benchmark suite for Pure-Go Race Detector
#
# This script runs all benchmark categories and generates reports:
#   1. Full benchmark suite (all benchmarks)
#   2. Real-world workload benchmarks
#   3. Overhead measurement benchmarks
#   4. Phase 1 vs Phase 2 comparison benchmarks
#
# Usage:
#   ./scripts/run-benchmarks.sh [options]
#
# Options:
#   -q, --quick     Quick mode (benchtime=100ms, less iterations)
#   -f, --full      Full mode (benchtime=3s, more iterations) [default]
#   -c, --compare   Compare with baseline (requires baseline.txt)
#   -h, --help      Show this help message
#
# Output:
#   - benchmarks_full.txt         All benchmarks
#   - benchmarks_workloads.txt    Real-world workloads
#   - benchmarks_overhead.txt     Overhead measurements
#   - benchmarks_comparison.txt   Phase comparisons

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Default options
BENCHTIME="1s"
MODE="full"
COMPARE=false

# Parse command line arguments
while [[ $# -gt 0 ]]; do
  case $1 in
    -q|--quick)
      BENCHTIME="100ms"
      MODE="quick"
      shift
      ;;
    -f|--full)
      BENCHTIME="3s"
      MODE="full"
      shift
      ;;
    -c|--compare)
      COMPARE=true
      shift
      ;;
    -h|--help)
      grep '^#' "$0" | sed 's/^# //; s/^#//'
      exit 0
      ;;
    *)
      echo "Unknown option: $1"
      echo "Use -h or --help for usage information"
      exit 1
      ;;
  esac
done

echo -e "${BLUE}======================================${NC}"
echo -e "${BLUE}  Race Detector - Benchmark Suite${NC}"
echo -e "${BLUE}  Mode: ${MODE} (benchtime=${BENCHTIME})${NC}"
echo -e "${BLUE}======================================${NC}"
echo ""

# Create output directory
mkdir -p benchmarks

# 1. Run all benchmarks
echo -e "${YELLOW}[1/5] Running full benchmark suite...${NC}"
go test -bench=. -benchmem -benchtime=${BENCHTIME} ./internal/race/... > benchmarks/benchmarks_full.txt 2>&1
if [ $? -eq 0 ]; then
  echo -e "${GREEN}✓ Complete (output: benchmarks/benchmarks_full.txt)${NC}"
else
  echo -e "${RED}✗ Failed (check benchmarks/benchmarks_full.txt for errors)${NC}"
fi
echo ""

# 2. Run workload benchmarks
echo -e "${YELLOW}[2/5] Running real-world workload benchmarks...${NC}"
go test -bench=BenchmarkWorkload -benchmem -benchtime=${BENCHTIME} ./internal/race/api > benchmarks/benchmarks_workloads.txt 2>&1
if [ $? -eq 0 ]; then
  echo -e "${GREEN}✓ Complete (output: benchmarks/benchmarks_workloads.txt)${NC}"
else
  echo -e "${RED}✗ Failed (check benchmarks/benchmarks_workloads.txt for errors)${NC}"
fi
echo ""

# 3. Run overhead benchmarks
echo -e "${YELLOW}[3/5] Running overhead measurement benchmarks...${NC}"
go test -bench=BenchmarkOverhead -benchmem -benchtime=${BENCHTIME} ./internal/race/api > benchmarks/benchmarks_overhead.txt 2>&1
if [ $? -eq 0 ]; then
  echo -e "${GREEN}✓ Complete (output: benchmarks/benchmarks_overhead.txt)${NC}"
else
  echo -e "${RED}✗ Failed (check benchmarks/benchmarks_overhead.txt for errors)${NC}"
fi
echo ""

# 4. Run comparison benchmarks
echo -e "${YELLOW}[4/5] Running phase comparison benchmarks...${NC}"
go test -bench=BenchmarkComparison -benchmem -benchtime=${BENCHTIME} ./internal/race/api > benchmarks/benchmarks_comparison.txt 2>&1
if [ $? -eq 0 ]; then
  echo -e "${GREEN}✓ Complete (output: benchmarks/benchmarks_comparison.txt)${NC}"
else
  echo -e "${RED}✗ Failed (check benchmarks/benchmarks_comparison.txt for errors)${NC}"
fi
echo ""

# 5. Generate summary
echo -e "${YELLOW}[5/5] Generating benchmark summary...${NC}"

cat > benchmarks/SUMMARY.txt <<EOF
# Benchmark Summary - $(date)
# Mode: ${MODE} (benchtime=${BENCHTIME})

===========================================
KEY PERFORMANCE METRICS (Phase 2)
===========================================

EOF

# Extract key benchmarks from full results
echo "## GID Extraction Performance" >> benchmarks/SUMMARY.txt
grep "BenchmarkGetGoroutineID_Fast" benchmarks/benchmarks_full.txt | head -1 >> benchmarks/SUMMARY.txt 2>/dev/null || true
grep "BenchmarkGetGoroutineID_Slow" benchmarks/benchmarks_full.txt | head -1 >> benchmarks/SUMMARY.txt 2>/dev/null || true
echo "" >> benchmarks/SUMMARY.txt

echo "## Hot Path Performance" >> benchmarks/SUMMARY.txt
grep "BenchmarkRaceRead-" benchmarks/benchmarks_full.txt | head -1 >> benchmarks/SUMMARY.txt 2>/dev/null || true
grep "BenchmarkRaceWrite-" benchmarks/benchmarks_full.txt | head -1 >> benchmarks/SUMMARY.txt 2>/dev/null || true
echo "" >> benchmarks/SUMMARY.txt

echo "## Context Management" >> benchmarks/SUMMARY.txt
grep "BenchmarkGetCurrentContext_Cached" benchmarks/benchmarks_full.txt | head -1 >> benchmarks/SUMMARY.txt 2>/dev/null || true
grep "BenchmarkGetCurrentContext_FirstCall" benchmarks/benchmarks_full.txt | head -1 >> benchmarks/SUMMARY.txt 2>/dev/null || true
echo "" >> benchmarks/SUMMARY.txt

echo "## Real-World Workloads" >> benchmarks/SUMMARY.txt
grep "BenchmarkWorkload_WebServer/WithDetector" benchmarks/benchmarks_workloads.txt | head -1 >> benchmarks/SUMMARY.txt 2>/dev/null || true
grep "BenchmarkWorkload_WorkerPool/WithDetector" benchmarks/benchmarks_workloads.txt | head -1 >> benchmarks/SUMMARY.txt 2>/dev/null || true
grep "BenchmarkWorkload_DataPipeline/WithDetector" benchmarks/benchmarks_workloads.txt | head -1 >> benchmarks/SUMMARY.txt 2>/dev/null || true
echo "" >> benchmarks/SUMMARY.txt

echo "## Phase Comparison (Speedup)" >> benchmarks/SUMMARY.txt
echo "See benchmarks/benchmarks_comparison.txt for detailed comparisons" >> benchmarks/SUMMARY.txt
echo "" >> benchmarks/SUMMARY.txt

echo -e "${GREEN}✓ Summary generated (output: benchmarks/SUMMARY.txt)${NC}"
echo ""

# Display summary
cat benchmarks/SUMMARY.txt

echo ""
echo -e "${BLUE}======================================${NC}"
echo -e "${BLUE}  Benchmark Results${NC}"
echo -e "${BLUE}======================================${NC}"
echo -e "${GREEN}All benchmarks complete!${NC}"
echo ""
echo "Results saved to:"
echo "  - ${GREEN}benchmarks/benchmarks_full.txt${NC}        (all benchmarks)"
echo "  - ${GREEN}benchmarks/benchmarks_workloads.txt${NC}   (real-world workloads)"
echo "  - ${GREEN}benchmarks/benchmarks_overhead.txt${NC}    (overhead measurements)"
echo "  - ${GREEN}benchmarks/benchmarks_comparison.txt${NC}  (phase comparisons)"
echo "  - ${GREEN}benchmarks/SUMMARY.txt${NC}                (key metrics)"
echo ""

if [ "$COMPARE" = true ]; then
  if [ -f "benchmarks/baseline.txt" ]; then
    echo -e "${YELLOW}Comparing with baseline...${NC}"
    echo ""
    echo "Comparison results:"
    echo "(Baseline comparison not implemented yet - planned for Task 2.3)"
    echo ""
  else
    echo -e "${RED}Warning: baseline.txt not found. Cannot compare.${NC}"
    echo "To create baseline: cp benchmarks/benchmarks_full.txt benchmarks/baseline.txt"
    echo ""
  fi
fi

echo -e "${BLUE}To analyze results:${NC}"
echo "  cat benchmarks/SUMMARY.txt"
echo "  less benchmarks/benchmarks_full.txt"
echo ""
echo -e "${BLUE}To compare with baseline:${NC}"
echo "  ./scripts/run-benchmarks.sh --compare"
echo ""
echo -e "${GREEN}Done!${NC}"
