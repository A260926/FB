#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test -s go-mpeg2/ts-cbr.go
test -s go-mpeg2/cbr_cases_test.go
mkdir -p .task-build
task_cache=$(mktemp -d "${TMPDIR:-/tmp}/check-target.XXXXXX")
trap 'rm -rf "$task_cache"' EXIT HUP INT TERM
export GOMODCACHE="$task_cache/mod" GOCACHE="$task_cache/build" GOPROXY=off GOTOOLCHAIN=local GOSUMDB=off
export GOFLAGS=
go test -count=1 -json ./go-mpeg2 -run '^TestCBR(Clock|Tables|Media|Compatibility)$' > .task-build/tests.json
go run ./task_support/checktests.go
go run ./task_support/baseline.go
