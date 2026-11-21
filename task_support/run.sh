#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s checks/target.cpp; fi
case "$(uname -m)" in
 arm64|aarch64) archflags=-DGLM_FORCE_NEON ;;
 *) archflags='-mavx2 -mfma' ;;
esac
rm -rf .task-build/obj
mkdir -p .task-build/obj
find src/Base src/Render/Software -name '*.cpp' -type f | LC_ALL=C sort > .task-build/sources
test -s .task-build/sources
while IFS= read -r src; do
 obj=".task-build/obj/$src.o"
 mkdir -p "$(dirname "$obj")"
 c++ -std=c++17 -O1 -w -pthread -I. -Isrc -Ithird_party/glm -Ithird_party/stb/include -Ithird_party/md5 -include task_support/prelude.h $archflags -c "$src" -o "$obj"
done < .task-build/sources
cc -O1 -w -Ithird_party/md5 -c third_party/md5/md5.c -o .task-build/obj/md5.o
c++ -std=c++17 -O1 -w -pthread -I. -Isrc -Ithird_party/glm -Ithird_party/stb/include -Ithird_party/md5 -include task_support/prelude.h $archflags checks/baseline.cpp $(find .task-build/obj -name '*.o' -type f | LC_ALL=C sort) -o .task-build/baseline
c++ -std=c++17 -O1 -w -pthread -I. -Isrc -Ithird_party/glm -Ithird_party/stb/include -Ithird_party/md5 -include task_support/prelude.h $archflags -DSMOKE_MS4 checks/baseline.cpp $(find .task-build/obj -name '*.o' -type f | LC_ALL=C sort) -o .task-build/baseline-ms4
if [[ $mode == target ]]; then
  c++ -std=c++17 -O1 -w -pthread -I. -Isrc -Ithird_party/glm -Ithird_party/stb/include -Ithird_party/md5 -include task_support/prelude.h $archflags checks/target.cpp task_support/driver.c $(find .task-build/obj -name '*.o' -type f | LC_ALL=C sort) -o .task-build/target
test -s examples/sample_material.cpp
c++ -std=c++17 -O1 -w -pthread -I. -Isrc -Ithird_party/glm -Ithird_party/stb/include -Ithird_party/md5 -include task_support/prelude.h $archflags examples/sample_material.cpp $(find .task-build/obj -name '*.o' -type f | LC_ALL=C sort) -o .task-build/example
rm -f .task-build/sample.ppm
.task-build/example .task-build/sample.ppm
test -s .task-build/sample.ppm
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in interpolation derivatives commit; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
.task-build/baseline-ms4
