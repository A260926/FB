#!/usr/bin/env bash
set -euo pipefail
export GOMODCACHE="${TMPDIR:-/tmp}/userfb-z1163-gomod"
export GOCACHE="${TMPDIR:-/tmp}/userfb-z1163-gocache"
mkdir -p "$GOMODCACHE" "$GOCACHE"
exec go "$@"
