#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test -f test/stream_decoder_contract_test.cpp || { echo 'MISSING test/stream_decoder_contract_test.cpp' >&2; exit 3; }
bash checks/install.sh
log=$(mktemp)
trap 'rm -f "$log"' EXIT
./.check-build/check-stream-contract --gtest_filter='stream_input.*:stream_prediction.*:stream_restart.*:stream_lifecycle.*' | tee "$log"
for suite in stream_input stream_prediction stream_restart stream_lifecycle; do
  grep -Eq "^EXECUTED_SUITE $suite passed=[1-9][0-9]*$" "$log"
done
grep -Eq '^STREAM_CONTRACT_EXECUTED tests=[1-9][0-9]*$' "$log"
bash checks/baseline.sh
echo CHARLS_STREAM_TARGET_OK
