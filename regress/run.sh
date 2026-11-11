#!/usr/bin/env bash
set -euo pipefail
src=${1:-regress/baseline.c}
test -s "$src"
build=$(mktemp -d ./.dvb-build.XXXXXX)
trap 'rm -rf "$build"' EXIT
printf '#define HAVE_STDINT_H 1\n#define HAVE_INTTYPES_H 1\n' > "$build/config.h"
cc -std=gnu99 -O1 -g -I "$build" -I src "$src" src/dvbpsi.c src/psi.c src/demux.c src/descriptor.c src/tables/eit.c src/tables/eit_extended.c src/descriptors/dr_4e.c -o "$build/test"
"$build/test"
