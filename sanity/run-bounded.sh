#!/usr/bin/env bash
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOFLAGS=-mod=vendor
set -euo pipefail
test -s render/dc/bounded_test.go
test -s sanity/bounded_example.go
export GOMODCACHE="$PWD/.cache/mod"
export GOCACHE="$PWD/.cache/build"
out=$(mktemp)
trap 'rm -f "$out"' EXIT
go test ./render/dc -run '^TestBounded' -count=1 -v | tee "$out"
for name in TestBoundedRank TestBoundedBoundary TestBoundedRender; do
 grep -Fq -- "--- PASS: $name" "$out"
done
go run ./sanity/bounded_example.go
bash sanity/baseline.sh
echo 'SDFX_BOUNDED_QEF_PASS'
