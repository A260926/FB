#!/bin/sh
set -eu
mkdir -p build
cc -std=c99 -O0 -Iinc checks/baseline.c src/program.c src/types.c src/image.c src/result.c src/state.c src/value.c src/context.c src/opcode_execute.c src/opcode_setup.c src/ext/GLSL450.c -lm -o build/baseline
./build/baseline
cc -std=c99 -O0 -Iinc example/example.c src/program.c src/types.c src/image.c src/result.c src/state.c src/value.c src/context.c src/opcode_execute.c src/opcode_setup.c src/ext/GLSL450.c -lm -o build/upstream-example
(cd example && ../build/upstream-example)
