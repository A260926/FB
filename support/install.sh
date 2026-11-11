#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
export GOENV=off GOTOOLCHAIN=local GOTELEMETRY=off GOWORK=off GOFLAGS=-mod=readonly
export GOCACHE="$PWD/.build/go-cache" GOMODCACHE="$PWD/.build/go-mod" GOPATH="$PWD/.build/go-path"
test "$(go version | awk '{print $3}')" = go1.26.4
mkdir -p .build
go version
go mod download
go mod verify
go list -m all
