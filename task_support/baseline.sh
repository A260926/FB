#!/bin/bash
set -euo pipefail
export GOPROXY=off GOSUMDB=off GOFLAGS=-mod=readonly
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
export GOCACHE="$b/build" GOMODCACHE="$b/mod"
go version
go test ./commands -run '^(TestCommands|TestSeedReplay|TestSeedStatefulBaseline)$' -count=1 -v
