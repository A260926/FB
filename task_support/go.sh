#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
task_cache=$(mktemp -d "${TMPDIR:-/tmp}/go.XXXXXX")
trap 'rm -rf "$task_cache"' EXIT HUP INT TERM
export GOCACHE="$task_cache/build" GOMODCACHE="$task_cache/mod" GOPROXY=off GOTOOLCHAIN=local GOSUMDB=off
exec_code=0
"$@" || exec_code=$?
exit "$exec_code"
