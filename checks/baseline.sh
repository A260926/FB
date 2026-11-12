#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
./.check-build/check-baseline
(cd .check-build/test && ./charls-test --gtest_filter='compliance_test.*:scan_decoder_test.*:regular_mode_context_test.*:run_mode_context_test.*')
echo CHARLS_BASELINE_OK
