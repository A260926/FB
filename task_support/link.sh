#!/bin/bash
set -euo pipefail
unset HFUZZ_THREAD_NO
cc_bin=${CC:-cc}
cxx_bin=${CXX:-c++}
if [[ $(uname -s) == Darwin ]]; then arch=(-D_HF_ARCH_DARWIN); strip_flags=(-Wl,-dead_strip); else arch=(-D_GNU_SOURCE -D_HF_ARCH_LINUX); strip_flags=(-Wl,--gc-sections -ldl -lm); fi
test -s libhfuzz/covmerge.h
test -s libhfuzz/covmerge.c
test -s examples/covmerge_replay.c
"$cc_bin" -std=gnu11 -O1 -I. "${arch[@]}" -include libhfuzz/covmerge.h -c libhfuzz/covmerge.c -o .task-build/covmerge.o
objects=(.task-build/covmerge.o .task-build/instrument.o .task-build/memorycmp.o .task-build/files.o .task-build/util.o .task-build/log.o)
"$cc_bin" -std=gnu11 -I. "${arch[@]}" task_support/api_consumer.c "${objects[@]}" "${strip_flags[@]}" -pthread -o .task-build/api-consumer
.task-build/api-consumer
"$cxx_bin" -std=c++17 -O1 -I. "${arch[@]}" checks/covmerge_smoke.cpp "${objects[@]}" "${strip_flags[@]}" -pthread -o .task-build/covmerge-cpp-smoke
.task-build/covmerge-cpp-smoke
"$cc_bin" -std=gnu11 -I. "${arch[@]}" examples/covmerge_replay.c "${objects[@]}" "${strip_flags[@]}" -pthread -o .task-build/example
mkdir -p .task-build/example-output
rm -f .task-build/example-output/transcript.txt
.task-build/example .task-build/example-output
test -s .task-build/example-output/transcript.txt
bash checks/covmerge_transcript_check.sh .task-build/example-output/transcript.txt
"$cc_bin" -std=gnu11 -I. "${arch[@]}" task_support/driver.c checks/covmerge_tests.c "${objects[@]}" "${strip_flags[@]}" -pthread -o .task-build/target
