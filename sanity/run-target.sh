#!/bin/bash
set -euo pipefail
mkdir -p .build
test -s sanity/input_ack_test.go
bash sanity/go.sh test -count=1 -v ./sanity -run '^TestInputAcknowledgement$' | tee .build/input-ack.log
grep -q -- '^--- PASS: TestInputAcknowledgement ' .build/input-ack.log
