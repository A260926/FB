#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mode=${1:-baseline}
case "$mode" in
 baseline) testsrc=task_support/baseline.cpp ;;
 target) testsrc=test/animation/offline/retarget_target.cpp; test -s "$testsrc" || { echo 'missing retarget_target.cpp' >&2; exit 2; } ;;
 *) exit 2 ;;
esac
build=$(mktemp -d "${TMPDIR:-/tmp}/ozz-retarget.XXXXXX")
trap 'rm -rf "$build"' EXIT HUP INT TERM
export XDG_CACHE_HOME="$build/cache"
find src/base src/animation/runtime src/animation/offline -name '*.cc' ! -path '*/fbx/*' ! -path '*/gltf/*' ! -path '*/tools/*' -print > "$build/sources"
while IFS= read -r source; do
 object="$build/$(echo "$source" | tr / _).o"
 "${CXX:-c++}" -std=c++17 -O1 -Iinclude -Isrc -c "$source" -o "$object"
done < "$build/sources"
"${CXX:-c++}" -std=c++17 -O1 -Iinclude -Isrc -I. -Itask_support "$testsrc" "$build"/*.o -o "$build/check"
"$build/check"
