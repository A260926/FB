#!/bin/bash
set -euo pipefail
test -s ima4_contract_test.go
b=$(mktemp -d)
trap 'chmod -R u+w "$b"; rm -rf "$b"' EXIT
export GOWORK=off GOPROXY=off GOSUMDB=off GOMODCACHE="$b/mod" GOCACHE="${TASK_GOCACHE:-$b/build}" GOFLAGS=-mod=vendor
mkdir -p "$GOMODCACHE" "$GOCACHE"
go test -c -o "$b/ima4.test" .
"$b/ima4.test" -test.count=1 -test.v -test.run='^TestIMA4(Codec|Stream|Seek)$' | tee "$b/out"
for name in TestIMA4Codec/mono TestIMA4Codec/stereo TestIMA4Stream/extent TestIMA4Stream/truncated TestIMA4Seek/exact TestIMA4Seek/pending; do
 grep -Fq -- "--- PASS: $name (" "$b/out"
 grep -Eq -- "CHECK_PASS $name$" "$b/out"
done
for name in TestIMA4Codec TestIMA4Stream TestIMA4Seek; do
 grep -Eq -- "^--- PASS: $name [(]" "$b/out"
done
grep -qx PASS "$b/out"
echo AIFF_IMA4_OK
bash task_support/baseline.sh
