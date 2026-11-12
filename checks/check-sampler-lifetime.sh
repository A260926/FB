#!/bin/bash
set -euo pipefail
test -s tests/SamplerLifetime.cpp || { echo 'missing tests/SamplerLifetime.cpp' >&2; exit 1; }
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
c++ -std=c++14 -O1 -g -fsanitize=address -fno-omit-frame-pointer -Iinclude tests/SamplerLifetime.cpp -o "$b/test"
ASAN_OPTIONS=detect_leaks=0 "$b/test" | tee "$b/out"
grep -Eq '^SAMPLER_LIFETIME_OK cases=[1-9][0-9]*$' "$b/out"
bash checks/reproduce.sh
bash checks/reproduce.sh flush
bash checks/run-baseline.sh
