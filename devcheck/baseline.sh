#!/bin/sh
#
# Fixed framerate two-pass regression check.
#
# Builds the actual Xvid sources, runs the existing fixed framerate 80 frame
# two-pass encode through devcheck/baseline.c and fails on any error.

set -e

here=`dirname "$0"`
root=`cd "$here/.." && pwd`
out="$root/.devcheck/baseline"

rm -rf "$out"
mkdir -p "$out"

cc=${CC:-cc}

lib=`sh "$here/build_lib.sh" "$out"`
if [ ! -f "$lib" ]; then
	echo "library build failed" >&2
	exit 1
fi

"$cc" -O2 -Wall -I"$root/xvidcore/src" \
	"$here/baseline.c" "$lib" -lm -o "$out/baseline"

"$out/baseline" "$out"
