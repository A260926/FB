#!/bin/bash
set -euo pipefail
out=$1; shift
extra=(src/voro++.cc)
if [[ -f src/periodic_complex.cc ]]; then extra+=(src/periodic_complex.cc); fi
c++ -std=c++17 -O0 -Wno-deprecated-declarations -Isrc -Ichecks "${extra[@]}" "$@" -o "$out"
