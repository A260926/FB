#!/bin/bash
set -euo pipefail
ulimit -c 0
mkdir -p .task-build/baseline
cc -std=c99 -O1 task_support/fixture.c -o .task-build/fixture
rm -f .task-build/baseline/input.yuv .task-build/baseline/out.bit .task-build/baseline/recon.yuv .task-build/baseline/decoded.yuv
.task-build/fixture .task-build/baseline/input.yuv
.task-build/Thorenc -if .task-build/baseline/input.yuv -of .task-build/baseline/out.bit -rf .task-build/baseline/recon.yuv -width 64 -height 64 -n 9 -qp 34 -log2_sb_size 6 -num_reorder_pics 3 -max_num_ref 2 -dyadic_coding 1 -snrcalc 0 -cdef 0 > .task-build/baseline/encode.log 2>&1
.task-build/Thordec .task-build/baseline/out.bit .task-build/baseline/decoded.yuv > .task-build/baseline/decode.log 2>&1
test -s .task-build/baseline/out.bit
test "$(wc -c < .task-build/baseline/recon.yuv)" -eq 55296
test "$(wc -c < .task-build/baseline/decoded.yuv)" -eq 55296
cmp .task-build/baseline/recon.yuv .task-build/baseline/decoded.yuv
printf 'PRODUCTION_ROUNDTRIP frames=9 pixels_bytes=%s bitstream_bytes=%s\n' "$(wc -c < .task-build/baseline/decoded.yuv | tr -d ' ')" "$(wc -c < .task-build/baseline/out.bit | tr -d ' ')"
awk '/^[ ]*[0-9]+ [IPB] / {printf "%s%s", sep, $1; sep=","} END {print ""}' .task-build/baseline/encode.log
if [[ ${1:-full} == full ]]; then
  set +e
  .task-build/Thorenc -stream_input 1 -if .task-build/baseline/input.yuv -of .task-build/baseline/unsupported.bit > .task-build/baseline/unsupported.log 2>&1
  status=$?
  set -e
  test "$status" -ne 0
  grep -q 'Unknown parameter: -stream_input' .task-build/baseline/unsupported.log
  printf 'STARTING_POINT stream_input=unsupported exit=%s\n' "$status"
fi
