#!/bin/sh
set -eu
sh devcheck/build.sh
mkdir -p devcheck/out
test -f tests/roi_rate_control.c
cc -std=c99 -Isrc -Idevcheck/build tests/roi_rate_control.c devcheck/build/libkvazaar.a -lm -lpthread -o devcheck/out/roi_rate_control
for group in allocation feedback mapping compatibility; do
 devcheck/out/roi_rate_control "$group"
done
