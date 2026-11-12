#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d)
export GOCACHE="$build/cache" GOMODCACHE="$build/mod"
trap 'chmod -R u+w "$build"; rm -rf "$build"' EXIT
go mod download
go run ./baseline
log="$build/test.log"
go test ./binary -run '^TestEncodeCode$' -count=1 -v | tee "$log"
grep -q -- '--- PASS: TestEncodeCode' "$log"
echo WABIN_OLD_CODE_TEST_OK
