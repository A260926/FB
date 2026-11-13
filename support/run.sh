#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
build=$(mktemp -d "${TMPDIR:-/tmp}/build.XXXXXX")
trap 'rm -rf "$build"' EXIT
export http_proxy= https_proxy= HTTP_PROXY= HTTPS_PROXY= ALL_PROXY= all_proxy=
cmake -S UEFIExtract -B "$build/lib" > "$build/cmake-configure.log"
cmake --build "$build/lib" -j2 > "$build/cmake-build.log"
objects=()
while IFS= read -r -d '' object; do
 case "$object" in */uefiextract_main.cpp.o|*/ffsdumper.cpp.o|*/uefidump.cpp.o) continue;; esac
 objects+=("$object")
done < <(find "$build/lib/CMakeFiles/UEFIExtract.dir" -name '*.o' -print0)
c++ -std=c++11 -I. support/baseline.cpp "${objects[@]}" -o "$build/baseline"
if [[ "$mode" == target ]]; then
cc -std=c99 -I. -c support/main.c -o "$build/main.o"
sources=(regress_tests/regression.cpp)
for file in common/depex*.cpp; do
 if [[ ! -f "$file" ]]; then continue; fi
 linked=false
 for object in "${objects[@]}"; do
  if [[ "$object" == */"$file.o" ]]; then linked=true; break; fi
 done
 if [[ "$linked" == false ]]; then sources+=("$file"); fi
done
c++ -std=c++11 -I. "${sources[@]}" "$build/main.o" "${objects[@]}" -o "$build/target"
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
