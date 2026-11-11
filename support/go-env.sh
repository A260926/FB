#!/usr/bin/env bash
set -euo pipefail
mkdir -p .build
export GOCACHE="${TMPDIR:-/tmp}/gocache-build"
export GOMODCACHE="${TMPDIR:-/tmp}/gocache-mod"
exec "$@"
