#!/usr/bin/env bash
set -euo pipefail
export GOENV=off GOTOOLCHAIN=go1.26.4
export GOPATH="$PWD/.build/go" GOMODCACHE="$PWD/.build/mod" GOCACHE="$PWD/.build/cache"
test -s effective_redirect_test.go || { echo 'missing effective_redirect_test.go' >&2; exit 2; }
mkdir -p .build
log=$(mktemp .build/effective-redirect.XXXXXX)
trap 'rm -f "$log"' EXIT
if ! go test -mod=readonly -race -json -count=1 -run '^TestEffectiveRedirect(Live|Cache|Abort|Isolation)$' . > "$log"; then
 cat "$log"
 exit 1
fi
cat "$log"
# Read go test JSON with Go's JSON decoder so a log string cannot forge a PASS event.
go run -mod=readonly author_support/check_events/main.go "$log"
