#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d "${TMPDIR:-/tmp}/build.XXXXXX")
trap 'chmod -R u+w "$build" 2>/dev/null || true; rm -rf "$build"' EXIT
export GOWORK=off GOTOOLCHAIN=local GOPROXY=off GOSUMDB=off GOCACHE="$build/cache" GOMODCACHE="$build/mod"
case "${1:-}" in
 baseline)
  go test -mod=vendor -count=1 -v -run '^(TestUnitBaseline|TestParseValidListLine|TestParseSymlinks|TestScanner|TestScannerEmpty|TestNoCommandInjection|TestEPSV_Parse_.*)$' .
  echo 'BASELINE_OK'
  ;;
 target)
  test -s unit_target_test.go
  go test -mod=vendor -count=1 -v -timeout=60s -run '^(TestFXPTransfer|TestFXPFinalReplies|TestFXPAbort)$' . | tee "$build/target.log"
  grep -Eq '^--- PASS: TestFXPTransfer \(' "$build/target.log"
  grep -Eq 'UNIT_DONE TestFXPTransfer [1-9][0-9]*$' "$build/target.log"
  grep -Eq '^[[:space:]]+--- PASS: TestFXPTransfer/[^ ]+ \(' "$build/target.log"
  grep -Eq '^--- PASS: TestFXPFinalReplies \(' "$build/target.log"
  grep -Eq 'UNIT_DONE TestFXPFinalReplies [1-9][0-9]*$' "$build/target.log"
  grep -Eq '^[[:space:]]+--- PASS: TestFXPFinalReplies/[^ ]+ \(' "$build/target.log"
  grep -Eq '^--- PASS: TestFXPAbort \(' "$build/target.log"
  grep -Eq 'UNIT_DONE TestFXPAbort [1-9][0-9]*$' "$build/target.log"
  grep -Eq '^[[:space:]]+--- PASS: TestFXPAbort/[^ ]+ \(' "$build/target.log"
  bash support/run.sh baseline
  echo 'TARGET_OK'
  ;;
 *) echo 'use baseline or target' >&2; exit 2;;
esac
