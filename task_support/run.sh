#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
case "${1:-baseline}" in
 baseline) source=task_support/baseline.cpp ;;
 target) source=tests/clipped_mesh_target.cpp; test -s "$source" || { echo 'missing clipped_mesh_target.cpp' >&2; exit 2; } ;;
 *) exit 2 ;;
esac
scratch=$(mktemp -d "${TMPDIR:-/tmp}/dragon-mesh.XXXXXX")
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
export HOME="$scratch" XDG_CACHE_HOME="$scratch/cache"
find DragonBones/src -name '*.cpp' -print > "$scratch/sources"
set --
while IFS= read -r file; do set -- "$@" "$file"; done < "$scratch/sources"
"${CXX:-c++}" -std=c++17 -O1 -Wno-deprecated-declarations -IDragonBones/src -I3rdParty -I. -Itask_support "$source" "$@" -o "$scratch/check"
"$scratch/check"
