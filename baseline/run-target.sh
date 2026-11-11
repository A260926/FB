#!/bin/sh
set -eu
. ./env.sh
test -f go-fuzz/feedback_test.go || { echo 'missing go-fuzz/feedback_test.go' >&2; exit 1; }
test -x baseline/testee-bin || { echo 'run sh baseline/install.sh first' >&2; exit 1; }
export CHECK_TESTEE=../baseline/testee-bin
mkdir -p .userfb-cache
report=.userfb-cache/target.jsonl
if ! go test -json ./go-fuzz -run '^(TestStableFeedbackReplay|TestStableFeedbackMinimize|TestStableFeedbackAdmission|TestIncrementDecrement)$' -count=1 -timeout=60s > "$report"; then
  cat "$report"
  exit 1
fi
cat "$report"
go run ./baseline/check-tests.go "$report" TestStableFeedbackReplay TestStableFeedbackMinimize TestStableFeedbackAdmission TestIncrementDecrement
go test ./go-fuzz/versifier -count=1
