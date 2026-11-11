#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
build=$1
cd "$build"
"$root/configure" --disable-shared --without-crypto --disable-magic --disable-cuckoo > configure.log 2>&1
make -j4 libyara.la test-arena > build.log 2>&1
./test-arena
printf 'YARA_OLD_ARENA_OK\n'
