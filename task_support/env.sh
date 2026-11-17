#!/bin/bash
# Only this restored task's initially empty module/build caches are used.
mkdir -p .task-build/modcache .task-build/gocache
export GOMODCACHE="$PWD/.task-build/modcache"
export GOCACHE="$PWD/.task-build/gocache"
export GOENV=off GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOFLAGS=-mod=vendor
