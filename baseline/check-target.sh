#!/usr/bin/env bash
set -euo pipefail
test -s text_stats_index_test.go
b=$(mktemp -d)
trap 'chmod -R u+w "$b"; rm -rf "$b"' EXIT
export GOTOOLCHAIN=local GOPROXY=off GOSUMDB=off GOCACHE="$b/build" GOMODCACHE="$b/mod"
go test -mod=vendor . -run '^TestTextStatsIndex' -count=1 -v | tee "$b/out"
grep -Eq '^--- PASS: TestTextStatsIndex' "$b/out"
go test -mod=vendor ./baseline -run '^TestReadabilityBaseline$' -count=1 -v
go test -mod=vendor . -run '^Test_(wordCount|toAbsoluteURI)$' -count=1 -v
