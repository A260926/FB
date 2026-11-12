#!/bin/bash
set -euo pipefail
export GOMODCACHE="$PWD/.build/mod"
export GOCACHE="$PWD/.build/cache"
cd v3
exec "$@"
