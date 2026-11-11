#!/usr/bin/env bash
set -euo pipefail
test -s metadata_main_entity_test.go || { echo 'missing metadata_main_entity_test.go'; exit 1; }
log=$(mktemp)
trap 'rm -f "$log"' EXIT
bash support/go-env.sh go test . -run '^TestMainEntity(Extraction|Isolation)$' -count=1 -v > "$log" 2>&1
cat "$log"
grep -q '^--- PASS: TestMainEntityExtraction ' "$log"
grep -q '^--- PASS: TestMainEntityIsolation ' "$log"
bash support/go-env.sh go test . -run '^Test_Metadata($|_Titles$|_Authors$|_URLs$)' -count=1 -v
