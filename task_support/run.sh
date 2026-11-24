#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/rate44100/contract.c; fi
mkdir -p .task-build
cmake -S . -B .task-build/opus -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DOPUS_DISABLE_INTRINSICS=ON -DOPUS_BUILD_SHARED_LIBRARY=OFF -DOPUS_BUILD_TESTING=OFF -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_FIXED_POINT=OFF -DOPUS_ENABLE_FLOAT_API=ON -DOPUS_DRED=OFF -DOPUS_DEEP_PLC=OFF -DOPUS_OSCE=OFF -DOPUS_QEXT=OFF
cmake --build .task-build/opus --parallel 2
cc -std=c99 -O1 -I. -Iinclude -Icelt -Isilk -Isilk/float -I.task-build/opus task_support/normal.c .task-build/opus/libopus.a -lm -o .task-build/normal
if [[ $mode == target ]]; then
  test -s src/opus_resampler_44100.c
test -s src/opus_resampler_44100.h
test -s examples/decode_44100.c
ar t .task-build/opus/libopus.a > .task-build/archive-members.txt
grep -Eq '^opus_resampler_44100.c.o$' .task-build/archive-members.txt
cc -std=c99 -O1 -I. -Iinclude -Icelt -Isilk -Isilk/float -I.task-build/opus -DHAVE_CONFIG_H -include src/opus_resampler_44100.h -c src/opus_resampler_44100.c -o .task-build/rate44100.o
cc -std=c99 -O1 -I. -Iinclude -Icelt -Isilk -Isilk/float -I.task-build/opus -Werror=implicit-function-declaration task_support/api_consumer.c .task-build/rate44100.o .task-build/opus/libopus.a -lm -o .task-build/api_consumer
.task-build/api_consumer
cc -std=c99 -O1 -I. -Iinclude -Icelt -Isilk -Isilk/float -I.task-build/opus examples/decode_44100.c .task-build/rate44100.o .task-build/opus/libopus.a -lm -o .task-build/example
rm -f .task-build/example.out
.task-build/example > .task-build/example.out
cat .task-build/example.out
grep -qx OPUS_44100_DEMO_OK .task-build/example.out
cc -std=c99 -O1 -I. -Iinclude -Icelt -Isilk -Isilk/float -I.task-build/opus task_support/driver.c tests/rate44100/contract.c .task-build/rate44100.o .task-build/opus/libopus.a -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in clock recovery multistream; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == baseline ]]; then .task-build/normal --gap; else .task-build/normal; fi
