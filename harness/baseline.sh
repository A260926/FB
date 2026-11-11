#!/usr/bin/env bash
set -euo pipefail
log=$(mktemp)
trap 'rm -f "$log"' EXIT
bash harness/go.sh test -count=1 -v -run '^(TestBaseline|TestDecode|TestIMDCT)$' . | tee "$log"
for name in TestBaseline TestDecode TestIMDCT; do grep -F -- "--- PASS: $name " "$log" > /dev/null; done
