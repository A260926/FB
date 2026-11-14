#!/usr/bin/env bash
#
# Builds and runs the preserve-sample-timeline regression tests for dr_flac.h.
#
# The C test in dr_flac_timeline_test.c generates its own CRC-correct native FLAC streams with the auditable generator in
# flac_gen.h (C only, no external encoder), corrupts or deletes individual frames, and verifies sample by sample that holes are
# emitted as exact silence while every validated frame stays at its declared sample position. It also checks the historical
# frame-skipping behavior with the mode disabled. Run task_support/baseline.sh alongside this script.
set -euo pipefail
cd "$(dirname "$0")/.."

mkdir -p .task-build

# The main regression suite.
cc -std=c99 -O2 -Wall -Wextra -I. -Itask_support task_support/dr_flac_timeline_test.c -lm -o .task-build/dr_flac_timeline_test
.task-build/dr_flac_timeline_test

# Sanitizer build to guard the zero-fill and frame handling against bounds and undefined behavior.
cc -std=c99 -O1 -g -fsanitize=address,undefined -I. -Itask_support task_support/dr_flac_timeline_test.c -lm -o .task-build/dr_flac_timeline_test_asan
.task-build/dr_flac_timeline_test_asan

# A build with CRC validation disabled must refuse to enable the mode while remaining otherwise usable.
cc -std=c99 -O2 -Wall -Wextra -Wno-unused-function -I. -Itask_support task_support/nocrc_compile_check.c -o .task-build/nocrc_compile_check
.task-build/nocrc_compile_check

# The header must still compile standalone in the upstream style.
cc -std=c99 -I. -c tests/compilation/dr_flac.c -o .task-build/upstream-compile.o

echo "DRFLAC_TIMELINE_CHECK ok"
