#!/bin/sh
set -eu
: "${CXX:=c++}"
export CXX
mkdir -p .task-build
source_hash=$(find libs/eely/src/eely libs/eely/include/eely external/gsl/include external/acl/include -type f -exec shasum -a 256 {} + | LC_ALL=C sort | shasum -a 256)
build_hash=$(shasum -a 256 task_support/build.sh task_support/baseline.cpp)
compiler_hash=$("$CXX" --version)
fingerprint="$source_hash
$build_hash
$compiler_hash"
if [ -s .task-build/input-hash ] && [ -s .task-build/baseline ] && [ -s .task-build/object-list ] && [ "$fingerprint" = "$(cat .task-build/input-hash)" ]; then
  exit 0
fi
rm -rf .task-build/objects
mkdir -p .task-build/objects
find libs/eely/src/eely -name '*.cpp' -type f -print0 | xargs -0 -P 4 -I SRC sh -c '
set -eu
source_path=$1
object_path=.task-build/objects/${source_path%.cpp}.o
mkdir -p "$(dirname "$object_path")"
"$CXX" -std=c++23 -O1 -DEELY_DEBUG -Ilibs/eely/include -Iexternal/gsl/include -Iexternal/acl/include -I. -c "$source_path" -o "$object_path"
' sh SRC
find .task-build/objects -name '*.o' -type f | LC_ALL=C sort > .task-build/object-list
test -s .task-build/object-list
rm -f .task-build/libeely.a
ar rcs .task-build/libeely.a $(cat .task-build/object-list)
"$CXX" -std=c++23 -O1 -DEELY_DEBUG -Ilibs/eely/include -Iexternal/gsl/include -Iexternal/acl/include -I. task_support/baseline.cpp .task-build/libeely.a -o .task-build/baseline

printf '%s\n' "$fingerprint" > .task-build/input-hash
