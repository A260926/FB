#!/usr/bin/env bash
set -euo pipefail
b=$(mktemp -d "${TMPDIR:-/tmp}/ymfm.XXXXXX")
trap 'rm -rf "$b"' EXIT
c++ -std=c++14 -O1 -Isrc baseline/smoke.cpp src/ymfm_adpcm.cpp src/ymfm_ssg.cpp -o "$b/smoke"
"$b/smoke"
c++ -std=c++14 -O1 -Isrc examples/buildall/buildall.cpp src/*.cpp -o "$b/buildall"
"$b/buildall"
echo BASELINE_BUILDALL_OK
