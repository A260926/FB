#!/bin/bash
set -euo pipefail
cache=$(mktemp -d)
trap 'chmod -R u+w "$cache"; rm -rf "$cache"' EXIT
export GOCACHE="$cache/build" GOMODCACHE="$cache/mod" GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GO111MODULE=off
go test -count=1 -v ./parser -run '^TestBATBaseline$'
echo VHDX_BASELINE_OK
