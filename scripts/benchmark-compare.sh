#!/bin/bash
# benchmark-compare.sh - Automated benchmark comparison using benchstat
#
# Usage:
#   ./scripts/benchmark-compare.sh baseline    # Capture baseline benchmarks
#   ./scripts/benchmark-compare.sh current     # Run current benchmarks
#   ./scripts/benchmark-compare.sh compare     # Compare baseline vs current
#   ./scripts/benchmark-compare.sh all         # Run all steps
#
# Requirements:
#   go install golang.org/x/perf/cmd/benchstat@latest

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BENCH_DIR="$PROJECT_DIR/benchmarks"

# Benchmark configuration
BENCH_COUNT=5          # Number of iterations for statistical significance
BENCH_TIME="1s"        # Duration per benchmark
BENCH_PACKAGES="./internal/race/detector/... ./internal/race/shadowmem/... ./internal/race/vectorclock/..."

# Output files
BASELINE_FILE="$BENCH_DIR/baseline.txt"
CURRENT_FILE="$BENCH_DIR/current.txt"
COMPARISON_FILE="$BENCH_DIR/comparison.txt"

mkdir -p "$BENCH_DIR"

run_benchmarks() {
    local output_file="$1"
    local label="$2"

    echo "Running benchmarks ($label)..."
    echo "  Count: $BENCH_COUNT iterations"
    echo "  Time: $BENCH_TIME per benchmark"
    echo "  Output: $output_file"
    echo ""

    cd "$PROJECT_DIR"

    # Run benchmarks with -count for statistical significance
    # Filter to only benchmark lines (start with Benchmark)
    go test -bench=. -benchmem -benchtime="$BENCH_TIME" -count="$BENCH_COUNT" \
        $BENCH_PACKAGES 2>&1 | grep -E "^(Benchmark|goos|goarch|pkg|cpu)" > "$output_file"

    echo ""
    echo "Benchmarks saved to: $output_file"
    echo "Lines: $(wc -l < "$output_file")"
}

compare_benchmarks() {
    if [ ! -f "$BASELINE_FILE" ]; then
        echo "Error: Baseline file not found: $BASELINE_FILE"
        echo "Run: $0 baseline"
        exit 1
    fi

    if [ ! -f "$CURRENT_FILE" ]; then
        echo "Error: Current file not found: $CURRENT_FILE"
        echo "Run: $0 current"
        exit 1
    fi

    echo "Comparing benchmarks..."
    echo "  Baseline: $BASELINE_FILE"
    echo "  Current:  $CURRENT_FILE"
    echo ""

    # Run benchstat comparison
    benchstat "$BASELINE_FILE" "$CURRENT_FILE" | tee "$COMPARISON_FILE"

    echo ""
    echo "Comparison saved to: $COMPARISON_FILE"
}

show_help() {
    echo "benchmark-compare.sh - Automated benchmark comparison"
    echo ""
    echo "Usage: $0 <command>"
    echo ""
    echo "Commands:"
    echo "  baseline   Capture baseline benchmarks (before optimization)"
    echo "  current    Run current benchmarks (after optimization)"
    echo "  compare    Compare baseline vs current using benchstat"
    echo "  all        Run baseline, current, and compare"
    echo "  quick      Quick comparison (1 iteration, faster)"
    echo ""
    echo "Workflow:"
    echo "  1. Before optimization: $0 baseline"
    echo "  2. Implement optimization"
    echo "  3. After optimization:  $0 current"
    echo "  4. Compare results:     $0 compare"
    echo ""
    echo "Decision criteria:"
    echo "  - >20% improvement + <10% regression = MERGE"
    echo "  - Otherwise = DISCARD"
}

case "${1:-help}" in
    baseline)
        run_benchmarks "$BASELINE_FILE" "baseline"
        ;;
    current)
        run_benchmarks "$CURRENT_FILE" "current"
        ;;
    compare)
        compare_benchmarks
        ;;
    all)
        run_benchmarks "$BASELINE_FILE" "baseline"
        echo ""
        echo "=========================================="
        echo ""
        run_benchmarks "$CURRENT_FILE" "current"
        echo ""
        echo "=========================================="
        echo ""
        compare_benchmarks
        ;;
    quick)
        BENCH_COUNT=1
        run_benchmarks "$BASELINE_FILE" "baseline (quick)"
        run_benchmarks "$CURRENT_FILE" "current (quick)"
        compare_benchmarks
        ;;
    help|--help|-h)
        show_help
        ;;
    *)
        echo "Unknown command: $1"
        echo ""
        show_help
        exit 1
        ;;
esac
