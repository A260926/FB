#!/bin/sh
set -eu
TASK_CACHE=$(mktemp -d "${TMPDIR:-/tmp}/grab-go.XXXXXX")
trap 'rm -rf "$TASK_CACHE"' EXIT HUP INT TERM
export GOMODCACHE="$TASK_CACHE/mod" GOCACHE="$TASK_CACHE/build" GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local
mkdir -p "$GOMODCACHE" "$GOCACHE"
cd v3
TASK_EXIT=0
go "$@" || TASK_EXIT=$?
exit "$TASK_EXIT"
