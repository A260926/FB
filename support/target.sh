#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
d=$(mktemp -d); trap 'chmod -R u+w "$d"; rm -rf "$d"' EXIT
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOWORK=off GOCACHE="${GOCACHE:-$d/build}" GOMODCACHE="$d/mod"
go test -mod=readonly -count=1 -v . -run '^(TestExclusiveNamespaces|TestExclusiveOctets|TestExclusiveWriter)$' | tee "$d/target.log"
for n in TestExclusiveNamespaces TestExclusiveOctets TestExclusiveWriter; do grep -Eq "^--- PASS: $n \(.*\)$" "$d/target.log"; done
! grep -Eq -- '--- SKIP:|no tests to run' "$d/target.log"
go test -mod=readonly -count=1 .
echo TARGET_EXCLUSIVE_OK
