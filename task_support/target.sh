#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
export GOPROXY=off GOSUMDB=off GOWORK=off GOTOOLCHAIN=local
export GOMODCACHE="$work/mod" GOCACHE="$work/build"
go test -mod=vendor -count=1 -json . -run '^(TestSessionCorrelation|TestSessionRetransmit|TestSessionRetirement)$' > "$work/events.json"
node task_support/check-go.cjs "$work/events.json"
go test -mod=vendor -count=1 -v . -run '^(TestMessageConfirmable|TestEncodeMessageSmallWithPayload|TestDecodeMessageSmallWithPayload|TestPathMatching|TestPathMatch)$'
echo 'TARGET_COMPLETE'
