#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s unit_tests/test_flv_splitter.c; fi
mkdir -p .task-build
cc -std=c99 -O2 -I. -Icaption -c src/caption.c -o .task-build/core-caption.o
cc -std=c99 -O2 -I. -Icaption -c src/cea708.c -o .task-build/core-cea708.o
cc -std=c99 -O2 -I. -Icaption -c src/eia608.c -o .task-build/core-eia608.o
cc -std=c99 -O2 -I. -Icaption -c src/eia608_charmap.c -o .task-build/core-eia608_charmap.o
cc -std=c99 -O2 -I. -Icaption -c src/mpeg.c -o .task-build/core-mpeg.o
cc -std=c99 -O2 -I. -Icaption -c src/sei_wire.c -o .task-build/core-sei_wire.o
cc -std=c99 -O2 -I. -Icaption -c src/scc.c -o .task-build/core-scc.o
cc -std=c99 -O2 -I. -Icaption -c src/srt.c -o .task-build/core-srt.o
cc -std=c99 -O2 -I. -Icaption -c src/utf8.c -o .task-build/core-utf8.o
cc -std=c99 -O2 -I. -Icaption -c src/vtt.c -o .task-build/core-vtt.o
cc -std=c99 -O2 -I. -Icaption -c src/xds.c -o .task-build/core-xds.o
cc -std=c99 -O2 -I. -Icaption -x c -c src/eia608_from_utf8.c.cached -o .task-build/core-from-utf8.o
cc -std=c99 -O2 -I. -Icaption -Itask_support task_support/baseline.c task_support/fixture.c .task-build/core-caption.o .task-build/core-cea708.o .task-build/core-eia608.o .task-build/core-eia608_charmap.o .task-build/core-mpeg.o .task-build/core-sei_wire.o .task-build/core-scc.o .task-build/core-srt.o .task-build/core-utf8.o .task-build/core-vtt.o .task-build/core-xds.o .task-build/core-from-utf8.o -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  for p in caption/flv_splitter.h src/flv_splitter.c caption/sei_wire.h src/sei_wire.c examples/flv_split_main.c; do test -s "$p"; done
cc -std=c99 -O2 -Werror=implicit-function-declaration -I. -Icaption -include caption/flv_splitter.h -include caption/sei_wire.h -c src/flv_splitter.c -o .task-build/flv_splitter.o
cc -std=c99 -O2 -Werror=implicit-function-declaration -I. -Icaption -include caption/flv_splitter.h -include caption/sei_wire.h -c src/sei_wire.c -o .task-build/sei_wire.o
cc -std=c99 -O2 -Werror=implicit-function-declaration -I. -Icaption -Itask_support task_support/driver.c unit_tests/test_flv_splitter.c .task-build/core-caption.o .task-build/core-cea708.o .task-build/core-eia608.o .task-build/core-eia608_charmap.o .task-build/core-mpeg.o .task-build/core-scc.o .task-build/core-srt.o .task-build/core-utf8.o .task-build/core-vtt.o .task-build/core-xds.o .task-build/core-from-utf8.o .task-build/flv_splitter.o .task-build/sei_wire.o -lm -o .task-build/target
cc -std=c99 -O2 -Werror=implicit-function-declaration -I. -Icaption -Itask_support task_support/api_probe.c .task-build/core-caption.o .task-build/core-cea708.o .task-build/core-eia608.o .task-build/core-eia608_charmap.o .task-build/core-mpeg.o .task-build/core-scc.o .task-build/core-srt.o .task-build/core-utf8.o .task-build/core-vtt.o .task-build/core-xds.o .task-build/core-from-utf8.o .task-build/flv_splitter.o .task-build/sei_wire.o -lm -o .task-build/api-probe
cc -std=c99 -O2 -Werror=implicit-function-declaration -I. -Icaption -Itask_support examples/flv_split_main.c .task-build/core-caption.o .task-build/core-cea708.o .task-build/core-eia608.o .task-build/core-eia608_charmap.o .task-build/core-mpeg.o .task-build/core-scc.o .task-build/core-srt.o .task-build/core-utf8.o .task-build/core-vtt.o .task-build/core-xds.o .task-build/core-from-utf8.o .task-build/flv_splitter.o .task-build/sei_wire.o -lm -o .task-build/flv-split
cc -std=c99 -O2 -Werror=implicit-function-declaration -I. -Icaption -Itask_support task_support/fixture_main.c task_support/fixture.c .task-build/core-caption.o .task-build/core-cea708.o .task-build/core-eia608.o .task-build/core-eia608_charmap.o .task-build/core-mpeg.o .task-build/core-scc.o .task-build/core-srt.o .task-build/core-utf8.o .task-build/core-vtt.o .task-build/core-xds.o .task-build/core-from-utf8.o .task-build/flv_splitter.o .task-build/sei_wire.o -lm -o .task-build/fixture-gen
cc -std=c99 -O2 task_support/example_probe.c -o .task-build/example-probe
.task-build/api-probe
.task-build/fixture-gen .task-build/input.flv
rm -f .task-build/example1.flv .task-build/example2.flv .task-build/example3.flv .task-build/example4.flv
.task-build/flv-split .task-build/input.flv .task-build/example1.flv .task-build/example2.flv .task-build/example3.flv .task-build/example4.flv
.task-build/example-probe .task-build/example1.flv .task-build/example2.flv .task-build/example3.flv .task-build/example4.flv
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in wire routing backpressure; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == baseline ]]; then .task-build/baseline .task-build/baseline.flv; fi
