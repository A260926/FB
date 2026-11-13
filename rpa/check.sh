#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
export GOENV=off GOTOOLCHAIN=local GOPROXY=off GOSUMDB=off GOWORK=off
test -s rpa_binding_test.go
mkdir -p .task-results
go test -mod=vendor -run '^TestRPABind' -count=1 -json . > .task-results/binding.jsonl
go run -mod=vendor rpa/check.go .task-results/binding.jsonl TestRPABindMatching TestRPABindProtocol TestRPABindFailure
exec bash rpa/baseline.sh
