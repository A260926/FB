#!/usr/bin/env bash
set -euo pipefail
log=$(mktemp)
trap 'rm -f "$log"' EXIT
bash author/go.sh test -count=1 -v -run '^(TestSSNDExtent|TestAuthorBaseline|TestDecoder_Duration|TestDecoderRewind|TestDecoder_FullPCMBuffer)$' . | tee "$log"
grep -F -- '--- PASS: TestSSNDExtent ' "$log" > /dev/null
