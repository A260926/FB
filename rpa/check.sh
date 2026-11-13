#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
export GOENV=off GOTOOLCHAIN=local GOPROXY=off GOSUMDB=off GOWORK=off
test -s rpa_ax_test.go
mkdir -p .task-results
go test -mod=vendor -run '^TestAX' -count=1 -json . > .task-results/ax.jsonl
go run -mod=vendor rpa/check.go .task-results/ax.jsonl TestAXClosure TestAXProjection TestAXFocus
exec bash rpa/baseline.sh
