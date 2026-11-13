#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
export GOMODCACHE="$work/mod" GOCACHE="$work/cache" GOPROXY=off GOTOOLCHAIN=local GOSUMDB=off
[ -s dynamic_catalog.go ] && [ -s dynamic_catalog_test.go ]
go test -list '^TestDynamicCatalog$' . | grep -qx TestDynamicCatalog
go test -count=1 -v -run '^TestDynamicCatalog$' . | tee "$work/test.log"
! grep -q -- '--- SKIP:' "$work/test.log"
awk '/--- PASS: TestDynamicCatalog\// {n++} END {exit(n<6)}' "$work/test.log"
go run ./tools/baseline
