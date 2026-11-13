#!/bin/sh
#
# Variable framerate two-pass check.
#
# Builds the actual Xvid sources, compiles and runs tests/vfr_2pass.c and also
# builds and runs the example. Any missing file or failing process makes this
# script exit non-zero.

set -e

here=`dirname "$0"`
root=`cd "$here/.." && pwd`
out="$root/.devcheck/target"

rm -rf "$out"
mkdir -p "$out/tmp"

cc=${CC:-cc}

if [ ! -f "$root/tests/vfr_2pass.c" ]; then
	echo "missing tests/vfr_2pass.c" >&2
	exit 1
fi
if [ ! -f "$root/xvidcore/examples/xvid_vfr_2pass.c" ]; then
	echo "missing xvidcore/examples/xvid_vfr_2pass.c" >&2
	exit 1
fi

lib=`sh "$here/build_lib.sh" "$out"`
if [ ! -f "$lib" ]; then
	echo "library build failed" >&2
	exit 1
fi

"$cc" -O2 -Wall -I"$root/xvidcore/src" \
	"$root/tests/vfr_2pass.c" "$lib" -lm -o "$out/vfr_2pass"
"$out/vfr_2pass" "$out/tmp"

"$cc" -O2 -Wall -I"$root/xvidcore/src" \
	"$root/xvidcore/examples/xvid_vfr_2pass.c" "$lib" -lm -o "$out/vfr_example"
"$out/vfr_example" "$out/tmp"

if [ ! -s "$out/tmp/vfr_example.m4v" ]; then
	echo "example produced no bitstream" >&2
	exit 1
fi
if [ ! -s "$out/tmp/vfr_example.stats" ]; then
	echo "example produced no first pass file" >&2
	exit 1
fi
