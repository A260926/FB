#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s checks/target.cpp; fi
mkdir -p .task-build
case $(uname -s) in Darwin) task_os=Mac ;; *) task_os=Linux ;; esac
objects=()
while IFS= read -r -d '' source; do
  object=".task-build/${source//\//_}.o"
  c++ -std=c++14 -O0 -pthread -I. -Iinclude -Ihlslang -Ihlslang/MachineIndependent -Ihlslang/OSDependent/"$task_os" -c "$source" -o "$object"
  objects+=("$object")
done < <(find hlslang/GLSLCodeGen hlslang/MachineIndependent -name '*.cpp' -print0)
c++ -std=c++14 -O0 -pthread -Ihlslang/OSDependent/"$task_os" -c hlslang/OSDependent/"$task_os"/ossource.cpp -o .task-build/os.o
objects+=(.task-build/os.o)
(( ${#objects[@]} > 1 ))
rm -f .task-build/libshader.a
ar rcs .task-build/libshader.a "${objects[@]}"
c++ -std=c++14 -pthread -I. -Iinclude task_support/baseline.cpp .task-build/libshader.a -o .task-build/baseline
if [[ $mode == target ]]; then
  c++ -std=c++14 -pthread -I. -Iinclude -Ihlslang -Ihlslang/MachineIndependent task_support/driver.c checks/target.cpp .task-build/libshader.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in origin diagnostics lifetime; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
