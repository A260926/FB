#!/bin/bash
set -euo pipefail
export GOMODCACHE="$PWD/.build/modcache"
export GOCACHE="$PWD/.build/gocache"
export GOWORK=off
export GOFLAGS=''
exec go "$@"
