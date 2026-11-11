#!/usr/bin/env bash
#
# Debug build + assertion run for the motion (morph) meshlet bounds and builder.
# Runs from the repository root: bash demo/check_motion_meshlets.sh
#
# Fails (non-zero exit) if any required new file is missing, the existing
# meshoptdemo assertion suite fails, the motion regression program fails an
# assertion or runs no scenes, or the final success marker cannot be printed.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

required=(
    "demo/motion_meshlets.cpp"
    "demo/check_motion_meshlets.sh"
    "src/motionbounds.h"
    "src/clusterizer.cpp"
    "src/meshletutils.cpp"
    "src/meshoptimizer.h"
)

for file in "${required[@]}"; do
    if [[ ! -f "$file" ]]; then
        echo "missing required file: $file" >&2
        exit 1
    fi
done

tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/motion_meshlets.XXXXXX")
cleanup()
{
    rm -rf "$tmpdir"
}
trap cleanup EXIT

cxx="${CXX:-c++}"
cxxflags=(-g -O0 -Wall -Wextra -Wshadow -Wno-missing-field-initializers -std=c++11 -I "$root/src")

mkdir -p "$tmpdir/obj"

# Debug build of the static library from the current sources
library_sources=("$root"/src/*.cpp)
library_objects=()

for source in "${library_sources[@]}"; do
    object="$tmpdir/obj/$(basename "$source").o"
    "$cxx" "${cxxflags[@]}" -c "$source" -o "$object"
    library_objects+=("$object")
done

ar rcs "$tmpdir/libmeshoptimizer.a" "${library_objects[@]}"

# Existing assertion suite: meshoptdemo with no arguments runs runTests()
"$cxx" "${cxxflags[@]}" "$root/demo/main.cpp" "$root/demo/nanite.cpp" "$root/demo/tests.cpp" "$tmpdir/libmeshoptimizer.a" -o "$tmpdir/meshoptdemo"
"$tmpdir/meshoptdemo"

# New motion regression program
"$cxx" "${cxxflags[@]}" "$root/demo/motion_meshlets.cpp" "$tmpdir/libmeshoptimizer.a" -o "$tmpdir/motion_meshlets"

set +e
output=$("$tmpdir/motion_meshlets" 2>&1)
status=$?
set -e

echo "$output"

if [[ $status -ne 0 ]]; then
    echo "motion meshlet regression failed with exit status $status" >&2
    exit 1
fi

if ! echo "$output" | grep -q "motion meshlet scenes run: [1-9]"; then
    echo "motion meshlet regression ran no scenes" >&2
    exit 1
fi

echo "MOTION_MESHLETS_OK"
