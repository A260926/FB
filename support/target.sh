#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
d=$(mktemp -d); trap 'chmod -R u+w "$d"; rm -rf "$d"' EXIT
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOWORK=off GOCACHE="${GOCACHE:-$d/build}" GOMODCACHE="$d/mod"
go test -race -mod=readonly -count=1 -v . -run '^(TestBackupCompaction|TestBackupLifetimes|TestBackupFailures)$' | tee "$d/target.log"
for n in TestBackupCompaction TestBackupLifetimes TestBackupFailures; do grep -Eq "^--- PASS: $n \(.*\)$" "$d/target.log"; done
! grep -Eq -- '--- SKIP:|no tests to run' "$d/target.log"
go test -race -mod=readonly -count=1 -v . -run '^(TestBackup|TestCompaction)$'
echo TARGET_BACKUP_OK
