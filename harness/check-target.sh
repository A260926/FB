#!/bin/bash
set -euo pipefail
test -s test/stream-contract.mjs
test -s test/run-stream-contract.mjs
node --test --test-reporter=tap --test-name-pattern='^stream acquisition contract$' test/stream-contract.mjs | tee .build/stream-normal.tap
node harness/check-normal-report.mjs
node test/run-stream-contract.mjs | tee .build/stream-target.log
for marker in STREAM_MODEL_OK STREAM_ATTRIBUTE_CAUGHT STREAM_RANGE_CAUGHT STREAM_END_CAUGHT; do grep -qx "$marker" .build/stream-target.log; done
bash harness/baseline.sh
