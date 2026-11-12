#!/usr/bin/env bash
set -euo pipefail
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
export GOCACHE="$scratch/cache"

cp baseline/floor0_test.go "$scratch/floor0_baseline_test.go"
root=$(pwd)
cat > "$scratch/overlay.json" <<JSON
{"Replace":{"$root/floor0_baseline_test.go":"$scratch/floor0_baseline_test.go"}}
JSON
go test -overlay "$scratch/overlay.json" . -run '^(TestBaselineFloor0|TestDecode|TestIMDCT)$' -count=1 -v | tee "$scratch/result"
for name in TestBaselineFloor0 TestDecode TestIMDCT; do grep -q -- "--- PASS: $name " "$scratch/result"; done
echo VORBIS_OLD_TESTS_OK
