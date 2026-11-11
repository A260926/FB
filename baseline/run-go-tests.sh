#!/bin/bash
set -euo pipefail
export GOWORK=off
export GOFLAGS=''
export GOCACHE="$PWD/.userfb-build/go-cache"
mkdir -p "$GOCACHE"
exec go run ./baseline/checktests "$@"
