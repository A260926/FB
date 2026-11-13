#!/bin/sh
set -eu
sh devcheck/build.sh
mkdir -p devcheck/out
cc -std=c99 -Isrc -Idevcheck/build devcheck/baseline.c devcheck/build/libkvazaar.a -lm -lpthread -o devcheck/out/baseline
devcheck/out/baseline
