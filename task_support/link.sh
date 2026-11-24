#!/bin/bash
set -euo pipefail
lib=.task-build/cmake/src/libFLAC/libFLAC.a
case $(uname -s) in
  Darwin) lib_args=("-Wl,-force_load,$lib") ;;
  *) lib_args=(-Wl,--whole-archive "$lib" -Wl,--no-whole-archive) ;;
esac
cc -std=c99 -O2 -I. -Iinclude -include include/FLAC/stream_encoder_checkpoint.h task_support/driver.c tests/encoder_checkpoint.c "${lib_args[@]}" -lm -o .task-build/target
cc -std=c99 -O2 -I. -Iinclude -include include/FLAC/stream_encoder_checkpoint.h task_support/api_consumer.c "${lib_args[@]}" -lm -o .task-build/api-consumer
.task-build/api-consumer
test -s examples/checkpoint_resume.c
cc -std=c99 -O2 -Iinclude -include include/FLAC/stream_encoder_checkpoint.h examples/checkpoint_resume.c "${lib_args[@]}" -lm -o .task-build/checkpoint-example
rm -rf .task-build/checkpoint-demo
mkdir -p .task-build/checkpoint-demo
.task-build/checkpoint-example save .task-build/checkpoint-demo
test -s .task-build/checkpoint-demo/checkpoint.bin
test -s .task-build/checkpoint-demo/stream.flac
test -s .task-build/checkpoint-demo/suffix.pcm
.task-build/checkpoint-example resume .task-build/checkpoint-demo
test -s .task-build/checkpoint-demo/stream.flac
.task-build/checkpoint-example verify .task-build/checkpoint-demo
test -s .task-build/checkpoint-demo/reference.flac
test -s .task-build/checkpoint-demo/verified.txt
