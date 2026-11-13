#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
d=$(mktemp -d); trap 'chmod -R u+w "$d"; rm -rf "$d"' EXIT
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOCACHE="${GOCACHE:-$d/build}" GOMODCACHE="$d/mod"
go test -mod=vendor -count=1 -v ./proof -run '^(TestUpdateInclusion|TestUpdateValidation|TestUpdateGeometry)$' | tee "$d/target.log"
for n in TestUpdateInclusion TestUpdateValidation TestUpdateGeometry; do grep -Eq "^--- PASS: $n \(.*\)$" "$d/target.log"; done
! grep -Eq -- '--- SKIP:|no tests to run' "$d/target.log"
go test -mod=vendor -count=1 ./proof ./compact ./rfc6962
echo TARGET_UPDATE_OK
