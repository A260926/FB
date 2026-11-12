#!/usr/bin/env bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -r "$build"' EXIT
sources=(AudioFFT.cpp FFTConvolver.cpp TwoStageFFTConvolver.cpp Utilities.cpp)
c++ -std=c++11 -O2 -I. regress/current.cpp "${sources[@]}" -o "$build/current"
"$build/current" | tee "$build/current.log"
grep -Fq 'CONVOLUTION_BASELINE_OK selected=4' "$build/current.log"
if [[ $# -gt 0 ]]; then
 test "$#" -eq 2
 test -s "$1"
 c++ -std=c++11 -O2 -I. "$1" "${sources[@]}" -o "$build/target"
 "$build/target" | tee "$build/target.log"
 grep -Fq "$2" "$build/target.log"
fi
