#!/bin/bash
set -euo pipefail
ulimit -c 0
# Regenerate the known nonzero reference through the unchanged seekable path.
bash task_support/baseline.sh normal
mkdir -p .task-build/required-stream
rm -f .task-build/required-stream/out.bit .task-build/required-stream/recon.yuv .task-build/required-stream/decoded.yuv
set +e
cat .task-build/baseline/input.yuv | .task-build/Thorenc -stream_input 1 -if - -of .task-build/required-stream/out.bit -rf .task-build/required-stream/recon.yuv -width 64 -height 64 -n 9 -qp 34 -log2_sb_size 6 -num_reorder_pics 3 -max_num_ref 2 -dyadic_coding 1 -snrcalc 0 -cdef 0 > .task-build/required-stream/encode.log 2>&1
stream_exit=$?
set -e
cat .task-build/required-stream/encode.log
[[ $stream_exit == 0 ]] || exit "$stream_exit"
test -s .task-build/required-stream/out.bit
test "$(wc -c < .task-build/required-stream/recon.yuv)" -eq 55296
.task-build/Thordec .task-build/required-stream/out.bit .task-build/required-stream/decoded.yuv > .task-build/required-stream/decode.log 2>&1
test "$(wc -c < .task-build/required-stream/decoded.yuv)" -eq 55296
cmp .task-build/baseline/out.bit .task-build/required-stream/out.bit
cmp .task-build/baseline/recon.yuv .task-build/required-stream/recon.yuv
cmp .task-build/required-stream/recon.yuv .task-build/required-stream/decoded.yuv
printf 'REQUIRED_STREAM_INPUT_OK frames=9 stdin=pipe coding_order=preserved pixels=equal\n'
