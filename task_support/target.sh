#!/bin/bash
set -euo pipefail
export GOPROXY=off GOSUMDB=off GOFLAGS=-mod=readonly
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
# Use the caller's caches only when explicitly supplied; neither may fetch modules.
export GOCACHE="${GOCACHE:-$b/build}" GOMODCACHE="${GOMODCACHE:-$b/mod}"
test -s commands/regen_contract_test.go
test -s commands/example_regen_test.go
set +e
go test ./commands -run '^(TestRegenGeneration|TestRegenRepair|TestRegenLifecycle|Example_regenerativeCommands)$' -count=1 -json > "$b/new.json"
status=$?
set -e
cat "$b/new.json"
test "$status" -eq 0
go run task_support/check.go "$b/new.json" TestRegenGeneration TestRegenRepair TestRegenLifecycle Example_regenerativeCommands
go test ./commands -run '^(TestCommands|TestSeedReplay|Example_circularqueue)$' -count=1 -json > "$b/old.json"
cat "$b/old.json"
go run task_support/check.go "$b/old.json" TestCommands TestSeedReplay Example_circularqueue
