#!/bin/bash
# Local caches start empty in every restored seed; no host module cache is used.
mkdir -p .task-build/gocache .task-build/modcache
export GOCACHE="$PWD/.task-build/gocache"
export GOMODCACHE="$PWD/.task-build/modcache"
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local CGO_ENABLED=0
export GOFLAGS=-mod=vendor
