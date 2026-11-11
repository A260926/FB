#!/usr/bin/env bash
set -euo pipefail
mkdir -p .cache/mod .cache/build
export GOMODCACHE="$PWD/.cache/mod"
export GOCACHE="$PWD/.cache/build"
exec go "$@"
