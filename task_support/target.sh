#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
export GOPROXY=off GOSUMDB=off GOWORK=off GOTOOLCHAIN=local
export GOMODCACHE="$work/mod" GOCACHE="$work/build"
go test -mod=vendor -count=1 -json ./browser -run '^(TestEntryOwnership|TestEntryMutation|TestEntryWireOrder)$' > "$work/events.json"
node task_support/check-go.cjs "$work/events.json"
go test -mod=vendor -count=1 -v ./browser -run '^(TestRedirect|TestCookieHeader|TestTabInheritance|TestLinksWithBaseTag|TestSubmitMultipart)$'
echo 'TARGET_COMPLETE'
