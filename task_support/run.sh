#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/playout_test.c; fi
mkdir -p .task-build/speex
cat > .task-build/speex/speexdsp_config_types.h <<'TYPES'
#include <stdint.h>
typedef int16_t spx_int16_t;
typedef uint16_t spx_uint16_t;
typedef int32_t spx_int32_t;
typedef uint32_t spx_uint32_t;
TYPES
cc -std=c99 -O2 -DFLOATING_POINT -DEXPORT= -I.task-build -Iinclude -Ilibspeexdsp -c libspeexdsp/jitter.c -o .task-build/jitter.o
cc -std=c99 -O2 -DFLOATING_POINT -DEXPORT= -I.task-build -Iinclude -Ilibspeexdsp -c libspeexdsp/resample.c -o .task-build/resample.o
cc -std=c99 -O2 -I.task-build -Iinclude task_support/baseline.c .task-build/jitter.o .task-build/resample.o -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s libspeexdsp/playout.c
test -s include/speex/speex_playout.h
cc -std=c99 -O2 -DFLOATING_POINT -DEXPORT= -I. -I.task-build -Iinclude -Ilibspeexdsp -include speex/speex_playout.h -c libspeexdsp/playout.c -o .task-build/playout.o
cc -std=c99 -O2 -I. -I.task-build -Iinclude task_support/api_consumer.c .task-build/playout.o .task-build/jitter.o .task-build/resample.o -lm -o .task-build/api_consumer
.task-build/api_consumer
cc -std=c99 -O2 -I. -I.task-build -Iinclude tests/playout_test.c task_support/driver.c .task-build/playout.o .task-build/jitter.o .task-build/resample.o -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in timeline elastic lifecycle; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
