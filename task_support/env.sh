#!/bin/bash
set -euo pipefail
mkdir -p .build/go-cache .build/modcache
export GOWORK=off GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOFLAGS=-mod=vendor
export GOCACHE="$PWD/.build/go-cache" GOMODCACHE="$PWD/.build/modcache"
exec "$@"
