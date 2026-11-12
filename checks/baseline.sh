#!/bin/bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
source checks/build-lib.sh
cc -std=gnu99 -O1 -Iinclude checks/baseline.c "$build"/*.o -o "$build/witness"
"$build/witness"
cc -std=gnu99 -O1 -Iinclude test/test.c "$build"/*.o -o "$build/old"
source_root="$PWD"
(cd "$build" && srcdir="$source_root" ./old)
echo LIBXLS_OLD_READER_REGRESSIONS_OK
