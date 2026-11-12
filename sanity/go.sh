#!/bin/bash
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOFLAGS=-mod=vendor
set -euo pipefail
export GOMODCACHE="$PWD/.build/mod"
export GOCACHE="$PWD/.build/cache"
exec "$@"
