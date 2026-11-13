#!/bin/bash
set -euo pipefail
mkdir -p .build
test -s content_dedup_test.go
bash task_support/env.sh go test . -run '^TestContentDedup' -count=1 -v -timeout=60s > .build/dedup-target.log 2>&1
cat .build/dedup-target.log
for t in TestContentDedupRuns TestContentDedupAnchors TestContentDedupAttempts; do
 grep -q -- "^--- PASS: $t " .build/dedup-target.log
done
bash task_support/env.sh go test ./... -count=1 -timeout=90s
