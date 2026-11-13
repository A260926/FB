#!/bin/sh
set -eu
mkdir -p build
printf '%s\n' '#define MOJOSHADER_VERSION -1' '#define MOJOSHADER_CHANGESET "fixed-source"' > build/mojoshader_version.h
cc -std=c99 -O0 -I. -Ibuild checks/baseline.c mojoshader.c mojoshader_common.c profiles/mojoshader_profile_common.c profiles/mojoshader_profile_glsl.c profiles/mojoshader_profile_d3d.c profiles/mojoshader_profile_bytecode.c profiles/mojoshader_profile_arb1.c profiles/mojoshader_profile_metal.c profiles/mojoshader_profile_spirv.c profiles/mojoshader_profile_hlsl.c -lm -o build/baseline
./build/baseline
