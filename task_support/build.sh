#!/bin/sh
set -eu
: "${CXX:=c++}"
export CXX
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
