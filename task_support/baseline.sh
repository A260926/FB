#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
task_cache=$(mktemp -d "${TMPDIR:-/tmp}/check.XXXXXX")
trap 'rm -rf "$task_cache"' EXIT HUP INT TERM
export GOMODCACHE="$task_cache/mod" GOCACHE="$task_cache/build" GOPROXY=off GOTOOLCHAIN=local GOSUMDB=off
GOFLAGS= go run ./task_support/baseline.go
