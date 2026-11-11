#!/usr/bin/env bash
set -euo pipefail
log=$(mktemp)
trap 'rm -f "$log"' EXIT
bash author/go.sh test -count=1 -v -run '^(TestAuthorBaseline|TestDecode|TestIMDCT)$' . | tee "$log"
for name in TestAuthorBaseline TestDecode TestIMDCT; do grep -F -- "--- PASS: $name " "$log" > /dev/null; done
