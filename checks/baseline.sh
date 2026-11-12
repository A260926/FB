#!/bin/sh
set -eu
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT HUP INT TERM
${CXX:-c++} -std=c++11 -O2 -w -Icodec -x c++ checks/baseline.cpp codec/ALACEncoder.cpp codec/ALACDecoder.cpp codec/EndianPortable.c codec/ALACBitUtilities.c codec/ag_enc.c codec/ag_dec.c codec/dp_enc.c codec/dp_dec.c codec/matrix_enc.c codec/matrix_dec.c -o "$build/baseline"
"$build/baseline"
