#!/bin/bash
set -euo pipefail
mkdir -p .build
test -s positional_plan_test.go
bash task_support/env.sh go test -race . -run '^TestPositionPlan' -count=1 -v -timeout=60s > .build/position-target.log 2>&1
cat .build/position-target.log
for t in TestPositionPlanResults TestPositionPlanWork TestPositionPlanIsolation; do
 grep -q -- "^--- PASS: $t " .build/position-target.log
done
bash task_support/env.sh go test ./... -count=1 -timeout=60s
