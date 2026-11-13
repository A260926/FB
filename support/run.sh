#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
build=$(mktemp -d "${TMPDIR:-/tmp}/build.XXXXXX")
trap 'rm -rf "$build"' EXIT
export http_proxy= https_proxy= HTTP_PROXY= HTTPS_PROXY= ALL_PROXY= all_proxy=
cmake -S . -B "$build/lib" -DBUILD_SHARED_LIBS=OFF -DBUILD_COMMAND_LINE_TOOLS=OFF -DPEPARSE_ENABLE_EXAMPLES=OFF -DPEPARSE_ENABLE_TESTING=OFF -DCMAKE_BUILD_TYPE=Debug > "$build/configure.log"
cmake --build "$build/lib" -j2 > "$build/compile.log"
c++ -std=c++11 -Ipe-parser-library/include support/baseline.cpp "$build/lib/pe-parser-library/libpe-parse.a" -o "$build/baseline"
if [[ "$mode" == target ]]; then
cc -std=c99 -I. -c support/main.c -o "$build/main.o"
c++ -std=c++11 -I. -Ipe-parser-library/include regress_tests/regression.cpp "$build/main.o" "$build/lib/pe-parser-library/libpe-parse.a" -o "$build/target"
  out="$build/target.stdout"
  "$build/target" > "$out"
  cat "$out"
awk '
$1=="CASE" {if(NF!=4 || $4<1)bad=1; counts[$2]++}
$1=="GROUP" {if(NF!=4 || $3<2 || $4<2 || $3!=counts[$2])bad=1; groups[$2]++}
$1=="TARGET_COMPLETE" {if(NF!=3 || $2<6 || $3<6)bad=1; done++}
END {if(bad || done!=1 || groups["normal"]!=1 || groups["boundary"]!=1 || groups["integration"]!=1)exit 1}' "$out"
elif [[ "$mode" != baseline ]]; then
  exit 2
fi
"$build/baseline"
