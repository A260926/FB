#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .cache
b=$(mktemp -d .cache/baseline.XXXXXX)
trap 'rm -rf "$b"' EXIT
bash support/build.sh support/baseline.cpp -o "$b/baseline"
"$b/baseline"
for old in integrate_x_sq cos_cubed; do
  bash support/build.sh "src/TheRestOfYourLife/$old.cc" -o "$b/$old"
  "$b/$old" > "$b/$old.stdout"
  cat "$b/$old.stdout"
done
awk '/^I = / {if ($3>2.6666666 && $3<2.6666667) ok=1} END {exit !ok}' "$b/integrate_x_sq.stdout"
awk '/^Estimate = / {if ($3>1.52 && $3<1.62) ok=1} END {exit !ok}' "$b/cos_cubed.stdout"
echo RTW_OLD_NUMERICAL_EXAMPLES_OK
