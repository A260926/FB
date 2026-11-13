#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-target}
build=.task-build
mkdir -p "$build"
cxx=${CXX:-c++}
flags=(-std=c++17 -O1 -pthread -DUSE_CAL3D_WITH_CPP_11 -DIMVU_NO_INTRINSICS -include cstring -I. -Isrc -Isupport)
sources=(animation bone bonetransform coreanimation corebone coremorphtarget coreskeleton coresubmesh coretrack error forsythtriangleorderoptimizer matrix memory mixer physique quaternion simplify skeleton streamops submesh transform trisort vector)
for n in "${sources[@]}"; do
  f=src/cal3d/$n.cpp
  if [[ ! -f "$build/$n.o" || "$f" -nt "$build/$n.o" || support/run.sh -nt "$build/$n.o" ]] || [[ -n "$(find src/cal3d -name '*.h' -newer "$build/$n.o" -print -quit 2>/dev/null)" ]]; then
    "$cxx" "${flags[@]}" -c "$f" -o "$build/$n.o"
  fi
done
"$cxx" "${flags[@]}" -c support/loader_sentinel.cpp -o "$build/loader_sentinel.o"
objects=()
for n in "${sources[@]}" loader_sentinel; do objects+=("$build/$n.o"); done
if [[ $mode == build ]]; then echo BUILD_OK; exit 0; fi
verify() {
  local binary=$1 label=$2; shift 2
  local out="$build/$label.stdout"
  set +e
  "$binary" "$label" "$@" >"$out" 2>&1
  local status=$?
  set -e
  cat "$out"
  [[ $status == 0 ]] || return "$status"
  local g
  for g in "$@"; do
    grep -Eq "^GROUP $g cases=[1-9][0-9]* checks=[1-9][0-9]* DONE$" "$out" || return 1
  done
  grep -qx "HARNESS_COMPLETE mode=$label groups=$#" "$out"
}
baseline() {
  "$cxx" "${flags[@]}" support/baseline.cpp "${objects[@]}" -o "$build/baseline"
  "$build/baseline"
}
legacy() {
  # Only substitute the unavailable third-party TestPrologue; test bodies stay intact.
  for n in testTriSort testTransform testPhysique; do
    sed 's@#include "TestPrologue.h"@#include "upstream_adapter.h"@' "test/$n.cpp" > "$build/$n.cpp"
  done
  "$cxx" "${flags[@]}" support/main.cpp "$build/testTriSort.cpp" "$build/testTransform.cpp" "$build/testPhysique.cpp" "${objects[@]}" -o "$build/legacy"
  verify "$build/legacy" legacy legacy
}
case "$mode" in
baseline) baseline; legacy ;;
legacy) legacy ;;
target)
  test -s test/transparent_packet.cpp
  if [[ -f src/cal3d/transparentpacket.cpp ]]; then
    "$cxx" "${flags[@]}" -c src/cal3d/transparentpacket.cpp -o "$build/transparentpacket.o"
    objects+=("$build/transparentpacket.o")
  fi
  "$cxx" "${flags[@]}" support/main.cpp test/transparent_packet.cpp "${objects[@]}" -o "$build/target"
  verify "$build/target" target bsp rays animation
  baseline
  legacy
  echo TRANSPARENT_PACKET_OK
  ;;
*) echo "unknown mode" >&2; exit 2 ;;
esac
