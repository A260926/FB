#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
export GOMODCACHE="$work/mod" GOCACHE="$work/cache" GOPROXY=off GOTOOLCHAIN=local GOSUMDB=off GOFLAGS=-mod=vendor
[ -s split_page_groups.go ] && [ -s split_page_groups_test.go ]
go test -list '^TestSplitPageGroups$' . | grep -qx TestSplitPageGroups
go test -count=1 -v -run '^TestSplitPageGroups$' . | tee "$work/test.log"
! grep -q -- '--- SKIP:' "$work/test.log"
awk '/--- PASS: TestSplitPageGroups\// {n++} END {exit(n<7)}' "$work/test.log"
go run ./tools/baseline
