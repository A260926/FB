#!/bin/bash
set -euo pipefail
if [[ ! -s .checks-build-dir ]] || [[ ! -d "$(cat .checks-build-dir)" ]]; then
 mktemp -d > .checks-build-dir
fi
b=$(cat .checks-build-dir)
cmake -S checks -B "$b" -DCMAKE_BUILD_TYPE=Release
cmake --build "$b" --target origin -j2
