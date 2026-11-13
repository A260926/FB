#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
export GOPROXY=off GOTOOLCHAIN=local GOSUMDB=off GOWORK=off
case "${1:-baseline}" in
baseline) exec go test -modfile=support/offline.mod -mod=readonly -count=1 -v support/baseline_test.go ;;
target)
 test -s relational_query_test.go
 mkdir -p .task-results
 go test -modfile=support/offline.mod -mod=readonly -count=1 -json relational_query_test.go > .task-results/target.jsonl
 go run -modfile=support/offline.mod -mod=readonly support/check.go .task-results/target.jsonl TestRelationAlgebra TestRelationActions TestRelationCompatibility
 exec sh support/run.sh baseline ;;
*) exit 2;;
esac
