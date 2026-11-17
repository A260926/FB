#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/macho_references.c; fi
mkdir -p .task-build
cc -std=gnu11 -include stdbool.h -O0 -I. -Isrc -D_FILE_OFFSET_BITS=64 -c src/headerParserLib.c -o .task-build/library.o
objects=(.task-build/library.o)
while IFS= read -r source; do
  object=.task-build/$(printf '%s' "$source" | tr '/.' '__').o
  cc -std=gnu11 -include stdbool.h -O0 -I. -Isrc -D_FILE_OFFSET_BITS=64 -c "$source" -o "$object"
  objects+=("$object")
done < <(find src -name '*.c' ! -path 'src/headerParserLib.c' ! -path 'src/headerParser.c' | sort)
cc -std=gnu11 -include stdbool.h -I. -Isrc task_support/baseline.c "${objects[@]}" -o .task-build/baseline
if [[ $mode == target ]]; then
  cc -std=gnu11 -I. -Isrc tests/macho_references.c task_support/driver.c "${objects[@]}" -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in identity coordinates ownership; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
