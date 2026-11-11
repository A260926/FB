#!/usr/bin/env bash
set -euo pipefail
export GOTOOLCHAIN=local GOWORK=off GOFLAGS=-mod=readonly
export GOPROXY=https://proxy.golang.org GOSUMDB=sum.golang.org
export GOMODCACHE="$PWD/.build/gomod" GOCACHE="$PWD/.build/gocache"
test "$(go env GOVERSION)" = go1.26.4
mkdir -p .build
before="$(shasum -a 256 go.mod go.sum)"
go mod download all
test "$before" = "$(shasum -a 256 go.mod go.sum)"
go mod verify
