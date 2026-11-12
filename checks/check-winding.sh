#!/bin/bash
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOFLAGS=-mod=vendor
set -euo pipefail
test -s model3d/winding_test.go || { echo 'missing model3d/winding_test.go' >&2; exit 1; }
export GOMODCACHE="$PWD/.cache/mod"
export GOCACHE="$PWD/.cache/go"
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
go test -race -v ./model3d -run '^(TestWindingReference|TestWindingHierarchy|TestWindingSDF)$' -count=1 | tee "$b/out"
for name in TestWindingReference TestWindingHierarchy TestWindingSDF; do
 grep -Eq "^--- PASS: $name \\(" "$b/out"
done
bash checks/run-baseline.sh
