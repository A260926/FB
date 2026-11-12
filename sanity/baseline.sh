#!/usr/bin/env bash
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOFLAGS=-mod=vendor
set -euo pipefail
export GOMODCACHE="$PWD/.cache/mod"
export GOCACHE="$PWD/.cache/build"
go run ./sanity/baseline.go
out=$(mktemp)
trap 'rm -f "$out"' EXIT
go test ./render -run '^Test_LoadSTL$' -count=1 -v | tee "$out"
grep -Fq -- '--- PASS: Test_LoadSTL' "$out"
echo 'BASELINE STL old test PASS'
