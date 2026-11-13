#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
b=$(mktemp -d "${TMPDIR:-/tmp}/build.XXXXXX")
trap 'rm -rf "$b"' EXIT
mode=${1:-baseline}
case "$mode" in baseline) flag=OFF;; target) flag=ON;; *) exit 64;; esac
cmake -S support -B "$b" -DTASK_TARGET="$flag" -DCMAKE_BUILD_TYPE=Release -DFETCHCONTENT_FULLY_DISCONNECTED=ON
cmake --build "$b" --target task_baseline -j 4
"$b/task_baseline"
if [ "$mode" = target ]; then
 cmake --build "$b" --target task_target -j 4
 "$b/task_target" > "$b/target.stdout"
 cat "$b/target.stdout"
 awk -f support/verify.awk "$b/target.stdout"
fi
