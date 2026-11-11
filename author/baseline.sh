#!/usr/bin/env bash
set -euo pipefail
bash author/go.sh mod download
log=$(mktemp)
trap 'rm -f "$log"' EXIT
bash author/go.sh test -count=1 -v -run '^(TestAuthorBaseline|TestDecoder_Duration|TestDecoderRewind|TestDecoder_FullPCMBuffer)$' . | tee "$log"
for name in TestAuthorBaseline TestDecoder_Duration TestDecoderRewind TestDecoder_FullPCMBuffer; do
 grep -F -- "--- PASS: $name " "$log" > /dev/null
done
