#!/bin/bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
for src in src/*.c; do cc -std=c99 -O1 -Iinclude -Isrc -c "$src" -o "$build/$(basename "$src" .c).o"; done
cc -std=c99 -O1 -Iinclude checks/baseline.c "$build"/*.o -o "$build/witness"
"$build/witness"
cc -std=c99 -O1 -Iinclude t/harness.c "$build"/*.o -o "$build/old"
perl t/run-test.pl --executable "$build/old" t/69screen_reflow.test
echo VTERM_OLD_REFLOW_TESTS_OK
