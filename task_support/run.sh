#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_support/consumer.c; fi
mkdir -p .task-build
for unit in bit.c fixed.c frame.c huffman.c layer12.c layer3.c stream.c synth.c timer.c decoder.c version.c; do
  cc -std=c99 -O1 -DFPM_64BIT -DOPT_ACCURACY -DHAVE_ASSERT_H -DHAVE_LIMITS_H -I. -include task_support/work.h -c "$unit" -o ".task-build/${unit%.c}.o"
done
rm -f .task-build/libmad.a
ar rcs .task-build/libmad.a .task-build/bit.o .task-build/fixed.o .task-build/frame.o .task-build/huffman.o .task-build/layer12.o .task-build/layer3.o .task-build/stream.o .task-build/synth.o .task-build/timer.o .task-build/decoder.o .task-build/version.o
cc -std=c99 -O1 -DFPM_64BIT -DOPT_ACCURACY -DHAVE_ASSERT_H -DHAVE_LIMITS_H -I. -include task_support/work.h task_support/baseline.c task_support/fixture.c task_support/work.c .task-build/libmad.a -o .task-build/normal
if [[ $mode == target ]]; then
  test -s granule.h
test -s granule.c
cc -std=c99 -O1 -DFPM_64BIT -DOPT_ACCURACY -DHAVE_ASSERT_H -DHAVE_LIMITS_H -I. -include task_support/work.h -include granule.h -c granule.c -o .task-build/granule.o
ar rcs .task-build/libmad.a .task-build/granule.o
cc -std=c99 -O1 -DFPM_64BIT -DOPT_ACCURACY -DHAVE_ASSERT_H -DHAVE_LIMITS_H -I. -include task_support/work.h -include granule.h task_support/api_consumer.c task_support/fixture.c task_support/work.c .task-build/libmad.a -o .task-build/api_consumer
.task-build/api_consumer
cc -std=c99 -O1 -DFPM_64BIT -DOPT_ACCURACY -DHAVE_ASSERT_H -DHAVE_LIMITS_H -I. -include task_support/work.h -include granule.h task_support/driver.c task_support/consumer.c task_support/fixture.c task_support/work.c .task-build/libmad.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in stream suspend lifecycle; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/normal
