#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test -f test/segment_summary_test.c || { echo 'MISSING test/segment_summary_test.c' >&2; exit 1; }
test -f ebur128/ebur128_segment.c || { echo 'MISSING ebur128/ebur128_segment.c' >&2; exit 1; }
bash checks/build.sh
"${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_GNU_SOURCE -O2 -I ebur128 -I ebur128/queue -I checks \
  ebur128/ebur128_segment.c checks/runner.c .checks-build/ebur128.o -lm -o .checks-build/target
.checks-build/target | tee .checks-build/target.log
awk '
 /^GROUP (filter|gating|composition) CASES=[1-9][0-9]* PASS$/ {g[$2]++}
 /^SEGMENT_TARGET_COMPLETE CASES=[1-9][0-9]*$/ {done++}
 END {exit !(g["filter"]==1 && g["gating"]==1 && g["composition"]==1 && done==1)}
' .checks-build/target.log
bash checks/baseline.sh
bash checks/upstream.sh
