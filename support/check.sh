#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test -s support/ripple_delete_test.cpp
bash support/baseline.sh
cmake --build .build --target regression -j 4
.build/support/regression
