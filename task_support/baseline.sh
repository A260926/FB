#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
.build/src/sox_sample_test
.build/pcm_probe generate .build/input.s32
for b in 64 8192; do
 .build/src/sox -D --buffer "$b" -t raw -e signed-integer -b 32 -r 48000 -c 2 .build/input.s32 -t raw -e signed-integer -b 32 .build/output-$b.s32 splice -t 256s,8s,0s 768s,8s,0s
 .build/pcm_probe check .build/output-$b.s32
done
cmp .build/output-64.s32 .build/output-8192.s32
printf 'SOX_BUFFER_EQUIVALENCE_OK buffers=64,8192\n'
