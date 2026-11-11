#!/usr/bin/env bash
set -euo pipefail
file=$1
test -s "$file"
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
c++ -std=c++17 -O2 -DALREADY_CONFIGURED -DUSE_BQRESAMPLER -DUSE_BUILTIN_FFT -DNO_THREADING -DNO_TIMING -DNO_THREAD_CHECKS -DLACK_SINCOS -I. \
 "$file" single/RubberBandSingle.cpp -pthread -o "$b/check"
"$b/check"
