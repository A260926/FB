#!/usr/bin/env bash
set -euo pipefail
root=$PWD
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
cmake -S . -B "$build" -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=ON -DBUILD_PROGRAMS=OFF -DBUILD_EXAMPLES=OFF -DENABLE_EXTERNAL_LIBS=OFF -DENABLE_MPEG=OFF
cmake --build "$build" --target sndfile command_test --parallel 4
cc -std=c99 -I include support/baseline.c "$build/libsndfile.a" -lm -o "$build/baseline"
mkdir "$build/run"
cd "$build/run"
"$build/baseline"
"$build/command_test" cue
echo SNDFILE_OLD_CUE_PASS
