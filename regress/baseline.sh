#!/usr/bin/env bash
set -euo pipefail
bash regress/go.sh mod download
log=$(mktemp)
trap 'rm -f "$log"' EXIT
bash regress/go.sh test -count=1 -v -run '^(TestBaseline|TestDecoder_Duration|TestDecoderRewind|TestDecoder_FullPCMBuffer)$' . | tee "$log"
for name in TestBaseline TestDecoder_Duration TestDecoderRewind TestDecoder_FullPCMBuffer; do
 grep -F -- "--- PASS: $name " "$log" > /dev/null
done
