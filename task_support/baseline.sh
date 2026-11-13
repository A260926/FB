#!/bin/bash
set -euo pipefail
b=$(mktemp -d)
trap 'chmod -R u+w "$b"; rm -rf "$b"' EXIT
export GOWORK=off GOPROXY=off GOSUMDB=off GOMODCACHE="$b/mod" GOCACHE="${TASK_GOCACHE:-$b/build}" GOFLAGS=-mod=vendor
mkdir -p "$GOMODCACHE" "$GOCACHE"
go test -count=1 -v -run '^(TestPCMStart|TestDecoder_Duration|TestDecoder_FullPCMBuffer|TestDecoderRewind)$' . | tee "$b/out"
grep -Fq -- '--- PASS: TestPCMStart ' "$b/out"
grep -Fq 'AIFF_PCM_BASE_OK scalars=6 frames=3' "$b/out"
grep -Fq -- '--- PASS: TestDecoderRewind ' "$b/out"
grep -Fq -- '--- PASS: TestDecoder_FullPCMBuffer ' "$b/out"
echo AIFF_BASELINE_OK
