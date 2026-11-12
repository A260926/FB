#!/usr/bin/env bash
set -euo pipefail
test -s include_graph_test.go
b=$(mktemp -d)
trap 'chmod -R u+w "$b"; rm -rf "$b"' EXIT
export GOTOOLCHAIN=local GOPROXY=off GOSUMDB=off GOCACHE="$b/build" GOMODCACHE="$b/mod"
go test -mod=vendor . -run '^TestIncludeGraph' -count=1 -v | tee "$b/out"
grep -Eq '^--- PASS: TestIncludeGraph' "$b/out"
go test -mod=vendor ./baseline -run '^TestIncludeBaseline$' -count=1 -v
go test -mod=vendor . -run '^Test_(BackcompatIncludeRefs|IsAncestorNode)$' -count=1 -v
