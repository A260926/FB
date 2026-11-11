#!/usr/bin/env bash
set -euo pipefail
test -s capture_contract_test.go || { echo 'missing capture_contract_test.go'; exit 1; }
test -s capture-checks/verify-capture.sh || { echo 'missing capture-checks/verify-capture.sh'; exit 1; }
log=$(mktemp)
trap 'rm -f "$log"' EXIT
bash support/go-env.sh go test . -run '^TestMailCaptureWireContract$' -count=1 -v > "$log" 2>&1
cat "$log"
grep -q '^--- PASS: TestMailCaptureWireContract ' "$log"
bash support/go-env.sh bash capture-checks/verify-capture.sh > "$log" 2>&1
cat "$log"
grep -qx 'CAPTURE_DOTSTUFF_DETECTED' "$log"
grep -qx 'CAPTURE_SOURCE_ID_DETECTED' "$log"
bash support/go-env.sh go test . -run '^(TestReadAll.*|TestMailCapture)$' -count=1 -v
