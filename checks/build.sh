#!/bin/bash
set -euo pipefail
out=$1; shift
files=()
for dir in bfr far sdc vtr; do
 for f in opensubdiv/$dir/*.cpp; do files+=("$f"); done
done
c++ -std=c++17 -O0 -DOPENSUBDIV_GREGORY_EVAL_TRUE_DERIVATIVES -I. -Ichecks "${files[@]}" "$@" -o "$out"
