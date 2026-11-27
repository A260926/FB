#!/bin/sh
# Predicated-instruction translation checks for the glsl120 profile.
#
# This only needs a C99 compiler and the C math library: no GPU, OpenGL,
# Python, or extra dependencies. The C test builds its own legal Shader Model
# 3 words, cross-checks numeric behavior with an independent register model,
# and reports the number of cases and assertions it executed.
set -eu

mkdir -p build
printf '%s\n' '#define MOJOSHADER_VERSION -1' '#define MOJOSHADER_CHANGESET "fixed-source"' > build/mojoshader_version.h

SOURCES="mojoshader.c mojoshader_common.c \
 profiles/mojoshader_profile_common.c profiles/mojoshader_profile_glsl.c \
 profiles/mojoshader_profile_d3d.c profiles/mojoshader_profile_bytecode.c \
 profiles/mojoshader_profile_arb1.c profiles/mojoshader_profile_metal.c \
 profiles/mojoshader_profile_spirv.c profiles/mojoshader_profile_hlsl.c"

echo "== building predicated-instruction tests =="
cc -std=c99 -O0 -Wall -Wextra -I. -Ibuild \
   checks/predication.c $SOURCES -lm -o build/predication

echo "== running predicated-instruction tests =="
./build/predication

# Rerun the pre-existing baseline (plain, non-predicated vertex translation)
# and the predicated sample so both stay green alongside the new coverage.
echo "== rebuilding baseline samples =="
cc -std=c99 -O0 -I. -Ibuild checks/baseline.c $SOURCES -lm -o build/baseline

echo "== rerunning original (non-predicated) baseline =="
./build/baseline

echo "== rerunning predicated baseline sample =="
./build/baseline --predicated

echo "CHECK_PREDICATION_OK"
