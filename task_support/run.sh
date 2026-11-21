#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/shell_diffusion.c; fi
extra_production=""
if [[ $mode == target ]]; then
 test -s src/raytracer/shell_diffusion.h
 test -s src/raytracer/shell_diffusion.c
 extra_production=src/raytracer/shell_diffusion.c
fi
mkdir -p .task-build
cc -std=c11 -O2 -Isrc/core -Isrc/raytracer -Ilib/SimplexNoise checks/current.c src/core/argv.c src/core/calc.c src/core/mem.c src/core/strhash.c src/core/system.c src/raytracer/accel.c src/raytracer/camera.c src/raytracer/material.c src/raytracer/object.c src/raytracer/render.c lib/SimplexNoise/SimplexNoise.c checks/offscreen.c $extra_production -lm -o .task-build/current
if [[ $mode == target ]]; then
  test -s src/raytracer/shell_diffusion.h
test -s src/raytracer/shell_diffusion.c
test -s examples/shell_diffusion.c
cc -std=c11 -O2 -Isrc/core -Isrc/raytracer -Ilib/SimplexNoise -I. -include src/raytracer/shell_diffusion.h -c src/raytracer/shell_diffusion.c -o .task-build/shell_diffusion.o
cc -std=c11 -O2 -Isrc/core -Isrc/raytracer -Ilib/SimplexNoise -I. task_support/driver.c tests/shell_diffusion.c src/core/argv.c src/core/calc.c src/core/mem.c src/core/strhash.c src/core/system.c src/raytracer/accel.c src/raytracer/camera.c src/raytracer/material.c src/raytracer/object.c src/raytracer/render.c lib/SimplexNoise/SimplexNoise.c checks/offscreen.c .task-build/shell_diffusion.o -lm -o .task-build/target
cc -std=c11 -O2 -Isrc/core -Isrc/raytracer -Ilib/SimplexNoise -I. examples/shell_diffusion.c src/core/argv.c src/core/calc.c src/core/mem.c src/core/strhash.c src/core/system.c src/raytracer/accel.c src/raytracer/camera.c src/raytracer/material.c src/raytracer/object.c src/raytracer/render.c lib/SimplexNoise/SimplexNoise.c checks/offscreen.c .task-build/shell_diffusion.o -lm -o .task-build/example
.task-build/example > .task-build/example.log
cat .task-build/example.log
grep -qx SHELL_DIFFUSION_CPU_OK .task-build/example.log
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in operator solver shading; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/current
