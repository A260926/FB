#!/bin/sh
# Source from the repository root; all Go caches stay inside this checkout.
export GOPATH="$PWD/.userfb-cache/gopath"
export GOMODCACHE="$GOPATH/pkg/mod"
export GOCACHE="$PWD/.userfb-cache/build"
export GOTMPDIR="$PWD/.userfb-cache/tmp"
export GOTOOLCHAIN=local GOENV=off GOTELEMETRY=off
export GOFLAGS=-mod=readonly
mkdir -p "$GOPATH" "$GOCACHE" "$GOTMPDIR"
