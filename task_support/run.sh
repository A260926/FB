#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
case "$mode" in baseline|target) ;; *) echo 'unknown mode' >&2; exit 2;; esac
if [ "$mode" = target ]; then
  test -s tests/src/spectral/contract_hpss.c || { echo 'missing contract_hpss.c' >&2; exit 2; }
fi
bash task_support/build.sh
link=(-L.task-build/build/src "-Wl,-rpath,$PWD/.task-build/build/src" -laubio -lm)
if [ "$mode" = target ]; then
  cc -std=c99 -Isrc -I. -Itask_support tests/src/spectral/contract_hpss.c task_support/runner.c "${link[@]}" -o .task-build/contract
  set +e
  .task-build/contract >.task-build/target.stdout 2>.task-build/target.stderr
  status=$?
  set -e
  cat .task-build/target.stdout
  cat .task-build/target.stderr >&2
  test "$status" -eq 0 || exit "$status"
  for group in medians stems stream; do
    test "$(grep -Ec "^DONE $group cases=[1-9][0-9]* checks=[1-9][0-9]*$" .task-build/target.stdout)" -eq 1 || exit 79
  done
  test "$(grep -Fxc 'HPSS_ALL_DONE groups=3' .task-build/target.stdout)" -eq 1 || exit 79
fi
cc -std=c99 -Isrc -Itask_support task_support/baseline.c "${link[@]}" -o .task-build/baseline
.task-build/baseline
for testname in fft phasevoc tss; do
  cc -std=c99 -Isrc "tests/src/spectral/test-$testname.c" "${link[@]}" -o ".task-build/old-$testname"
  ".task-build/old-$testname" >".task-build/old-$testname.stdout" 2>".task-build/old-$testname.stderr"
  echo "AUBIO_OLD_TEST_OK $testname"
done
