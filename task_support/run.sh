#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/test_smk_avi.c; fi
mkdir -p .task-build
cc -std=c99 -O2 -I. -Itask_support -c smacker.c -o .task-build/smacker.o
cc -std=c99 -O2 -I. -Itask_support task_support/baseline.c task_support/fixture.c .task-build/smacker.o -o .task-build/baseline
if [[ $mode == target ]]; then
  for p in smk_avi.h smk_avi.c smk_export.h smk_export.c examples/smk_export_main.c; do test -s "$p"; done
cc -std=c99 -O2 -Werror=implicit-function-declaration -I. -Itask_support -include smk_avi.h -include smk_export.h -c smk_avi.c -o .task-build/avi.o
cc -std=c99 -O2 -Werror=implicit-function-declaration -I. -Itask_support -include smk_avi.h -include smk_export.h -c smk_export.c -o .task-build/export.o
cc -std=c99 -O2 -I. -Itask_support task_support/driver.c tests/test_smk_avi.c task_support/fixture.c .task-build/smacker.o .task-build/avi.o .task-build/export.o -o .task-build/target
cc -std=c99 -O2 -Werror=implicit-function-declaration -I. -Itask_support task_support/api_probe.c task_support/fixture.c .task-build/smacker.o .task-build/avi.o .task-build/export.o -o .task-build/api-probe
cc -std=c99 -O2 -I. -Itask_support examples/smk_export_main.c .task-build/smacker.o .task-build/avi.o .task-build/export.o -o .task-build/smk-export
cc -std=c99 -O2 task_support/example_probe.c -o .task-build/example-probe
rm -f .task-build/api.avi .task-build/example.avi
.task-build/api-probe .task-build/input.smk .task-build/api.avi
.task-build/smk-export .task-build/input.smk .task-build/example.avi 5
.task-build/example-probe .task-build/example.avi
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in pcm layout streaming; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == baseline ]]; then .task-build/baseline .task-build/baseline.smk; fi
