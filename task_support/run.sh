#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
case "${1:-baseline}" in
 baseline) exec sh task_support/go.sh go test -mod=vendor -tags=baselinecheck -v -count=1 -timeout=15s -run '^TestBaseline$' .;;
 target) test -s outbound_ids_test.go; mkdir -p .task-build
 sh task_support/go.sh sh -ec 'go test -mod=vendor -json -count=1 -timeout=45s -run "^(TestOutboundIDNamespace|TestOutboundIDPhases|TestOutboundIDRetry)$" . > .task-build/target.json; go run -mod=vendor ./task_support/verify .task-build/target.json TestOutboundIDNamespace TestOutboundIDPhases TestOutboundIDRetry; go test -mod=vendor -count=1 -timeout=30s -run "^(TestConnect|TestRemainingLengthParse|TestPublish_MessageValidation|TestServeMux_HandleFunc)$" .' ;;
 *) exit 2;;
esac
