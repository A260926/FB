#!/usr/bin/env bash
set -euo pipefail
export GOENV=off GOTOOLCHAIN=go1.26.4
export GOPATH="$PWD/.build/go" GOMODCACHE="$PWD/.build/mod" GOCACHE="$PWD/.build/cache"
mkdir -p .build
[ "$(go env GOVERSION)" = go1.26.4 ]
go mod download
go mod verify
printf 'TOOLCHAIN %s\n' "$(go env GOVERSION)"
