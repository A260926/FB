#!/bin/sh
set -eu
. ./userfb_env.sh
test -f go-fuzz/feedback_userfb_test.go || { echo 'missing go-fuzz/feedback_userfb_test.go' >&2; exit 1; }
test -x userfb_baseline/testee-bin || { echo 'run sh userfb_baseline/install.sh first' >&2; exit 1; }
export USERFB_TESTEE=../userfb_baseline/testee-bin
mkdir -p .userfb-cache
report=.userfb-cache/target.jsonl
if ! go test -json ./go-fuzz -run '^(TestStableFeedbackReplay|TestStableFeedbackMinimize|TestStableFeedbackAdmission|TestIncrementDecrement)$' -count=1 -timeout=60s > "$report"; then
  cat "$report"
  exit 1
fi
cat "$report"
go run ./userfb_baseline/check-tests.go "$report" TestStableFeedbackReplay TestStableFeedbackMinimize TestStableFeedbackAdmission TestIncrementDecrement
go test ./go-fuzz/versifier -count=1
