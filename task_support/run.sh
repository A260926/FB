#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s split_dwarf_test.c; fi
mkdir -p .task-build
if [[ ! -f .task-build/Makefile ]]; then
 (cd .task-build && CONFIG_SITE=/dev/null ../configure --disable-shared --disable-dependency-tracking) >.task-build/configure.log 2>&1 || { tail -30 .task-build/configure.log; exit 1; }
fi
make -C .task-build -j4 >.task-build/build.log 2>&1 || { tail -30 .task-build/build.log; exit 1; }
# Select the ELF64 production backend and disable host module enumeration.
awk '{print; if ($0 == "#include \"config.h\"") {print "#undef HAVE_DL_ITERATE_PHDR"; print "#undef BACKTRACE_ELF_SIZE"; print "#define BACKTRACE_ELF_SIZE 64"}}' elf.c > .task-build/task-elf.c
cc -std=c99 -I.task-build -I. -c .task-build/task-elf.c -o .task-build/task-elf.o
cc -std=c99 -I.task-build -I. task_support/baseline.c .task-build/task-elf.o .task-build/.libs/libbacktrace.a -o .task-build/baseline
if [[ $mode == target ]]; then
  cc -std=c99 -I.task-build -I. split_dwarf_test.c task_support/driver.c .task-build/task-elf.o .task-build/.libs/libbacktrace.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in pairing contexts late_attach; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
