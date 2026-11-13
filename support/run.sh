#!/bin/sh
# Proposed replacement for the missing support/run.sh; not installed in a seed.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
mode=${1:-baseline}
case "$mode" in baseline|target) ;; *) echo "usage: sh support/run.sh baseline|target" >&2; exit 2;; esac
build_dir="$root/.task-build/protected-keyframes"
mkdir -p "$build_dir"
compiler=${CXX:-c++}
test -s support/baseline.cpp
test -f includes/acl/compression/compress.h
test -f external/rtm/includes/rtm/vqvf.h
"$compiler" -std=c++17 -O2 -Iincludes -Iexternal/rtm/includes \
  support/baseline.cpp -o "$build_dir/baseline"
"$build_dir/baseline"
if [ "$mode" = target ]; then
  test -s tests/protected_keyframes_target.cpp || {
    echo "Missing model-produced tests/protected_keyframes_target.cpp" >&2
    exit 1
  }
  "$compiler" -std=c++17 -O2 -Iincludes -Iexternal/rtm/includes \
    tests/protected_keyframes_target.cpp -o "$build_dir/protected_keyframes_target"
  "$build_dir/protected_keyframes_target"
  echo ACL_PROTECTED_TARGET_OK
fi
