#!/bin/bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
bash checks/build-lib.sh "$build"
cc -std=c11 -O1 -Isrc -I"$build/src" checks/baseline.c "$build/src/libleptonica.a" -lm -o "$build/base"
"$build/base"
cc -std=c11 -O1 -Isrc -I"$build/src" checks/old_numeric.c "$build/src/libleptonica.a" -lm -o "$build/old"
"$build/old"
echo LEPT_NO_CODECS_BUILD_OK
