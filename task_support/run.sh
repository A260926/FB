#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
build=.cache/ordered
mkdir -p "$build"
cxx=${CXX:-c++}
# All selected translation units use only headers supplied by this repository.
flags=(-std=c++20 -O1 -I. -DNO_THREADED_BUILDS)
build_old() {
  "$cxx" "${flags[@]}" -DTINYBVH_NO_SIMD task_support/baseline.cpp -o "$build/baseline"
  for test in double tlas_state axis_ray simd_reference; do
    "$cxx" "${flags[@]}" -march=native "tests/tiny_bvh_${test}_test.cpp" -o "$build/old_${test}"
  done
}
run_old() {
  test -x "$build/baseline" || build_old
  "$build/baseline"
  for test in double tlas_state axis_ray simd_reference; do "$build/old_${test}"; done
  printf '%s\n' 'LEGACY_COMPLETE 4'
}
case "$mode" in
  build) build_old ;;
  baseline) run_old ;;
  target)
    test -s tests/tiny_bvh_ordered_test.cpp || { echo 'missing ordered tests' >&2; exit 1; }
    test -s examples/ordered_layers.cpp || { echo 'missing ordered example' >&2; exit 1; }
    "$cxx" "${flags[@]}" -DTINYBVH_NO_SIMD examples/ordered_layers.cpp -o "$build/ordered_layers"
    "$build/ordered_layers" "$build/ordered_layers.ppm"
    awk -f task_support/check-ppm.awk "$build/ordered_layers.ppm"
    for variant in scalar native; do
      extra=(-march=native)
      if [[ "$variant" == scalar ]]; then extra=(-DTINYBVH_NO_SIMD); fi
      "$cxx" "${flags[@]}" "${extra[@]}" task_support/driver.cpp tests/tiny_bvh_ordered_test.cpp -o "$build/target_$variant"
      # Preserve the actual process exit separately from output validation.
      rc=0
      "$build/target_$variant" > "$build/target_$variant.stdout" 2> "$build/target_$variant.stderr" || rc=$?
      cat "$build/target_$variant.stdout"
      cat "$build/target_$variant.stderr" >&2
      [[ $rc -eq 0 ]] || exit "$rc"
      awk -f task_support/check-output.awk "$build/target_$variant.stdout"
    done
    # Recompile old paths after changes; no stale production code from install.
    build_old
    run_old
    printf '%s\n' 'ORDERED_TARGET_OK'
    ;;
  *) echo 'usage: bash task_support/run.sh build|baseline|target' >&2; exit 2 ;;
esac
