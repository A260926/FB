#!/bin/bash
# Baseline regression check: builds the library, the command-line
# utility, and the examples, and runs a smoke test of the command-line
# utility to confirm that the existing behavior is unchanged.
set -e
cd "$(dirname "$0")/.."

make -C src
make -C examples

# Smoke test: two particles in a unit cube must produce two cells whose
# volumes sum to the box volume
work=checks/.tmp
trap 'rm -rf "$work"' EXIT
mkdir -p "$work"
printf '1 0.25 0.5 0.5\n2 0.75 0.5 0.5\n' > "$work/pts.txt"
./src/voro++ -c "%v" 0 1 0 1 0 1 "$work/pts.txt"
awk 'BEGIN {s=0} {s+=$1; n++} END {exit !(n==2 && s>0.999999 && s<1.000001)}' "$work/pts.txt.vol"

echo "BASELINE_OK"
