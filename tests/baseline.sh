#!/usr/bin/env bash
set -euo pipefail
b=$(mktemp -d)
trap 'find "$b" -type f -delete; rmdir "$b"' EXIT
cc -O2 -std=gnu11 minimp4_test.c -o "$b/minimp4" -lm
"$b/minimp4" vectors/foreman.264 "$b/regular.mp4"
cmp "$b/regular.mp4" vectors/out_ref.mp4
"$b/minimp4" -s vectors/foreman.264 "$b/sequential.mp4"
cmp "$b/sequential.mp4" vectors/out_sequential_ref.mp4
"$b/minimp4" -f vectors/foreman.264 "$b/fragmented.mp4"
cmp "$b/fragmented.mp4" vectors/out_fragmentation_ref.mp4
"$b/minimp4" vectors/foreman_slices.264 "$b/slices.mp4"
cmp "$b/slices.mp4" vectors/out_slices_ref.mp4
"$b/minimp4" -d vectors/out_ref.mp4 "$b/demux.264"
cmp "$b/demux.264" vectors/foreman.264
printf 'minimp4 legacy mux/demux: ok\n'
