#!/bin/bash
set -euo pipefail
out=$1
shift
case $(uname -s) in Darwin) dead=-Wl,-dead_strip;; *) dead=-Wl,--gc-sections;; esac
test -s .task-build/objects.list
while IFS= read -r object; do
  test -s "$object"
  set -- "$@" "$object"
done < .task-build/objects.list
${CXX:-c++} -std=c++17 -O1 -w -ffunction-sections -fdata-sections -I. -Isrc -Iextlib/eigen -Itask_support "$@" "$dead" -o "$out"
