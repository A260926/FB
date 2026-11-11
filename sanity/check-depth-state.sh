#!/bin/sh
# Builds the actual TinyGL sources together with the depth-state regression in
# a private temporary directory, runs it, and leaves a small PPM story image
# (foreground / read-only depth draw / background overlay) next to this script.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=$(mktemp -d "${TMPDIR:-/tmp}/tgl-depth-state.XXXXXX")
trap 'rm -rf "$build"' EXIT HUP INT TERM

ppm="$root/sanity/depth_state.ppm"

cc -std=c99 -O2 -I"$root/include" "$root"/src/*.c \
	"$root/sanity/depth_state_test.c" -lm -o "$build/depth_state_test"

"$build/depth_state_test" "$ppm"

# the test itself prints DEPTH_STATE_OK only when every assertion held
if [ ! -s "$ppm" ]; then
	echo "missing PPM output: $ppm" >&2
	exit 1
fi
head -c 2 "$ppm" | grep -q P6
echo "PPM written: $ppm"
