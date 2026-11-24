#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/tied_ownership_contract.c; fi
mkdir -p .task-build/fixture
cmake -S . -B .task-build/cmake -DENABLE_DECOMPRESSION=OFF -DBUILD_MMAP=OFF -DBUILD_DWARFDUMP=OFF -DBUILD_DWARFGEN=OFF -DBUILD_DWARFEXAMPLE=OFF -DDO_TESTING=OFF -DCMAKE_BUILD_TYPE=Debug
cmake --build .task-build/cmake --target dwarf-static --clean-first --parallel 4
clang --target=x86_64-unknown-linux-gnu -gdwarf-5 -gsplit-dwarf -O0 -c task_support/fixture.c -o .task-build/fixture/split.o
test -s .task-build/fixture/split.o
test -s .task-build/fixture/split.dwo
cc -std=c99 -Isrc/lib/libdwarf task_support/tie_probe.c .task-build/cmake/src/lib/libdwarf/libdwarf.a -o .task-build/tie-probe

if [[ $mode == target ]]; then
  test -s examples/tied_ownership.c
cc -std=c99 -I. -Isrc/lib/libdwarf task_support/driver.c test/tied_ownership_contract.c .task-build/cmake/src/lib/libdwarf/libdwarf.a -o .task-build/target
.task-build/tie-probe contract
cc -std=c99 -Isrc/lib/libdwarf examples/tied_ownership.c .task-build/cmake/src/lib/libdwarf/libdwarf.a -o .task-build/tied-example
example_dir=$(mktemp -d .task-build/example.XXXXXX)
.task-build/tied-example "$example_dir/result.txt"
test -s "$example_dir/result.txt"

  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in relations errors survivors; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == baseline ]]; then
  .task-build/tie-probe observe
else
  .task-build/tie-probe healthy
fi
