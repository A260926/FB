#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
cache=.cache/spatial
mkdir -p "$cache"
cxx=${CXX:-c++}
flags=(-std=c++20 -O2 -pthread -I src -I task_support)
legacy() {
 "$cxx" "${flags[@]}" task_support/baseline.cpp -o "$cache/baseline"
 "$cache/baseline"
 for name in simple_example serialize threadpool_test; do
  "$cxx" "${flags[@]}" "test/$name.cpp" -o "$cache/$name"
  (cd "$cache" && "./$name")
 done
 echo LEGACY_DONE tests=3
}
case "$mode" in
 baseline) legacy ;;
 target)
  test -s test/spatial_sah_test.cpp || { echo 'missing test/spatial_sah_test.cpp' >&2; exit 2; }
  "$cxx" "${flags[@]}" task_support/runner.cpp test/spatial_sah_test.cpp -o "$cache/target"
  status=0
  "$cache/target" > "$cache/target.stdout" 2> "$cache/target.stderr" || status=$?
  printf '%s\n' "$status" > "$cache/target.exit"
  cat "$cache/target.stdout"
  cat "$cache/target.stderr" >&2
  test "$status" -eq 0 || exit "$status"
  awk '
   /^GROUP (clipping|budget|queries|integration) cases=[0-9]+ checks=[0-9]+$/ {
    split($3,n,"=");split($4,c,"=");if(n[2]<2||c[2]<4)bad=1;seen[$2]++
   }
   /^TARGET_DONE cases=[0-9]+ checks=[0-9]+$/ {
    split($2,n,"=");split($3,c,"=");if(n[2]<8||c[2]<16)bad=1;done++
   }
   END {if(bad||done!=1||seen["clipping"]!=1||seen["budget"]!=1||seen["queries"]!=1||seen["integration"]!=1)exit 1}
  ' "$cache/target.stdout" || { echo 'incomplete target execution' >&2; exit 4; }
  legacy
  echo SPATIAL_TARGET_OK ;;
 *) echo 'mode must be baseline or target' >&2;exit 2 ;;
esac
