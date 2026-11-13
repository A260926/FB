#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOWORK=off GOFLAGS=-mod=readonly
mode=${1:-baseline}
case "$mode" in
 baseline)
 go test -count=1 -timeout=20s -v -run '^TestSeed(Raster|Clip|Parallel)$' .
 ;;
 target)
 test -s analytic_target_test.go || { echo 'missing or empty analytic_target_test.go' >&2; exit 1; }
 scratch=$(mktemp -d "${TMPDIR:-/tmp}/fauxgl-analytic.XXXXXX")
 trap 'rm -rf "$scratch"' EXIT HUP INT TERM
 go test -race -count=1 -timeout=45s -json -run '^TestAnalytic(Regions|Integrals|Pipeline)$' . > "$scratch/events.jsonl" 2>&1 || { cat "$scratch/events.jsonl"; exit 1; }
 cat "$scratch/events.jsonl"
 go run ./task_support/checkevents "$scratch/events.jsonl"
 test -s examples/analytic/main.go || { echo "missing analytic example" >&2; exit 1; }
 mkdir -p .cache
 rm -f .cache/analytic.png
 go run ./examples/analytic
 go run ./task_support/checkimage .cache/analytic.png
 go test -count=1 -timeout=20s -v -run '^TestSeed(Raster|Clip|Parallel)$' .
 printf '%s\n' ANALYTIC_TARGET_OK
 ;;
 *) echo 'use baseline or target' >&2; exit 2;;
esac
