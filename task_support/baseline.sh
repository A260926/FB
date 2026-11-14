#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d "$PWD/.check-baseline.XXXXXX")
trap 'rm -rf "$build"' EXIT
cc -std=gnu99 -O2 task_support/baseline.c -o "$build/baseline"
"$build/baseline"
cc -std=gnu99 -O2 minimp4_test.c -o "$build/minimp4"
"$build/minimp4" vectors/foreman.264 "$build/default.mp4"
cmp "$build/default.mp4" vectors/out_ref.mp4
printf 'OLD_PASS mux_default\n'
"$build/minimp4" -s vectors/foreman.264 "$build/sequential.mp4"
cmp "$build/sequential.mp4" vectors/out_sequential_ref.mp4
printf 'OLD_PASS mux_sequential\n'
"$build/minimp4" -f vectors/foreman.264 "$build/fragmented.mp4"
cmp "$build/fragmented.mp4" vectors/out_fragmentation_ref.mp4
printf 'OLD_PASS mux_fragmented\n'
"$build/minimp4" vectors/foreman_slices.264 "$build/slices.mp4"
cmp "$build/slices.mp4" vectors/out_slices_ref.mp4
printf 'OLD_PASS mux_slices\n'
for mode in default sequential slices; do
    case "$mode" in
        default) input=out_ref.mp4; expected=foreman.264 ;;
        sequential) input=out_sequential_ref.mp4; expected=foreman.264 ;;
        slices) input=out_slices_ref.mp4; expected=foreman_slices.264 ;;
    esac
    "$build/minimp4" -d "vectors/$input" "$build/$mode.264"
    cmp "$build/$mode.264" "vectors/$expected"
    printf 'OLD_PASS demux_%s\n' "$mode"
done
printf 'MINIMP4_OLD_COMPLETE comparisons=7\n'
