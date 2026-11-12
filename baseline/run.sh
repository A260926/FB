#!/usr/bin/env bash
set -euo pipefail
b=$(mktemp -d)
trap 'chmod -R u+w "$b"; rm -rf "$b"' EXIT
export GOTOOLCHAIN=local GOPROXY=off GOSUMDB=off GOCACHE="$b/build" GOMODCACHE="$b/mod"
go test -mod=vendor ./baseline -run '^TestReadabilityBaseline$' -count=1 -v
go test -mod=vendor . -run '^Test_(wordCount|toAbsoluteURI)$' -count=1 -v
