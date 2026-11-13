#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
export GOENV=off GOFLAGS=
export GOMODCACHE="$build/modcache" GOCACHE="$build/gocache" GOPROXY=off GOTOOLCHAIN=local GOSUMDB=off GOWORK=off
mkdir -p "$build/pkg/internal/saferio"
cp src/debug/plan9obj/*.go "$build/pkg/"
cp -R src/debug/plan9obj/testdata "$build/pkg/"
cp src/internal/saferio/io.go "$build/pkg/internal/saferio/"
sed 's|"internal/saferio"|"author.local/plan9obj/internal/saferio"|' src/debug/plan9obj/file.go > "$build/pkg/file.go"
printf 'module author.local/plan9obj\n\ngo 1.23\n' > "$build/pkg/go.mod"
cp task_support/baseline.go.in "$build/pkg/baseline_test.go"
if [[ "$mode" == target ]]; then
 test -s src/debug/plan9obj/task_cases_test.go || { echo 'missing src/debug/plan9obj/task_cases_test.go' >&2; exit 2; }
 cp task_support/runner.go.in "$build/pkg/author_runner_test.go"
elif [[ "$mode" != baseline ]]; then
 exit 2
fi
cd "$build/pkg"
go test -count=1 -v .
