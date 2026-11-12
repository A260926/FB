#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
if [[ $# -lt 1 || -z $1 ]]; then
 echo "usage: bash tools/check-temporal.sh <ppm-output-dir>" >&2
 exit 2
fi
out=$1
mkdir -p "$out"

required=(include/temporal.hpp src/temporal.cpp tests/temporal.cpp)
for f in "${required[@]}"; do
 if [[ ! -f "$root/$f" ]]; then
  echo "missing file: $f" >&2
  exit 1
 fi
 if [[ ! -s "$root/$f" ]]; then
  echo "empty file: $f" >&2
  exit 1
 fi
done
if ! grep -q 'assert(' "$root/tests/temporal.cpp"; then
 echo "tests/temporal.cpp contains no assertions" >&2
 exit 1
fi

build=$(mktemp -d 2>/dev/null || mktemp -d "${TMPDIR:-/tmp}/temporal.XXXXXX")
trap 'rm -r "$build"' EXIT
c++ -std=c++17 -O2 -Wall -Wextra -I"$root/include" \
 "$root/src/camera.cpp" "$root/src/image.cpp" "$root/src/raster.cpp" \
 "$root/src/temporal.cpp" "$root/tests/temporal.cpp" -o "$build/temporal"

log=$("$build/temporal" "$out")
printf '%s\n' "$log"
for line in "four-tap weights ok" "per-texel depth/object rejection ok" \
            "exposure rescale ok" "YCoCg clamp ok" \
            "jitter difference and motion ok" \
            "invalid query / background ok" "restart conditions ok" \
            "invalid arguments preserve history ok" \
            "snapshot ownership / aliasing ok" "independent accumulators ok" \
            "draw sequence ok" "TEMPORAL_TESTS_OK"; do
 if ! grep -qF "$line" <<<"$log"; then
  echo "missing expected test output: $line" >&2
  exit 1
 fi
done

for ppm in current.ppm temporal.ppm; do
 if [[ ! -s "$out/$ppm" ]]; then
  echo "missing or empty output: $out/$ppm" >&2
  exit 1
 fi
 if [[ $(head -c 2 "$out/$ppm") != "P6" ]]; then
  echo "not a P6 PPM: $out/$ppm" >&2
  exit 1
 fi
done

echo "TEMPORAL_RESOLVE_OK"
