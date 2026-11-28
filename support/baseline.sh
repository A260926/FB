#!/bin/sh
set -eu
mkdir -p build/test-tmp
TMPDIR="$(pwd)/build/test-tmp"
export TMPDIR
cc -std=c99 -UNDEBUG -Ilibarchive support/baseline.c build/libarchive/libarchive.a -o build/baseline
./build/baseline
./build/bin/libarchive_test -r libarchive/test test_write_disk_secure
