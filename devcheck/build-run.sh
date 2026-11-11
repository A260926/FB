#!/usr/bin/env bash
set -euo pipefail
source_file=${1:?C++ source required}
test -s "$source_file"
build=$(mktemp -d "${TMPDIR:-/tmp}/p2t-build.XXXXXX")
trap 'rm -rf "$build"' EXIT
c++ -std=c++14 -O0 -DP2T_STATIC_EXPORTS -I. "$source_file" poly2tri/common/shapes.cc poly2tri/sweep/advancing_front.cc poly2tri/sweep/cdt.cc poly2tri/sweep/sweep.cc poly2tri/sweep/sweep_context.cc -o "$build/check"
"$build/check"
