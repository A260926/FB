#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/callsite_contract.c; fi
mkdir -p .task-build
cmake -S . -B .task-build/cmake -DBUILD_DWARFDUMP=OFF -DBUILD_DWARFGEN=OFF -DBUILD_DWARFEXAMPLE=OFF -DDO_TESTING=OFF -DENABLE_DECOMPRESSION=OFF > .task-build/configure.log 2>&1
cmake --build .task-build/cmake --target dwarf-static -j2 > .task-build/build.log 2>&1
cc -std=c99 task_support/fixture.c -o .task-build/fixture
.task-build/fixture .task-build/callsite.elf
cc -std=c99 -DLIBDWARF_STATIC -I. -Isrc/lib/libdwarf -I.task-build/cmake task_support/baseline.c .task-build/cmake/src/lib/libdwarf/libdwarf.a -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s src/lib/libdwarf/dwarf_callsite.c
test -s src/bin/dwarfexample/tailcall.c
cc -std=c99 -DLIBDWARF_STATIC -I. -Isrc/lib/libdwarf -I.task-build/cmake src/lib/libdwarf/dwarf_callsite.c test/callsite_contract.c task_support/driver.c .task-build/cmake/src/lib/libdwarf/libdwarf.a -o .task-build/target
cc -std=c99 -DLIBDWARF_STATIC -I. -Isrc/lib/libdwarf -I.task-build/cmake src/lib/libdwarf/dwarf_callsite.c src/bin/dwarfexample/tailcall.c .task-build/cmake/src/lib/libdwarf/libdwarf.a -o .task-build/tailcall
.task-build/tailcall .task-build/callsite.elf
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in identity substitution termination; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline .task-build/callsite.elf
