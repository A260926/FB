#!/usr/bin/env bash
set -euo pipefail
export GOTOOLCHAIN=go1.26.4 GOWORK=off GOFLAGS=-mod=readonly
export GOCACHE="$PWD/.cache/build" GOMODCACHE="$PWD/.cache/mod"
export GOTMPDIR="$PWD/.cache/tmp"
mkdir -p "$GOCACHE" "$GOMODCACHE" "$GOTMPDIR"
exec go "$@"
