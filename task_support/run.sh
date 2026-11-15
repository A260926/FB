#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then
  test -s src/raytracer/lattice.h
  test -s src/raytracer/lattice.c
  cc -std=c11 -DUNBOUND_OBJECTS -I. -Isrc/core -Isrc/raytracer -Ilib/SimplexNoise -Ilib/cJSON -x c -fsyntax-only src/raytracer/lattice.h
  cc -std=c11 -DUNBOUND_OBJECTS -I. -Isrc/core -Isrc/raytracer -Ilib/SimplexNoise -Ilib/cJSON -x c -fsyntax-only src/raytracer/lattice.c
fi
if [[ $mode == target ]]; then test -s tests/lattice_target.c; fi
mkdir -p .task-build
find .task-build -name '*.o' -delete
find src/core src/raytracer lib -name '*.c' ! -path 'src/raytracer/main.c' ! -path 'src/raytracer/image.c' -print > .task-build/sources.txt
while IFS= read -r unit; do
  name=$(printf '%s' "$unit" | tr '/.' '__')
  cc -std=c11 -O1 -DUNBOUND_OBJECTS -I. -Isrc/core -Isrc/raytracer -Ilib/SimplexNoise -Ilib/cJSON -c "$unit" -o ".task-build/prod-$name.o"
done < .task-build/sources.txt
cc -std=c11 -O1 -DUNBOUND_OBJECTS -Isrc/core -Isrc/raytracer -c task_support/headless_image.c -o .task-build/headless.o
cc -std=c11 -O1 -DUNBOUND_OBJECTS -I. -Isrc/core -Isrc/raytracer task_support/baseline.c .task-build/prod-*.o .task-build/headless.o -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  cc -std=c11 -O1 -DUNBOUND_OBJECTS -I. -Isrc/core -Isrc/raytracer -Ilib/SimplexNoise -Ilib/cJSON task_support/driver.c tests/lattice_target.c .task-build/prod-*.o .task-build/headless.o -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in cells identity transport; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
