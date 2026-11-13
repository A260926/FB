#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
export GOPROXY=off GOSUMDB=off GOWORK=off GOTOOLCHAIN=local
export GOMODCACHE="$work/mod" GOCACHE="$work/build"
go test -mod=vendor -count=1 -json . -run '^(TestIXFRGrammar|TestZoneTransferApply|TestZoneTransferOwnership)$' > "$work/events.json"
node task_support/check-go.cjs "$work/events.json"
go test -mod=vendor -count=1 -v . -run '^(TestSingleEnvelopeXfr|TestMultiEnvelopeXfr|TestSingleEnvelopeXfrTLS|TestDuplicateA|TestDuplicateOwner|TestDuplicateTXT|TestDuplicateDomain)$'
go run -mod=vendor ./task_support/baseline
echo 'TARGET_COMPLETE'
