#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
case "$mode" in baseline|target) ;; *) echo 'usage: bash support/run.sh baseline|target' >&2; exit 2;; esac
mkdir -p .build/logs
flag=OFF
if [ "$mode" = target ]; then flag=ON; fi
if ! cmake -S support -B .build/offline -DCMAKE_BUILD_TYPE=Release -DJOIN_TARGET="$flag" >.build/logs/configure.log 2>&1; then
  echo 'configure failed; see .build/logs/configure.log' >&2; exit 1
fi
if ! cmake --build .build/offline --parallel 4 >.build/logs/build.log 2>&1; then
  echo 'build failed; see .build/logs/build.log' >&2; exit 1
fi
.build/offline/baseline
if [ "$mode" = target ]; then
  for group in basis rational ownership; do .build/offline/join_contracts "$group"; done
  mkdir -p .build/artifacts
  .build/offline/join_example .build/artifacts/join.obj
  test -s .build/artifacts/join.obj
  grep -q '^v ' .build/artifacts/join.obj
  grep -q '^f ' .build/artifacts/join.obj
fi
