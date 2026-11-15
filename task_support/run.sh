#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
case "${1:-target}" in
 baseline) node --disable-warning=ExperimentalWarning --import ./task_support/loader.mjs baseline/current.ts ;;
 target)
  for path in src/animation/tracks/CurveChainTrack.ts src/ikrig/solvers/CurveChainSolver.ts test/curve-chain.ts examples/curve-chain-cpu.ts; do test -s "$path" || { echo "REQUIRED_FILE_MISSING_OR_EMPTY $path" >&2; exit 1; }; done
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  node --disable-warning=ExperimentalWarning --import ./task_support/loader.mjs task_support/runner.mjs >"$output_log" 2>&1 || { cat "$output_log"; exit 1; }
  cat "$output_log"
  for group in roots sampling skinning; do grep -Eq "^TASK_GROUP_OK $group checks=[1-9][0-9]*$" "$output_log"; done
  grep -qx TASK_TARGET_COMPLETE "$output_log"
  node --disable-warning=ExperimentalWarning --import ./task_support/loader.mjs examples/curve-chain-cpu.ts >"$output_log" 2>&1 || { cat "$output_log"; exit 1; }
  cat "$output_log"
  grep -qx CURVE_CHAIN_DEMO_OK "$output_log"
  node --disable-warning=ExperimentalWarning --import ./task_support/loader.mjs baseline/current.ts
  ;;
 *) exit 2;;
esac
