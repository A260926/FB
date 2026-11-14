#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
extra=(-std=c99 -O2)
if [[ ${1:-baseline} == target ]]; then extra+=(-DTWOLAME_RD_TESTING); fi
mkdir -p .task-build/obj
rm -f .task-build/obj/*.o .task-build/libtwolame.a
for f in libtwolame/*.c; do
  name=${f##*/}
  cc "${extra[@]}" -Ichecks -Ilibtwolame -c "$f" -o ".task-build/obj/${name%.c}.o"
done
ar rcs .task-build/libtwolame.a .task-build/obj/*.o
