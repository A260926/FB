#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d "${TMPDIR:-/tmp}/build.XXXXXX")
trap 'chmod -R u+w "$build" 2>/dev/null || true; rm -rf "$build"' EXIT
export GOWORK=off GOTOOLCHAIN=local GOPROXY=off GOSUMDB=off GOCACHE="$build/cache" GOMODCACHE="$build/mod"
case "${1:-}" in
 baseline)
  go test -modfile=support/offline.mod -count=1 -v -run '^(TestUnitBaseline|TestResponseError|TestAGReadDBPLCErrorResponse|TestHelper_SetBoolAt|TestHelper_GetBoolAt)$' .
  echo 'BASELINE_OK'
  ;;
 target)
  test -s unit_target_test.go
  go test -modfile=support/offline.mod -count=1 -v -timeout=60s -run '^(TestCOTPReceive|TestCOTPNegotiation|TestCOTPAssociation)$' . | tee "$build/target.log"
  grep -Eq '^--- PASS: TestCOTPReceive \(' "$build/target.log"
  grep -Eq 'UNIT_DONE TestCOTPReceive [1-9][0-9]*$' "$build/target.log"
  grep -Eq '^[[:space:]]+--- PASS: TestCOTPReceive/[^ ]+ \(' "$build/target.log"
  grep -Eq '^--- PASS: TestCOTPNegotiation \(' "$build/target.log"
  grep -Eq 'UNIT_DONE TestCOTPNegotiation [1-9][0-9]*$' "$build/target.log"
  grep -Eq '^[[:space:]]+--- PASS: TestCOTPNegotiation/[^ ]+ \(' "$build/target.log"
  grep -Eq '^--- PASS: TestCOTPAssociation \(' "$build/target.log"
  grep -Eq 'UNIT_DONE TestCOTPAssociation [1-9][0-9]*$' "$build/target.log"
  grep -Eq '^[[:space:]]+--- PASS: TestCOTPAssociation/[^ ]+ \(' "$build/target.log"
  bash support/run.sh baseline
  echo 'TARGET_OK'
  ;;
 *) echo 'use baseline or target' >&2; exit 2;;
esac
