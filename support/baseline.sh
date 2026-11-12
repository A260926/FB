#!/bin/bash
set -euo pipefail
b=${CHECK_BUILD_DIR:-$(mktemp -d)}
bash support/compile.sh "$b"
lib=$(find "$b" -name libxlsxwriter.a -type f)
cc -O1 -Iinclude support/baseline.c "$lib" "$b/libz.a" -lm -o "$b/baseline"
"$b/baseline" "$b"
