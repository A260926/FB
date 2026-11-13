#!/bin/sh
set -eu
TASK_CACHE=$(mktemp -d "${TMPDIR:-/tmp}/go.XXXXXX")
trap 'rm -rf "$TASK_CACHE"' EXIT HUP INT TERM
export GOMODCACHE="$TASK_CACHE/mod" GOCACHE="$TASK_CACHE/build" GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local
mkdir -p "$GOMODCACHE" "$GOCACHE"
exec_code=0
go "$@" || exec_code=$?
exit "$exec_code"
