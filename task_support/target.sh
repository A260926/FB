#!/bin/bash
set -euo pipefail
export NODE_PATH="$PWD/task_support/vendor"
test -f test/async-filters.cjs
node --test test/async-filters.cjs
node task_support/compat.cjs
