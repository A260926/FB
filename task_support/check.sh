#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test -s task_support/target_cases.c
sh task_support/build.sh
sh task_support/baseline.sh
cc -std=gnu99 -O2 -Itask_support task_support/driver.c task_support/target_cases.c -lm -o .build/cut_plan_test
export SOX_TASK_BIN="$PWD/.build/src/sox"
.build/cut_plan_test > .build/cut_plan_test.log
cat .build/cut_plan_test.log
for group in planner pcm lifecycle; do grep -q "^GROUP $group assertions=" .build/cut_plan_test.log; done
grep -q '^SOX_CUT_PLAN_COMPLETE$' .build/cut_plan_test.log
