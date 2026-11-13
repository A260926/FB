#!/bin/sh
set -eu
bash task_support/baseline.sh
[ -s tests/spectral_freeze_test.cpp ] || { echo 'missing tests/spectral_freeze_test.cpp' >&2; exit 2; }
c++ -std=c++17 -O2 -Wall -Wextra -I. -Itask_support/vendor/linear/include tests/spectral_freeze_test.cpp -o .task-build/freeze-test
.task-build/freeze-test > .task-build/freeze.log
cat .task-build/freeze.log
grep -Eq '^SPECTRAL_FREEZE_OK cases=[1-9][0-9]*$' .task-build/freeze.log
