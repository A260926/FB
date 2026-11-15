#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then
 test -s examples/snapshots.cpp
 test -s src/TheRestOfYourLife/scene_snapshot.h
 test -s src/TheRestOfYourLife/scene_snapshot.cpp
 c++ -std=c++17 -pthread -I. -Isrc -Isrc/TheRestOfYourLife -x c++ -fsyntax-only src/TheRestOfYourLife/scene_snapshot.h
 c++ -std=c++17 -pthread -I. -Isrc -Isrc/TheRestOfYourLife -x c++ -fsyntax-only src/TheRestOfYourLife/scene_snapshot.cpp
fi
if [[ $mode == target ]]; then test -s checks/snapshot_cases.cpp; fi
mkdir -p .task-build/prod
: > .task-build/objects.list
while IFS= read -r source_file; do
 object_file=".task-build/prod/$(basename "$source_file").o"
 c++ -std=c++17 -O1 -pthread -I. -Isrc -Isrc/TheRestOfYourLife -c "$source_file" -o "$object_file"
 printf '%s\n' "$object_file" >> .task-build/objects.list
done < <(find src/TheRestOfYourLife -type f -name '*.cpp' | LC_ALL=C sort)
set --
while IFS= read -r object_file; do set -- "$@" "$object_file"; done < .task-build/objects.list
c++ -std=c++17 -O1 -pthread -I. -Isrc -Isrc/TheRestOfYourLife task_support/baseline.cpp "$@" -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s src/TheRestOfYourLife/scene_snapshot.cpp
test -s examples/snapshots.cpp
c++ -std=c++17 -O1 -pthread -I. -Isrc -Isrc/TheRestOfYourLife checks/snapshot_cases.cpp -x c++ task_support/driver.c -x none "$@" -o .task-build/target
c++ -std=c++17 -O1 -pthread -I. -Isrc -Isrc/TheRestOfYourLife examples/snapshots.cpp "$@" -o .task-build/snapshots
mkdir -p .task-build/example
rm -f .task-build/example/parent.ppm
rm -f .task-build/example/left.ppm
rm -f .task-build/example/right.ppm
.task-build/snapshots .task-build/example
test -s .task-build/example/parent.ppm
test -s .task-build/example/left.ppm
test -s .task-build/example/right.ppm
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in persistence sharing lighting; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
