#!/usr/bin/env bash
# Builds the C++ baseline and the ripple-delete regression suite, and runs
# both. Fails (non-zero) if the regression suite did not actually execute.
set -euo pipefail
cd "$(dirname "$0")/.."
test -s support/ripple_delete_test.cpp
bash support/baseline.sh
cmake --build .build --target regression -j 4
out="$(.build/support/regression)"
printf '%s\n' "$out"
# Confirm the new regression really ran rather than silently doing nothing.
printf '%s\n' "$out" | grep -q '^RIPPLE_DELETE_TESTS_OK checks=[0-9]\+$'
