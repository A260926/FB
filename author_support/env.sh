#!/bin/sh
set -eu
export GOTOOLCHAIN=local
export GOPATH="$PWD/.build/gopath"
export GOMODCACHE="$GOPATH/pkg/mod"
export GOCACHE="$PWD/.build/gocache"
mkdir -p "$GOCACHE" "$GOMODCACHE"
exec "$@"
