#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
build=$(mktemp -d "${TMPDIR:-/tmp}/build.XXXXXX")
trap 'rm -rf "$build"' EXIT
export http_proxy= https_proxy= HTTP_PROXY= HTTPS_PROXY= ALL_PROXY= all_proxy=
cmake -S . -B "$build/lib" -DBDD_INCLUDE_TOOL=OFF -DBDD_INCLUDE_ISAGENERATOR_X86=OFF -DBDD_INCLUDE_TESTS=OFF -DBDD_INCLUDE_FUZZERS=OFF > "$build/configure.log"
cmake --build "$build/lib" -j2 > "$build/compile.log"
cc -std=c11 -Iinc support/baseline.c "$build/lib/libbdshemu.a" "$build/lib/libbddisasm.a" -o "$build/baseline"
if [[ "$mode" == target ]]; then
cc -std=c11 -I. -Iinc support/main.c regress_tests/regression.c "$build/lib/libbdshemu.a" "$build/lib/libbddisasm.a" -o "$build/target"
  out="$build/target.stdout"
  "$build/target" > "$out"
  cat "$out"
awk '
$1=="CASE" {if(NF!=4 || $4<1)bad=1; counts[$2]++}
$1=="GROUP" {if(NF!=4 || $3<2 || $4<2 || $3!=counts[$2])bad=1; groups[$2]++}
$1=="TARGET_COMPLETE" {if(NF!=3 || $2<6 || $3<6)bad=1; done++}
END {if(bad || done!=1 || groups["normal"]!=1 || groups["boundary"]!=1 || groups["integration"]!=1)exit 1}' "$out"
bash support/mutation-check.sh
elif [[ "$mode" != baseline ]]; then
  exit 2
fi
"$build/baseline"
