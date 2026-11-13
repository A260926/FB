#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p .task-build
sh configure > .task-build/configure.log 2>&1
make -j2 lib cli/remuxer > .task-build/make.log 2>&1 || { tail -30 .task-build/make.log; exit 1; }
