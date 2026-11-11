#!/bin/bash
set -euo pipefail
export NODE_PATH="$PWD/userfb/node_modules:$PWD/packages"
exec node "$@"
