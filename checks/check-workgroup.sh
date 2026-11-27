#!/bin/sh
# Workgroup executor checks (C99). Builds the word-fixture test program with
# the project sources and only the system compiler + libm, then reruns the
# existing baseline and the colored shader example.
set -eu

mkdir -p build

SRCS="src/workgroup.c src/program.c src/types.c src/image.c src/result.c src/state.c src/value.c src/context.c src/opcode_execute.c src/opcode_setup.c src/ext/GLSL450.c"

# strict C99: the new sources must compile warning-free under -Werror
cc -std=c99 -Wall -Wextra -Werror -Iinc -c src/workgroup.c -o build/workgroup.o
cc -std=c99 -Wall -Wextra -Werror -Iinc -c checks/check-workgroup.c -o build/check-workgroup.o

cc -std=c99 -O0 -Iinc checks/check-workgroup.c $SRCS -lm -o build/check-workgroup
./build/check-workgroup

# existing single-state behavior and the colored shader example must keep working
sh checks/baseline.sh
