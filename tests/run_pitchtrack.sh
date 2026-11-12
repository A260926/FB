#!/usr/bin/env bash
# Build and run the streaming pitch tracker regression test.
#
# Only a system C compiler, CMake and pkg-config are required.  The library is
# built from the sources into a temporary directory; no Python implementation
# and no example program is used.
#
# The script reports how many checks actually ran and fails when none did.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${TMPDIR:-/tmp}/aubio-pitchtrack-test.$$"

cleanup() {
  rm -rf "$build_dir"
}
trap cleanup EXIT

mkdir -p "$build_dir/pkg"

# Configure with an empty pkg-config search path so that no optional dependency
# is picked up.
PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR="$build_dir/pkg" \
  cmake -S "$root" -B "$build_dir/cmake" -DCMAKE_BUILD_TYPE=Release \
  > "$build_dir/configure.log" 2>&1

cmake --build "$build_dir/cmake" --target aubio -j 4 \
  > "$build_dir/build.log" 2>&1

# Compile the regression test against the public headers and the freshly built
# library, the same way the other tests are built.
cc -std=c99 -O2 -Wall -Wextra -Wno-unused-parameter \
  -DAUBIO_PITCHTRACK_TEST=1 -I"$root/src" \
  "$root/tests/src/pitch/test-pitchtrack.c" \
  -L"$build_dir/cmake/src" -Wl,-rpath,"$build_dir/cmake/src" \
  -laubio -lm -o "$build_dir/test-pitchtrack"

set +e
"$build_dir/test-pitchtrack" > "$build_dir/run.log" 2> "$build_dir/run.err"
status=$?
set -e

cat "$build_dir/run.log"
if [ -s "$build_dir/run.err" ]; then
  echo "--- stderr ---" >&2
  cat "$build_dir/run.err" >&2
fi

checks=$(sed -n 's/^PITCHTRACK_CHECKS_RUN=\([0-9][0-9]*\)$/\1/p' \
  "$build_dir/run.log" | tail -n 1)
checks="${checks:-0}"

echo "pitchtrack: $checks checks executed"
if [ "$checks" -eq 0 ]; then
  echo "pitchtrack: no test case was executed" >&2
  exit 1
fi
exit "$status"
