#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/ra_state_contract.c; fi
mkdir -p .task-build
cmake -S . -B .task-build/cmake -DENABLE_DECOMPRESSION=OFF -DBUILD_DWARFDUMP=OFF -DBUILD_DWARFEXAMPLE=OFF -DDO_TESTING=OFF >.task-build/configure.log 2>&1
cmake --build .task-build/cmake --target dwarf-static -j 4 >.task-build/compile.log 2>&1
cc -std=c99 -I. -Isrc/lib/libdwarf task_support/fixture.c -o .task-build/fixture
.task-build/fixture .task-build/plain.elf plain
.task-build/fixture .task-build/set.elf set
cc -std=c99 -DLIBDWARF_STATIC -I. -Isrc/lib/libdwarf task_support/baseline.c .task-build/cmake/src/lib/libdwarf/libdwarf.a -o .task-build/baseline
if [[ $mode == target ]]; then
  cc -std=c99 -DLIBDWARF_STATIC -I. -Isrc/lib/libdwarf task_support/driver.c test/ra_state_contract.c .task-build/cmake/src/lib/libdwarf/libdwarf.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in Rows State Decode; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
