#!/bin/bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
bash checks/build-lib.sh "$build" checks/baseline.c
"$build/program"
bash checks/build-lib.sh "$build" test/test-integers.c
"$build/program"
echo EXIF_INTEGER_TESTS_OK
