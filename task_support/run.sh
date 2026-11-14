#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
export NODE_PATH=''
mode=${1:-baseline}
case "$mode" in
baseline)
  node --require ./task_support/no-packages.cjs task_support/baseline.cjs
  ;;
target)
  test -s task_tests/precision.test.cjs
  node --require ./task_support/no-packages.cjs --test --test-reporter=./task_support/reporter.cjs task_tests/precision.test.cjs
  node --require ./task_support/no-packages.cjs task_support/baseline.cjs --normal-only
  ;;
*) echo "usage: bash task_support/run.sh [baseline|target]" >&2; exit 2 ;;
esac
