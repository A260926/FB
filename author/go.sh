#!/usr/bin/env bash
set -euo pipefail
export GOMODCACHE="${TMPDIR:-/tmp}/userfb-z1164-gomod"
export GOCACHE="${TMPDIR:-/tmp}/userfb-z1164-gocache"
mkdir -p "$GOMODCACHE" "$GOCACHE"
exec go "$@"
