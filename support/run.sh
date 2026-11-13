#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d "${TMPDIR:-/tmp}/cookie-build.XXXXXX")
trap 'chmod -R u+w "$build"; rm -rf "$build"' EXIT
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOMODCACHE="$build/mod" GOCACHE="$build/cache"
mkdir -p "$GOMODCACHE" "$GOCACHE"
files=()
for file in *.go;do [[ "$file" == *_test.go ]] || files+=("$file");done
files+=(capture_baseline_test.go jar_test.go punycode_test.go)
case "${1:-baseline}" in
baseline)
 go test -mod=vendor -count=1 -v "${files[@]}" -run '^(TestCaptureBaseline|TestBasics|TestUpdateAndDelete|TestLoadSave|TestCanonicalHost|TestPunycode)$' | tee "$build/result"
 grep -q '^--- PASS: TestCaptureBaseline ' "$build/result"
 grep -q 'BASELINE_COOKIE_STORAGE_OK' "$build/result"
 echo UPSTREAM_COOKIE_CASES_OK
 ;;
target)
 go test -mod=vendor -count=1 -v "${files[@]}" partition_harness_test.go partition_target_test.go -run '^TestPartitionContract$' | tee "$build/result"
 grep -q '^--- PASS: TestPartitionContract ' "$build/result"
 grep -Eq 'PARTITION_COMPLETE cases=([6-9]|[1-9][0-9]+)' "$build/result"
 if grep -q -- '--- SKIP:' "$build/result";then exit 9;fi
 go test -mod=vendor -count=1 -v "${files[@]}" -run '^(TestCaptureBaseline|TestBasics|TestUpdateAndDelete|TestLoadSave|TestCanonicalHost|TestPunycode)$'
 ;;
*) exit 2;;esac
