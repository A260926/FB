#!/bin/bash
set -euo pipefail
mkdir -p .userfb-build
test -s sanity/input_ack_test.go
bash sanity/go.sh test -count=1 -v ./sanity -run '^TestInputAcknowledgement$' | tee .userfb-build/input-ack.log
grep -q -- '^--- PASS: TestInputAcknowledgement ' .userfb-build/input-ack.log
