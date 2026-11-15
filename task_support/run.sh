#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
export NODE_PATH="$PWD/vendor"
node task_support/run.cjs "${1:-baseline}"
