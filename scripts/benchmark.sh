#!/bin/bash
set -euo pipefail

# ── Configurable ──────────────────────────────────────────────
SCENE="${BENCH_SCENE:-../TestSuite/path_scene.json}"
THREAD_COUNTS=(${BENCH_THREADS:-1 2 4 8 16 32})
RUNS=${BENCH_RUNS:-3}
OUTPUT_DIR="/tmp/bench_cgrcw2/"
# ──────────────────────────────────────────────────────────────

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

cd "$PROJECT_DIR/build"

# ── Metadata ──────────────────────────────────────────────────
echo "# Benchmark Results"
echo ""
echo "## Metadata"
echo "- **Date**: $(date -u +"%Y-%m-%d %H:%M UTC")"
echo "- **CPU**: $(grep 'model name' /proc/cpuinfo | head -1 | cut -d: -f2 | xargs)"
echo "- **Cores**: $(nproc)"
echo "- **Compiler**: $(g++ --version | head -1)"
echo "- **Commit**: $(git -C "$PROJECT_DIR" rev-parse --short HEAD)"
echo "- **Scene**: $SCENE"
echo "- **Runs per config**: $RUNS (median reported)"
echo ""

# ── Build Release ─────────────────────────────────────────────
echo "Building Release..." >&2
cmake -DCMAKE_BUILD_TYPE=Release .. > /dev/null 2>&1
make -j$(nproc) > /dev/null 2>&1
echo "Build complete." >&2
echo ""

mkdir -p "$OUTPUT_DIR"

# ── Helpers ───────────────────────────────────────────────────
median_of() {
    # Args: space-separated numbers
    local sorted=($(printf '%s\n' $@ | sort -n))
    local count=${#sorted[@]}
    local mid=$((count / 2))
    echo "${sorted[$mid]}"
}

run_single() {
    # Args: threads
    # Returns: time in seconds (just the NT render time)
    local threads=$1
    local times=()
    for run in $(seq 1 $RUNS); do
        local output
        output=$($BINARY "$SCENE" "$OUTPUT_DIR" "bench_t${threads}_run${run}" --threads "$threads" 2>/dev/null || true)
        local t
        t=$(echo "$output" | grep "Rendering completed in" | awk '{print $4}')
        if [ -z "$t" ]; then
            echo "ERROR: no timing output for threads=$threads run=$run" >&2
            echo "$output" >&2
            exit 1
        fi
        times+=("$t")
    done
    median_of "${times[@]}"
}

BINARY="./ray_tracer"

# ── Header ────────────────────────────────────────────────────
echo "## Scaling Results"
echo ""
echo "| Threads | Median Time (s) | Speedup vs 1T | Parallel Efficiency |"
echo "|--------:|----------------:|---------------:|--------------------:|"

# ── Run benchmarks ────────────────────────────────────────────
baseline_time=""

for threads in "${THREAD_COUNTS[@]}"; do
    echo "# Running $threads thread(s) × $RUNS runs..." >&2
    median=$(run_single "$threads")

    if [ "$threads" -eq 1 ]; then
        baseline_time=$median
        echo "| $threads | $median | 1.00× | — |"
    else
        speedup=$(echo "scale=2; $baseline_time / $median" | bc)
        efficiency=$(echo "scale=0; $speedup * 100 / $threads" | bc)
        echo "| $threads | $median | ${speedup}× | ${efficiency}% |"
    fi
done

echo ""
echo "## Reproduce"
echo ""
echo "\`\`\`bash"
echo "bash scripts/benchmark.sh"
echo "\`\`\`"
echo ""
echo "Custom scene or thread counts:"
echo ""
echo "\`\`\`bash"
echo "BENCH_SCENE=../data/jsons/benchmark_scene.json BENCH_THREADS=\"1 4 8\" bash scripts/benchmark.sh"
echo "\`\`\`"
