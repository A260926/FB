#!/bin/bash
set -euo pipefail
extra=()
if [[ $(uname) == Darwin ]]; then extra+=(-liconv); fi
cc -std=gnu11 -O1 -I. -Ilibs/libsacd -Ilibs/libcommon -Ilibs/libdstdec -Ilibs/libid3 tests/dsd_window.c task_support/driver.c .task-build/production.a "${extra[@]}" -o .task-build/target
