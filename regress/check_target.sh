#!/usr/bin/env bash
set -euo pipefail
log=$(mktemp)
trap 'rm -f "$log"' EXIT
bash regress/go.sh test -count=1 -v -run '^(TestSSNDExtent|TestBaseline|TestDecoder_Duration|TestDecoderRewind|TestDecoder_FullPCMBuffer)$' . | tee "$log"
grep -F -- '--- PASS: TestSSNDExtent ' "$log" > /dev/null
