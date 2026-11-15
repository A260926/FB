#!/bin/bash
set -euo pipefail
mkdir -p .task-build/core
# Header edits invalidate every translation unit; each new production .cc is discovered.
header_stamp=.task-build/headers.cksum
find include -type f -name '*.h' -exec cksum {} \; | LC_ALL=C sort > .task-build/headers.next
if [[ ! -f $header_stamp ]] || ! cmp -s "$header_stamp" .task-build/headers.next; then
 find .task-build/core -type f -name '*.o' -delete
 mv .task-build/headers.next "$header_stamp"
fi
while IFS= read -r source; do
 object=".task-build/core/$(basename "${source%.cc}").o"
 if [[ ! -f $object || $source -nt $object ]]; then
  c++ -std=c++17 -O0 -Iinclude -I. -c "$source" -o "$object"
 fi
done < <(find libsrc -type f -name '*.cc' ! -name 'GLCamera.cc' ! -name 'GLManager.cc' | LC_ALL=C sort)
objects=$(find .task-build/core -type f -name '*.o' | LC_ALL=C sort)
test -n "$objects"
rm -f .task-build/libtrimesh.a
ar rcs .task-build/libtrimesh.a $objects
c++ -std=c++17 -O0 -Iinclude -I. utilsrc/mesh_check.cc .task-build/libtrimesh.a -o .task-build/mesh_check
c++ -std=c++17 -O0 -Iinclude -I. task_support/baseline.cpp .task-build/libtrimesh.a -o .task-build/baseline
