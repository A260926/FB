#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
d=$(mktemp -d)
trap 'rm -rf "$d"' EXIT
${CXX:-c++} -std=c++17 -O1 -I. checks/baseline.cpp -o "$d/base"
"$d/base"
case $(uname -s) in
 Darwin) gc=-Wl,-dead_strip ;;
 *) gc=-Wl,--gc-sections ;;
esac
${CXX:-c++} -std=c++17 -O1 -ffunction-sections -fdata-sections "$gc" -I. -Iexamples/common checks/uv_baseline.cpp -o "$d/uv"
"$d/uv"
