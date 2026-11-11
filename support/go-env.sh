#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$root/.cache/build" "$root/.cache/mod" "$root/.cache/gopath"
cd "$root"
export GOTOOLCHAIN=go1.26.4
export GOCACHE="$root/.cache/build"
export GOMODCACHE="$root/.cache/mod"
export GOPATH="$root/.cache/gopath"
exec "$@"
