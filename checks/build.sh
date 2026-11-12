#!/usr/bin/env bash
set -euo pipefail
# Source this helper with b set to a temporary build directory.
for name in denoise kiss_fft pitch celt_lpc rnn rnn_data rnn_reader; do
 cc -std=c99 -D_DEFAULT_SOURCE -O1 -Iinclude -Isrc -c "src/$name.c" -o "$b/$name.o"
done
