#!/bin/bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
bash support/build.sh "$b"
c++ -std=c++17 -x c++ -Isupport -I. -c support/driver.c -o "$b/driver.o"
c++ -std=c++17 -Isupport -I. support/target_cases.cc "$b/driver.o" "$b/libvp9-analysis.a" -o "$b/cases"
"$b/cases" | tee "$b/output"
awk -v style=A -v groups="headers tiles packets" -f support/verify_output.awk "$b/output"
bash support/baseline.sh
