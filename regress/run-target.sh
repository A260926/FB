#!/bin/bash
set -euo pipefail
test -s regress/freeform.cpp
test -s regress/check-freeform.sh
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
bash regress/build-run.sh
bash regress/build-run.sh regress/freeform.cpp >"$b/direct" 2>&1 || { cat "$b/direct"; exit 1; }
cat "$b/direct"
for tag in source-state rational-geometry discontinuities invalid-blocks; do grep -Eq "^PASS $tag cases=[1-9][0-9]*$" "$b/direct"; done
grep -Fx TINYOBJ_FREEFORM_OK "$b/direct"
bash regress/check-freeform.sh >"$b/result" 2>&1 || { cat "$b/result"; exit 1; }
cat "$b/result"
for tag in source-state rational-geometry discontinuities invalid-blocks; do grep -Eq "^PASS $tag cases=[1-9][0-9]*$" "$b/result"; done
grep -Fx TINYOBJ_FREEFORM_OK "$b/result"
