#!/bin/bash
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOFLAGS=-mod=vendor
set -euo pipefail
test -s playlist_test.go
out=$(bash sanity/go.sh go test -count=1 -v -run '^(TestPlaylistPlayback|TestPlaylistSeek)$' .)
printf '%s\n' "$out"
for name in TestPlaylistPlayback TestPlaylistSeek; do grep -q -- "--- PASS: $name " <<<"$out"; done
bash sanity/go.sh go test -count=1 -run '^(TestBaseline|TestDecoderSeek|TestDecoderRewind|TestDecoder_ReadMetadata|TestEncoderRoundTrip)$' .
