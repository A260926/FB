#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
export GOMODCACHE="$work/mod" GOCACHE="$work/cache" GOPROXY=off GOTOOLCHAIN=local GOSUMDB=off GOFLAGS=-mod=vendor
[ -s arm64_unwind.go ] && [ -s arm64_unwind_test.go ]
go test -list '^TestARM64UnwindView$' . | grep -qx TestARM64UnwindView
go test -count=1 -v -run '^TestARM64UnwindView$' . | tee "$work/test.log"
! grep -q -- '--- SKIP:' "$work/test.log"
awk '/--- PASS: TestARM64UnwindView\// {n++} END {exit(n<7)}' "$work/test.log"
go run ./tools/baseline
