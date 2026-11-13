#!/bin/bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
cc -std=c99 -O1 -Isljit_src docs/tutorial/sources/first_program.c sljit_src/sljitLir.c -o "$b/first"
"$b/first" > "$b/output"
cat "$b/output"
grep -qx 'func return 15' "$b/output"
echo SLJIT_NATIVE_ADD15_OK
cc -std=c99 -O1 -DSLJIT_HAVE_CONFIG_PRE=1 -Itest_src -Isljit_src test_src/sljitMain.c test_src/sljitTest.c sljit_src/sljitLir.c -lpthread -o "$b/oldtests"
"$b/oldtests" -s
echo SLJIT_UPSTREAM_138_OK
