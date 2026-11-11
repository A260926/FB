#!/bin/bash
set -euo pipefail
export NODE_PATH="$PWD/devcheck/node_modules:$PWD/packages"
exec node "$@"
