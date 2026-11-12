#!/bin/sh
set -eu
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT HUP INT TERM
${CC:-cc} -std=c99 -O2 -I. -Iinclude -Isrc checks/baseline.c src/loaders/itsex.c src/hio.c src/memio.c src/dataio.c -o "$build/baseline"
"$build/baseline"
${CC:-cc} -std=c99 -O2 -I. -Iinclude -Isrc checks/old_samples.c src/loaders/itsex.c src/hio.c src/memio.c src/dataio.c src/md5.c -o "$build/old_samples"
"$build/old_samples"
