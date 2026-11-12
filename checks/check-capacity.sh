#!/bin/bash
set -euo pipefail
test -s tests/capacity_test.cc || { echo 'missing tests/capacity_test.cc' >&2; exit 1; }
test -s src/capacity.cc || { echo 'missing src/capacity.cc' >&2; exit 1; }
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
bash checks/build.sh "$b"
c++ -std=c++11 -Isrc tests/capacity_test.cc "$b/libvoro++.a" -o "$b/test"
"$b/test" | tee "$b/out"
grep -Eq '^CAPACITY_OK cases=[1-9][0-9]*$' "$b/out"
bash checks/run-baseline.sh
