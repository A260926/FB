#!/bin/bash
set -euo pipefail
test -f overlapping_limits_target_test.go
bash task_support/env.sh go test -race -count=1 -timeout=45s -run '^TestOverlappingLimits(Admission|Shared|Release)$' -v .
bash task_support/env.sh go test -race -count=1 -timeout=45s -run '^(TestSharedLimitRule_AInitThenBInit_ANotStuck|TestSharedLimitRuleRace|TestLimitRuleClone)$' .
