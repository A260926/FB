#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/crossfade/contract.c; fi
mkdir -p .task-build
flags=(-std=c99 -O1 -DMA_NO_DEVICE_IO -DMA_NO_DECODING -DMA_NO_ENCODING -DMA_NO_RESOURCE_MANAGER -DMA_NO_ENGINE -I.)
cc "${flags[@]}" -c task_support/miniaudio_impl.c -o .task-build/miniaudio.o
objects=(.task-build/miniaudio.o)
for src in extras/ma_crossfade/*.c; do
 [ -f "$src" ] || continue
 obj=".task-build/extra-$(basename "${src%.c}").o"
 cc "${flags[@]}" -c "$src" -o "$obj"
 objects+=("$obj")
done
if [[ $mode == target ]]; then
  cc "${flags[@]}" task_support/driver.c tests/crossfade/contract.c "${objects[@]}" -lm -lpthread -o .task-build/target
  output_log=$(mktemp "${TMPDIR:-/tmp}/ma-crossfade-log.XXXXXX")
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in timeline backpressure seek_graph; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
bash checks/baseline.sh
