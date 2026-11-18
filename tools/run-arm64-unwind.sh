#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
export GOMODCACHE="$work/mod" GOCACHE="$work/cache" GOPROXY=off GOTOOLCHAIN=local GOSUMDB=off GOFLAGS=-mod=vendor
[ -s arm64_unwind.go ] && [ -s arm64_unwind_test.go ]
# Capture to a file rather than piping into grep -q: under pipefail a go
# toolchain that terminates on SIGPIPE when grep exits early would fail the
# check even though the test is listed correctly.
go test -list '^TestARM64UnwindView$' . > "$work/list.log" 2>&1
grep -qx TestARM64UnwindView "$work/list.log"
go test -count=1 -v -run '^TestARM64UnwindView$' . | tee "$work/test.log"
! grep -q -- '--- SKIP:' "$work/test.log"
awk '/--- PASS: TestARM64UnwindView\// {n++} END {exit(n<7)}' "$work/test.log"
go run ./tools/baseline
