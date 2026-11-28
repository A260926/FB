#!/bin/bash
set -euo pipefail
if [[ $(uname -s) == Darwin ]]; then
  library_flags=(-Wl,-force_load,.task-build/libmarrow.a)
else
  library_flags=(-Wl,--whole-archive .task-build/libmarrow.a -Wl,--no-whole-archive)
fi
exec bash task_support/compiler.sh "$@" .task-build/authoring.o "${library_flags[@]}" -lm
