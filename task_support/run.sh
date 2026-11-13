#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
export GOENV=off GOFLAGS=
export GOMODCACHE="$build/modcache" GOCACHE="$build/gocache" GOPROXY=off GOTOOLCHAIN=local GOSUMDB=off GOWORK=off
mkdir "$build/work"
cp -R binary wasm leb128 ieee754 u64 task_support go.mod go.sum "$build/work/"
if [[ "$mode" == target ]]; then
 test -s analysis/task_cases_test.go || { echo 'missing analysis/task_cases_test.go' >&2; exit 2; }
 cp -R analysis "$build/work/"
 cp task_support/runner.go.in "$build/work/analysis/runner_test.go"
elif [[ "$mode" != baseline ]]; then
 exit 2
fi
cd "$build/work"
go run ./task_support/baseline
if [[ "$mode" == target ]]; then go test -count=1 -v ./analysis; fi
