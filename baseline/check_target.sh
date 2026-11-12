#!/usr/bin/env bash
set -euo pipefail
if [[ ! -s floor0_regression_test.go ]]; then echo 'missing floor0_regression_test.go' >&2; exit 1; fi
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
export GOCACHE="$scratch/cache"
go test . -run '^(TestFloor0Coefficients|TestFloor0Curve|TestFloor0Packets)$' -v -count=1 | tee "$scratch/result"
for name in TestFloor0Coefficients TestFloor0Curve TestFloor0Packets; do grep -q -- "--- PASS: $name " "$scratch/result"; done
if grep -q -- '--- SKIP:' "$scratch/result"; then exit 1; fi
bash baseline/run.sh
echo FLOOR0_REGRESSION_OK
