#!/bin/bash
set -euo pipefail
b=$1
objects=()
for file in bitstream blockswitch channels cpu_compute faac filtbank fft frame huff2 huffdata quantize ratecontrol sbr sbr_bitstream sbr_tables sbr_analysis resample stereo tns util; do
 cc -std=c11 -O2 -include task_support/build_config.h -Iinclude -Ilibfaac -c "libfaac/$file.c" -o "$b/$file.o"
 objects+=("$b/$file.o")
done
ar rcs "$b/libfaac.a" "${objects[@]}"
