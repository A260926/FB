#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s checks/playout.c; fi
mkdir -p .task-build/types
cat > .task-build/types/speexdsp_config_types.h <<'EOF'
#include <stdint.h>
typedef int16_t spx_int16_t;
typedef uint16_t spx_uint16_t;
typedef int32_t spx_int32_t;
typedef uint32_t spx_uint32_t;
EOF
cc -std=c11 -O1 -fwrapv -DFLOATING_POINT -DEXPORT= -Iinclude -I.task-build/types -c libspeexdsp/jitter.c -o .task-build/jitter.o
if [[ $mode == target ]]; then
  cc -std=c11 -O1 -fwrapv -I. -Iinclude -I.task-build/types task_support/driver.c checks/playout.c .task-build/jitter.o -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in wrap_timeline fragment_cursor ownership; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == baseline ]]; then
 cc -std=c11 -O1 -fwrapv -Iinclude -I.task-build/types checks/baseline.c .task-build/jitter.o -o .task-build/baseline
 .task-build/baseline
 cc -std=c11 -O1 -fwrapv -Iinclude -I.task-build/types libspeexdsp/testjitter.c .task-build/jitter.o -o .task-build/upstream
 .task-build/upstream > .task-build/upstream.log 2>&1
 if grep -Ei 'fail(ed)?' .task-build/upstream.log; then exit 1; fi
 grep -q 'Frozen sender:' .task-build/upstream.log
 echo JITTER_UPSTREAM_FROZEN_OK
fi
