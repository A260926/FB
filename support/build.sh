#!/bin/bash
set -euo pipefail
sources=(common/vp9_header_parser.cc common/vp9_level_stats.cc common/libwebm_util.cc)
for source in common/vp9_tiles.cc; do
 if test -f "$source"; then sources+=("$source"); fi
done
for source in "${sources[@]}"; do
 name=${source##*/}
 c++ -std=c++17 -O2 -I. -c "$source" -o "$1/${name%.cc}.o"
done
ar rcs "$1/libvp9-analysis.a" "$1/"*.o
