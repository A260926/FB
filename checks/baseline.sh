#!/bin/sh
set -eu
.build/baseline .build/normal.mod
.build/converter .build/normal.mod .build/normal.wav
# Three orders, 64 rows/order, six ticks/row at 125 BPM, 44100 Hz.
# Converter must stop at the first global loop, exactly 1,016,064 stereo frames.
test "$(wc -c < .build/normal.wav | tr -d ' ')" = 4064300
printf '%s\n' 'UPSTREAM_CONVERTER_OK frames=1016064'
