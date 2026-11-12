#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOCACHE="$build/cache" GOMODCACHE="$build/modcache"
go run ./baseline/cfg.go
