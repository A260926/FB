#!/bin/sh
set -eu

# Workgroup executor checks for SPIRV-VM.
#
# Needs only the project C sources, a C99 compiler and libm.  No shader SDK,
# Python or GPU is involved: the SPIR-V modules live in
# checks/workgroup_fixtures.h as C word arrays with their readable assembly
# in comments.

mkdir -p build

CC=${CC:-cc}
CFLAGS="-std=c99 -O0 -Iinc -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-unused-variable -Wno-unused-function -Wno-incompatible-pointer-types-discards-qualifiers"

echo "building workgroup checks with $CC"
$CC $CFLAGS \
	checks/workgroup.c \
	src/workgroup.c \
	src/program.c \
	src/types.c \
	src/image.c \
	src/result.c \
	src/state.c \
	src/value.c \
	src/context.c \
	src/opcode_execute.c \
	src/opcode_setup.c \
	src/ext/GLSL450.c \
	-lm -o build/workgroup-check

echo "running workgroup checks"
./build/workgroup-check

echo "running baseline check and the existing colored shader example"
sh checks/baseline.sh

echo "WORKGROUP_ALL_OK"
