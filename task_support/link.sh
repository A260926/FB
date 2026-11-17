#!/bin/bash
set -euo pipefail
flags=(-std=c11 -O1 -I. -Isrc -Itools/authoring -Itools/gltf2marrow -Itools/third_party)
if [[ $(uname -s) == Darwin && $(uname -m) == arm64 ]]; then flags+=(-arch x86_64); fi
cc "${flags[@]}" task_support/driver.c tests/forest_target.c .task-build/libtask.a -lm -o .task-build/target
