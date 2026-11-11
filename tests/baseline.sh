#!/usr/bin/env bash
set -euo pipefail
b=$(mktemp -d)
trap 'find "$b" -type f -delete; rmdir "$b"' EXIT
cc -std=c99 -O2 -Iinclude tests/normaliz_baseline.c src/filter/normaliz0r/normaliz0r.c -lm -o "$b/witness"
"$b/witness"
c++ -std=c++11 -O2 -Iinclude tests/delay_baseline.cpp src/filter/delay0r/delay0r.cpp -o "$b/delay"
"$b/delay"
