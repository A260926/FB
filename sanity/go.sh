#!/bin/bash
set -euo pipefail
export GOMODCACHE="$PWD/.userfb-build/modcache"
export GOCACHE="$PWD/.userfb-build/gocache"
export GOWORK=off
export GOFLAGS=''
exec go "$@"
