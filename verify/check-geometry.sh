#!/usr/bin/env bash
#
# check-geometry.sh - build and run the headless octasphere geometry
# regression. The production header is compiled unchanged for the clean run;
# three independent temporary source copies each receive one compiled-in,
# behavior-changing mutation and must be caught by the matching assertion
# family. Both PARO_CONSTANT_TOPOLOGY settings are exercised.
#
# Usage: check-geometry.sh [output_dir]
#
# A rounded-box OBJ and the check statistics are written to output_dir.
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/.." && pwd)
outdir=${1:-"$repo_root/verify/geometry-output"}

work=$(mktemp -d "${TMPDIR:-/tmp}/octasphere-geometry.XXXXXX")
trap 'rm -rf "$work"' EXIT
mkdir -p "$outdir"

cxx=${CXX:-c++}
cc=${CC:-cc}

test_src="$script_dir/octasphere_geometry.cpp"
mutator_src="$script_dir/mutate_header.c"
header="$repo_root/par_octasphere.h"

for required in "$test_src" "$mutator_src" "$header"; do
    if [ ! -s "$required" ]; then
        echo "missing required source: $required" >&2
        exit 2
    fi
done

# Optional watchdog: only used when available; a hang can never be reported as
# a detection because no DETECTED line is printed while the binary is alive.
run_bounded() {
    if command -v timeout >/dev/null 2>&1; then
        timeout 60 "$@"
    else
        "$@"
    fi
}

"$cc" -std=c99 -O2 "$mutator_src" -o "$work/mutate_header"

expected_scenarios=14
modes=(angle offset winding)

# Mutation -> assertion family that must detect it (plain function so this also
# works on Bash 3.2, which has no associative arrays).
detected_family() {
    case "$1" in
        angle)   printf '%s' geodesic-sampling ;;
        offset)  printf '%s' surface-normal ;;
        winding) printf '%s' closed-topology ;;
        *)       printf '%s' unknown ;;
    esac
}

overall_rc=0

for topo in 1 0; do
    echo "=== PARO_CONSTANT_TOPOLOGY=$topo : clean generation ==="
    run_dir="$work/clean-topo$topo"
    mkdir -p "$run_dir"
    "$cxx" -std=c++17 -O2 -DPARO_CONSTANT_TOPOLOGY="$topo" \
        -I"$repo_root" "$test_src" -o "$work/geom-clean-$topo"
    set +e
    clean_log=$(run_bounded "$work/geom-clean-$topo" "$run_dir" clean 2>&1)
    clean_rc=$?
    set -e
    printf '%s\n' "$clean_log"
    if [ "$clean_rc" -ne 0 ]; then
        echo "clean run failed for PARO_CONSTANT_TOPOLOGY=$topo (rc=$clean_rc)" >&2
        overall_rc=1
        continue
    fi
    for token in "PASS surface-normal" "PASS geodesic-sampling" "PASS closed-topology"; do
        if ! printf '%s\n' "$clean_log" | grep -qF "$token"; then
            echo "missing $token for topology=$topo" >&2
            overall_rc=1
        fi
    done
    scenario_rows=$(awk '$2 ~ /^[0-9]+$/ { c++ } END { print c + 0 }' \
        "$run_dir/geometry-stats.txt")
    if [ "$scenario_rows" -ne "$expected_scenarios" ]; then
        echo "expected $expected_scenarios scenarios, found $scenario_rows" >&2
        overall_rc=1
    fi
    if [ ! -s "$run_dir/rounded_box.obj" ] || [ ! -s "$run_dir/geometry-stats.txt" ]; then
        echo "clean run did not produce OBJ and statistics" >&2
        overall_rc=1
    fi

    # Preserve the artifacts from the default-topology clean run.
    if [ "$topo" -eq 1 ]; then
        cp "$run_dir/rounded_box.obj" "$outdir/rounded_box.obj"
        cp "$run_dir/geometry-stats.txt" "$outdir/geometry-stats.txt"
    fi
    cp "$run_dir/geometry-stats.txt" "$outdir/geometry-stats-topo$topo.txt"

    echo "=== PARO_CONSTANT_TOPOLOGY=$topo : mutation detection ==="
    for mode in "${modes[@]}"; do
        copy_dir="$work/mut-$mode-topo$topo"
        mkdir -p "$copy_dir"
        # Independent temporary source copy; the repo header is never modified.
        cp "$header" "$copy_dir/par_octasphere.h"
        "$work/mutate_header" "$mode" "$header" "$copy_dir/par_octasphere.h"
        "$cxx" -std=c++17 -O2 -DPARO_CONSTANT_TOPOLOGY="$topo" \
            -I"$copy_dir" -I"$repo_root" "$test_src" -o "$work/geom-$mode-$topo"
        mut_dir="$work/run-mut-$mode-topo$topo"
        mkdir -p "$mut_dir"
        set +e
        mut_log=$(run_bounded "$work/geom-$mode-$topo" "$mut_dir" "$mode" 2>&1)
        mut_rc=$?
        set -e
        printf '%s\n' "$mut_log"
        if [ "$mut_rc" -ne 0 ]; then
            echo "mutation $mode failed at runtime (rc=$mut_rc); not a detection" >&2
            overall_rc=1
            continue
        fi
        if ! printf '%s\n' "$mut_log" | grep -qF "DETECTED $mode"; then
            echo "mutation $mode escaped the $(detected_family "$mode") assertions" >&2
            overall_rc=1
        fi
        if ! printf '%s\n' "$mut_log" | grep -qF "family=$(detected_family "$mode")"; then
            echo "mutation $mode was not attributed to $(detected_family "$mode")" >&2
            overall_rc=1
        fi
    done
done

# Re-assert the required output tokens from the complete transcript.
if [ "$overall_rc" -eq 0 ]; then
    echo "GEOMETRY ARTIFACTS: $outdir/rounded_box.obj $outdir/geometry-stats.txt"
    echo "OCTASPHERE GEOMETRY ALL CHECKS PASS"
fi
exit "$overall_rc"
