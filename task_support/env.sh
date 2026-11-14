#!/bin/bash
# Source from the restored repository root. No runtime downloads or host module cache.
export GOENV=off GOTOOLCHAIN=local GOPROXY=off GOSUMDB=off GOWORK=off
export GOMODCACHE="$PWD/.task-build/modcache" GOCACHE="$PWD/.task-build/gocache"
export GOFLAGS=-mod=readonly
mkdir -p "$GOMODCACHE" "$GOCACHE"
