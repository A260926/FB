#!/bin/bash
set -euo pipefail
flags=.task-build/cmake/gme/CMakeFiles/gme_static.dir/flags.make
if grep -q -- '-DBLARGG_BIG_ENDIAN=1' "$flags"; then
 endian=-DBLARGG_BIG_ENDIAN=1
elif grep -q -- '-DBLARGG_LITTLE_ENDIAN=1' "$flags"; then
 endian=-DBLARGG_LITTLE_ENDIAN=1
else
 echo 'missing CMake endian definition' >&2; exit 2
fi
exec c++ "$endian" "$@"
