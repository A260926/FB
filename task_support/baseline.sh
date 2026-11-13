#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
sh task_support/build.sh
cc -std=c99 -I. task_support/inspect.c liblsmash.a -lm -o .task-build/inspect
.task-build/inspect task_support/source.mp4
cli/remuxer -i task_support/source.mp4 -o .task-build/remux.mp4
.task-build/inspect .task-build/remux.mp4
printf 'L-SMASH real two-track remux: ok\n'
