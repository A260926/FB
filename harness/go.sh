#!/usr/bin/env bash
set -euo pipefail
export GOMODCACHE="${TMPDIR:-/tmp}/gomod"
export GOCACHE="${TMPDIR:-/tmp}/gocache"
mkdir -p "$GOMODCACHE" "$GOCACHE"
exec go "$@"
