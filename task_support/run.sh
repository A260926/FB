#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/stream_input_cases.c; fi
scripts/lbd_to_hbd.sh common/common_kernels.c common/common_kernels_gen.c
scripts/lbd_to_hbd.sh enc/enc_kernels.c enc/enc_kernels_gen.c
mkdir -p .task-build
if [[ $mode == target ]]; then
  test -s enc/input_stream.c
  test -s enc/input_stream.h
  make -B -o common/common_kernels_gen.c -o enc/enc_kernels_gen.c -n ARCH=portable ENCODER_PROGRAM=.task-build/Thorenc DECODER_PROGRAM=.task-build/Thordec all > .task-build/link-plan.log
  awk '/-o \.task-build\/Thorenc/ {for(i=1;i<=NF;i++) if($i=="enc/input_stream.o") found=1} END {exit !found}' .task-build/link-plan.log
  make -B -o common/common_kernels_gen.c -o enc/enc_kernels_gen.c -j4 ARCH=portable ENCODER_PROGRAM=.task-build/Thorenc DECODER_PROGRAM=.task-build/Thordec CFLAGS='-std=c99 -O1 -Wall -I common -I . -DCONFIG_BIG_ENDIAN=0 -include enc/input_stream.h' all > .task-build/build.log 2>&1 || { tail -30 .task-build/build.log; exit 1; }
  test -s enc/input_stream.o
else
  make -B -o common/common_kernels_gen.c -o enc/enc_kernels_gen.c -j4 ARCH=portable ENCODER_PROGRAM=.task-build/Thorenc DECODER_PROGRAM=.task-build/Thordec CFLAGS='-std=c99 -O1 -Wall -I common -I . -DCONFIG_BIG_ENDIAN=0' all > .task-build/build.log 2>&1 || { tail -30 .task-build/build.log; exit 1; }
fi
if [[ $mode == target ]]; then
  test -s examples/stream_export.c
test -s task_support/require_stream_mode.sh
bash task_support/require_stream_mode.sh
cc -std=c99 -O1 -D_POSIX_C_SOURCE=200809L examples/stream_export.c -o .task-build/stream_export
mkdir -p .task-build/example
rm -f .task-build/example/out.bit .task-build/example/decoded.yuv .task-build/example/verify.yuv
.task-build/stream_export .task-build/example
test -s .task-build/example/out.bit
test -s .task-build/example/decoded.yuv
.task-build/Thordec .task-build/example/out.bit .task-build/example/verify.yuv > .task-build/example/decode.log 2>&1
test "$(wc -c < .task-build/example/verify.yuv)" -eq 55296
cmp .task-build/example/decoded.yuv .task-build/example/verify.yuv
objects=$(find enc common -name '*.o' ! -name mainenc.o -print)
test -n "$objects"
cc -std=c99 -O1 -D_POSIX_C_SOURCE=200809L -I . -I common task_support/driver.c tests/stream_input_cases.c $objects -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in StreamInput StreamReorder StreamProcess; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == baseline ]]; then bash task_support/baseline.sh full; else bash task_support/baseline.sh normal; fi
