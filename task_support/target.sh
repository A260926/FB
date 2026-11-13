#!/bin/bash
set -euo pipefail
mkdir -p .build
test -s retrieval_base_test.go
test -s atom/retrieval_base_test.go
test -s rss/retrieval_base_test.go
bash task_support/env.sh go test -race . ./atom ./rss -run '^TestRetrievalBase' -count=1 -v -timeout=60s > .build/base-target.log 2>&1
cat .build/base-target.log
for t in TestRetrievalBaseHTTP TestRetrievalBaseIsolation TestRetrievalBaseAtom TestRetrievalBaseRSS; do
 grep -q -- "^--- PASS: $t " .build/base-target.log
done
bash task_support/env.sh go test ./... -count=1 -timeout=90s
