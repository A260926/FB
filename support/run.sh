#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d "${TMPDIR:-/tmp}/build.XXXXXX")
trap 'chmod -R u+w "$build" 2>/dev/null || true; rm -rf "$build"' EXIT
export GOWORK=off GOTOOLCHAIN=local GOPROXY=off GOSUMDB=off GOCACHE="$build/cache" GOMODCACHE="$build/mod"
case "${1:-}" in
 baseline)
  go test -mod=vendor -count=1 -v -run '^(TestUnitBaseline|TestRequest|TestFromRequest|TestParseRetryAfterHeader|TestClient_DefaultBackoff|TestRoundTripper_implements)$' .
  echo 'BASELINE_OK'
  ;;
 target)
  test -s unit_target_test.go
  go test -mod=vendor -count=1 -v -timeout=60s -run '^(TestOriginGateSchedule|TestOriginGateRedirect|TestOriginGateErrors)$' . | tee "$build/target.log"
  grep -Eq '^--- PASS: TestOriginGateSchedule \(' "$build/target.log"
  grep -Eq 'UNIT_DONE TestOriginGateSchedule [1-9][0-9]*$' "$build/target.log"
  grep -Eq '^[[:space:]]+--- PASS: TestOriginGateSchedule/[^ ]+ \(' "$build/target.log"
  grep -Eq '^--- PASS: TestOriginGateRedirect \(' "$build/target.log"
  grep -Eq 'UNIT_DONE TestOriginGateRedirect [1-9][0-9]*$' "$build/target.log"
  grep -Eq '^[[:space:]]+--- PASS: TestOriginGateRedirect/[^ ]+ \(' "$build/target.log"
  grep -Eq '^--- PASS: TestOriginGateErrors \(' "$build/target.log"
  grep -Eq 'UNIT_DONE TestOriginGateErrors [1-9][0-9]*$' "$build/target.log"
  grep -Eq '^[[:space:]]+--- PASS: TestOriginGateErrors/[^ ]+ \(' "$build/target.log"
  bash support/run.sh baseline
  echo 'TARGET_OK'
  ;;
 *) echo 'use baseline or target' >&2; exit 2;;
esac
