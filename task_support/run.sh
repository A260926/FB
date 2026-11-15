#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then
  test -s opensubdiv/bfr/tessellationSession.h
  test -s opensubdiv/bfr/tessellationSession.cpp
  test -s opensubdiv/bfr/meshDelta.h
  test -s opensubdiv/bfr/meshDelta.cpp
  c++ -std=c++17 -DOPENSUBDIV_GREGORY_EVAL_TRUE_DERIVATIVES -I. -x c++ -fsyntax-only opensubdiv/bfr/tessellationSession.h
  c++ -std=c++17 -DOPENSUBDIV_GREGORY_EVAL_TRUE_DERIVATIVES -I. -x c++ -fsyntax-only opensubdiv/bfr/tessellationSession.cpp
  c++ -std=c++17 -DOPENSUBDIV_GREGORY_EVAL_TRUE_DERIVATIVES -I. -x c++ -fsyntax-only opensubdiv/bfr/meshDelta.h
  c++ -std=c++17 -DOPENSUBDIV_GREGORY_EVAL_TRUE_DERIVATIVES -I. -x c++ -fsyntax-only opensubdiv/bfr/meshDelta.cpp
fi
if [[ $mode == target ]]; then test -s tests/session_contracts.cpp; fi
cmake -S task_support -B .task-build -DCMAKE_BUILD_TYPE=Release
cmake --build .task-build --parallel 4
if [[ $mode == target ]]; then
  c++ -std=c++17 -DOPENSUBDIV_GREGORY_EVAL_TRUE_DERIVATIVES -I. task_support/driver.c tests/session_contracts.cpp .task-build/libkernel.a -o .task-build/target
test -s examples/session_producer.cpp
test -s examples/mesh_consumer.cpp
test -s opensubdiv/bfr/tessellationSession.cpp
test -s opensubdiv/bfr/meshDelta.cpp
c++ -std=c++17 -DOPENSUBDIV_GREGORY_EVAL_TRUE_DERIVATIVES -I. examples/session_producer.cpp .task-build/libkernel.a -o .task-build/session_producer
c++ -std=c++17 -I. examples/mesh_consumer.cpp opensubdiv/bfr/meshDelta.cpp -o .task-build/mesh_consumer
stream_dir=$(mktemp -d .task-build/stream.XXXXXX)
.task-build/session_producer "$stream_dir"
test -s "$stream_dir/stream.list"
.task-build/mesh_consumer "$stream_dir" "$stream_dir/final.obj"
test -s "$stream_dir/final.obj"
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in dependency retessellation replay; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
