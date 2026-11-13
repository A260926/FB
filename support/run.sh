#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
export GOMODCACHE="$tmp/mod" GOCACHE="$tmp/cache" GOPATH="$tmp/path" GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local
export GOFLAGS='' GOWORK=off
files=()
go list -mod=mod -modfile=support/offline.mod -f '{{range .GoFiles}}{{.}}{{"\n"}}{{end}}' . > "$tmp/files"
while IFS= read -r f; do
 [[ -z "$f" ]] || files+=("$f")
done < "$tmp/files"
files+=(baseline_test.go)
prefix=TestBaseline
if [[ "$mode" == target ]]; then
 test -s dom_lifetime_test.go
 files+=(dom_lifetime_test.go)
 prefix=TestDOMLifetime
elif [[ "$mode" != baseline ]]; then exit 2; fi
set +e
go test -mod=mod -modfile=support/offline.mod -count=1 -json "${files[@]}" > "$tmp/out.json" 2>&1
rc=$?
set -e
if [[ "$rc" != 0 ]]; then cat "$tmp/out.json"; exit "$rc"; fi
node support/check-tests.cjs "$tmp/out.json" "$prefix"
