#!/bin/bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
bash support/build.sh "$build" target
"$build/run" | tee "$build/target.log"
for group in optimality composite legacy; do
 grep -Eq "^GROUP $group checks=[1-9][0-9]*$" "$build/target.log"
done
grep -Eq "^GIF_PLAN_OK checks=[1-9][0-9]*$" "$build/target.log"
bash support/baseline.sh
