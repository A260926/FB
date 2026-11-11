#!/usr/bin/env bash
set -euo pipefail
log=$(mktemp)
trap 'rm -f "$log"' EXIT
bash author/go.sh test -count=1 -v -run '^(TestDecodeIntoWorkspace|TestAuthorBaseline|TestDecode|TestIMDCT)$' . | tee "$log"
grep -F -- '--- PASS: TestDecodeIntoWorkspace ' "$log" > /dev/null
