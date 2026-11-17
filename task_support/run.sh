#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=baseline
if (( $# )); then mode=$1; fi
if [[ "$mode" == target ]]; then
  for backend in USE_SCALAR USE_PACKED_SCALAR; do
    TASK_BACKEND="$backend" bash task_support/run-one.sh target
  done
else
  bash task_support/run-one.sh baseline
fi
