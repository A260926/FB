#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
cc -std=c99 -O1 -I. mpc.c tests/progress_contract.c baseline/target-main.c -lm -o "$build/target"
"$build/target" > "$build/target.log"
cat "$build/target.log"
for group in effects cycles repetition ownership; do
  test "$(grep -Ec "^PASS $group assertions=[1-9][0-9]*$" "$build/target.log")" -eq 1
done
test "$(grep -Fxc 'MPC_PROGRESS_CONTRACT_OK groups=4' "$build/target.log")" -eq 1
bash baseline/run.sh
