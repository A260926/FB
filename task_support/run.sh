#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/split_views_contract.cpp; fi
build_root=$(mktemp -d)
trap 'rm -rf "$build_root"' EXIT
mkdir -p "$build_root/objects"
incs=(-I. -Ibtfparse/btfparse/include -Ibtfparse/filereader/include -Ibtfparse/utils/include)
objects=()
for src in btfparse/btfparse/src/*.cpp btfparse/filereader/src/*.cpp; do
 obj="$build_root/objects/$(basename "$src").o"
 c++ -std=c++17 -O0 "${incs[@]}" -c "$src" -o "$obj"
 objects+=("$obj")
done
ar rcs "$build_root/libbtf.a" "${objects[@]}"
c++ -std=c++17 -O0 "${incs[@]}" task_support/baseline.cpp "$build_root/libbtf.a" -o "$build_root/baseline"
if [[ $mode == target ]]; then
  c++ -std=c++17 -O0 "${incs[@]}" -x c++ -c task_support/driver.c -o "$build_root/driver.o"
c++ -std=c++17 -O0 "${incs[@]}" tests/split_views_contract.cpp "$build_root/driver.o" "$build_root/libbtf.a" -o "$build_root/target"
test -s examples/split_modules.cpp
c++ -std=c++17 -O0 "${incs[@]}" examples/split_modules.cpp "$build_root/libbtf.a" -o "$build_root/split-modules"
  output_log=$(mktemp)
  trap 'rm -f "$output_log"; rm -rf "$build_root"' EXIT
  set +e
  "$build_root/target" >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in scopes declarations compatibility; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
"$build_root/baseline" "$build_root/baseline.h"
cc -std=c11 -I"$build_root" -fsyntax-only task_support/layout.c
if [[ $mode == target ]]; then
 "$build_root/split-modules" task_support/samples/base.btf task_support/samples/left.btf task_support/samples/right.btf > "$build_root/modules.h"
 test -s "$build_root/modules.h"
 cc -std=c11 -I"$build_root" -fsyntax-only task_support/merged_layout.c
fi
