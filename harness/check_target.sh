#!/usr/bin/env bash
set -euo pipefail
log=$(mktemp "${TMPDIR:-/tmp}/vorbis-check.XXXXXX")
trap 'rm -f "$log"' EXIT
bash harness/go.sh test -count=1 -v -run '^(TestDecodeIntoWorkspace|TestBaseline|TestDecode|TestIMDCT)$' . | tee "$log"
grep -F -- '--- PASS: TestDecodeIntoWorkspace ' "$log" > /dev/null
