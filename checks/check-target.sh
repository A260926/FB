#!/bin/bash
set -euo pipefail
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
export GOCACHE="$build/cache" GOMODCACHE="$build/mod"
CHECK_TARGET=1 go test -count=1 -v -run '^(TestTargetQuotedTerms|TestTargetAnnotations|TestTargetFormats|TestTargetInvalidRoles)$' . | tee "$build/results"
grep -F -- '--- PASS: TestTargetQuotedTerms (' "$build/results" >/dev/null
grep -F -- '--- PASS: TestTargetAnnotations (' "$build/results" >/dev/null
grep -F -- '--- PASS: TestTargetFormats (' "$build/results" >/dev/null
grep -F -- '--- PASS: TestTargetInvalidRoles (' "$build/results" >/dev/null
echo GO_TARGET_COMPLETE
bash checks/baseline.sh
