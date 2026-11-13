#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
case "${1:-baseline}" in
 baseline) exec sh task_support/go.sh go run -mod=vendor ./task_support/baseline;;
 target) test -s ratelimit_context_test.go
 mkdir -p .task-build
 sh task_support/go.sh sh -ec 'go test -mod=vendor -json -count=1 -timeout=45s -run "^(TestRateWaitContext|TestRateGlobalRecheck|TestRateRetryContext)$" . > .task-build/target.json; go run -mod=vendor ./task_support/verify .task-build/target.json TestRateWaitContext TestRateGlobalRecheck TestRateRetryContext; go test -mod=vendor -count=1 -run "^TestRatelimit" .' ;;
 *) exit 2;;
esac
