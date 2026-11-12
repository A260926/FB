#!/bin/sh
set -eu
build=$(mktemp -d "${TMPDIR:-/tmp}/pgl-occlusion.XXXXXX")
trap 'rm -rf "$build"' EXIT HUP INT TERM
# Unoptimized and optimized builds, same as checks/run-baseline.sh.
# The PPM is written into checks/ so the coverage map outlives the temp dir.
for opt in 0 2; do
	cc -std=c99 -O"$opt" -I. checks/occlusion_query.c -lm -o "$build/occlusion"
	"$build/occlusion" checks/occlusion_query_coverage.ppm
done
